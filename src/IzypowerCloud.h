// ============================================================
// IzypowerCloud.h — Client de l'API cloud IZYPOWER
// (application.izypowercloud.fr) pour micro-onduleurs
// iP-1000-W2 / iP-2000-W4.
//
// Les modules WiFi WE-P2 des iP sont 100 % orientés cloud :
// ni INDEVOLT RPC (8080) ni SOLARMAN V5 (8899) ne sont exposés
// localement (vérifié sur matériel). Seule voie de lecture des
// données : l'API cloud Izypower. Les données y sont rafraîchies
// toutes les ~3 minutes (pas de temps réel).
//
//   POST /photo_voltaic/api/login
//       corps JSON {"username","password"}, header app-platform: izy
//       → data.token (JWT, header x-tts-access-token ensuite)
//
//   GET  /photo_voltaic/api/powerStations/page?page=1&limit=100
//       → data.records[].stationsId (auto-découverte de la station)
//
//   GET  /photo_voltaic/api/v3/powerStations/info/{id}
//       → power (W), data.{todaySum,monthSum,yearSum,allSum} (kWh),
//         data.{stationName,installedCapacity,lastUpdate,signal,onlineState}
//
//   GET  /photo_voltaic/api/component/{id}?searchTime=YYYY-MM-DD
//       → pvData[] par chaîne : sn, pv (pv1..pv4), pvPower (W),
//         energy (kWh du jour), totalRatePower (W), deviceId.
//       searchTime est optionnel (données du jour) mais sans lui
//       energy vaut null → on extrait la date de data.lastUpdate.
//
// Ce fichier est header-only (tout est inline), dépend uniquement
// d'ArduinoJson. Compatible ESP8266/ESP32.
//
// Utilisation :
//   IzypowerCloud cloud;
//   cloud.begin("email@ex.fr", "mdp", 0);      // stationId = 0 → auto
//   int err = cloud.poll();                    // 0 = OK (cf. IZY_ERR_*)
//   cloud.inverterCount(), cloud.inverter(i), ...
// ============================================================

#ifndef IZYPOWERCLOUD_H
#define IZYPOWERCLOUD_H

#include <Arduino.h>
#ifdef ESP32
  #include <WiFi.h>
  #include <HTTPClient.h>
#else
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
#endif
#include <ArduinoJson.h>

// ============================================================
// LIMITES
// ============================================================
#define IZY_MAX_INVERTERS  4     // nombre max d'onduleurs sur la station
#define IZY_MAX_PV         4     // chaînes PV max par onduleur (iP-2000-W4)
#define IZY_SN_LEN        16
#define IZY_NAME_LEN      64
#define IZY_UPDATE_LEN    32     // "YYYY-MM-DD HH:MM:SS"
#define IZY_DATE_LEN      11     // "YYYY-MM-DD"
#define IZY_TOKEN_LEN    512

// ============================================================
// CODES DE RETOUR DE poll()
// ============================================================
enum {
    IZY_OK             =  0,
    IZY_ERR_LOGIN      = -1,  // échec d'authentification cloud
    IZY_ERR_HTTP       = -2,  // statut HTTP != 200
    IZY_ERR_PARSE      = -3,  // réponse JSON illisible
    IZY_ERR_NO_STATION = -4,  // aucun powerStations trouvé
    IZY_ERR_CONNECT    = -5,  // échec TCP/DNS
    IZY_ERR_TOKEN      = -6,  // 401 malgré ré-authentification
};

// ============================================================
// STRUCTURES DE DONNÉES
// ============================================================
struct IzypowerCloudPv {
    bool   exists;      // chaîne présente physiquement (exists)
    float  powerW;      // puissance DC de la chaîne (W)
    float  energyKWh;   // énergie du jour de la chaîne (kWh)
};

struct IzypowerCloudInverter {
    char   sn[IZY_SN_LEN];   // numéro de série (deviceName == sn)
    uint32_t deviceId;       // identifiant interne cloud
    uint16_t ratedW;         // puissance nominale totale (W)
    uint8_t  pvCount;        // nombre de chaînes retournées
    IzypowerCloudPv pv[IZY_MAX_PV];

    float powerW() const;    // somme des puissances PV (W)
    float todayKWh() const;  // somme des énergies du jour (kWh)
};

inline float IzypowerCloudInverter::powerW() const {
    float s = 0;
    for (uint8_t i = 0; i < pvCount && i < IZY_MAX_PV; i++) s += pv[i].powerW;
    return s;
}
inline float IzypowerCloudInverter::todayKWh() const {
    float s = 0;
    for (uint8_t i = 0; i < pvCount && i < IZY_MAX_PV; i++) s += pv[i].energyKWh;
    return s;
}

// ============================================================
// CLIENT CLOUD
// ============================================================
class IzypowerCloud {
public:
    IzypowerCloud()
        : _hasToken(false), _loginAtMs(0), _cfgStationId(0),
          _httpTimeoutMs(10000),
          _valid(false), _stationId(0), _installedCapacity(0),
          _powerW(0), _todayKWh(0), _monthKWh(0), _yearKWh(0), _totalKWh(0),
          _signal(0), _stationOnline(false), _count(0),
          _updatedMs(0), _lastError(IZY_OK) {
        _email[0] = _password[0] = _token[0] = 0;
        _stationName[0] = _lastUpdate[0] = _searchDate[0] = 0;
        memset(_inverters, 0, sizeof _inverters);
    }

    // stationId = 0 → auto-découverte de la première station du compte.
    void begin(const char* email, const char* password, uint32_t stationId = 0) {
        strncpy(_email, email, sizeof _email - 1);
        strncpy(_password, password, sizeof _password - 1);
        _cfgStationId = stationId;
        _stationId    = stationId;
    }

    // Récupère station + onduleurs. Retourne IZY_OK ou une erreur IZY_ERR_*.
    int poll() {
        _lastError = IZY_OK;

        if (!_hasToken || (millis() - _loginAtMs > (22UL * 3600UL * 1000UL))) {
            if (login() != IZY_OK) { _lastError = IZY_ERR_LOGIN; return _lastError; }
        }

        if (_stationId == 0) {
            int r = fetchStations();
            if (r != IZY_OK) return r;
            if (_stationId == 0) { _lastError = IZY_ERR_NO_STATION; return _lastError; }
        }

        int rInfo = fetchStationInfo();
        int rComp = fetchComponent();
        if (rInfo != IZY_OK) return rInfo;
        if (rComp != IZY_OK) return rComp;

        _valid = true;
        _updatedMs = millis();
        return IZY_OK;
    }

    // ---- Station -------------------------------------------------
    bool        valid() const         { return _valid; }
    uint32_t    stationId() const     { return _stationId; }
    const char* stationName() const   { return _stationName; }
    uint16_t    installedCapacityW() const { return _installedCapacity; }
    float       powerW() const        { return _powerW; }        // W, production PV station
    float       todayKWh() const      { return _todayKWh; }
    float       monthKWh() const      { return _monthKWh; }
    float       yearKWh() const       { return _yearKWh; }
    float       totalKWh() const      { return _totalKWh; }
    const char* lastUpdate() const    { return _lastUpdate; }    // "YYYY-MM-DD HH:MM:SS"
    int16_t     signal() const        { return _signal; }        // RSSI (dBm), meilleur device
    bool        stationOnline() const { return _stationOnline; }

    // ---- Onduleurs (ordre de découverte du cloud) ---------------
    uint8_t  inverterCount() const { return _count; }
    const IzypowerCloudInverter* inverter(uint8_t i) const {
        return (i < _count) ? &_inverters[i] : nullptr;
    }
    const IzypowerCloudInverter* find(const char* sn) const {
        if (!sn) return nullptr;
        for (uint8_t i = 0; i < _count; i++)
            if (strcmp(_inverters[i].sn, sn) == 0) return &_inverters[i];
        return nullptr;
    }

    uint32_t updatedMs() const { return _updatedMs; }
    int      lastError() const { return _lastError; }
    const char* lastErrorText() const {
        switch (_lastError) {
            case IZY_OK:             return "OK";
            case IZY_ERR_LOGIN:      return "Échec de connexion cloud (identifiants ?)";
            case IZY_ERR_HTTP:       return "Réponse HTTP invalide";
            case IZY_ERR_PARSE:      return "JSON illisible";
            case IZY_ERR_NO_STATION: return "Aucune centrale trouvée sur le compte";
            case IZY_ERR_CONNECT:    return "Impossible de joindre le cloud";
            case IZY_ERR_TOKEN:      return "Token refusé (401) malgré re-login";
            default:                 return "Erreur inconnue";
        }
    }

private:
    static const char* BASE_URL;

    char     _email[64];
    char     _password[64];
    char     _token[IZY_TOKEN_LEN];
    bool     _hasToken;
    uint32_t _loginAtMs;
    uint32_t _cfgStationId;
    uint16_t _httpTimeoutMs;
    WiFiClient _client;   // client TCP réutilisé (HTTPClient::begin(WiFiClient,url))

    bool     _valid;
    uint32_t _stationId;
    char     _stationName[IZY_NAME_LEN];
    uint16_t _installedCapacity;
    float    _powerW, _todayKWh, _monthKWh, _yearKWh, _totalKWh;
    char     _lastUpdate[IZY_UPDATE_LEN];
    char     _searchDate[IZY_DATE_LEN];   // date extraite de lastUpdate
    int16_t  _signal;
    bool     _stationOnline;
    uint8_t  _count;
    IzypowerCloudInverter _inverters[IZY_MAX_INVERTERS];
    uint32_t _updatedMs;
    int      _lastError;

    // ---- HTTP ----------------------------------------------------
    int httpGet(const char* url, String& body) {
        HTTPClient http;
        http.setTimeout(_httpTimeoutMs);
        if (!http.begin(_client, url)) { _lastError = IZY_ERR_CONNECT; return -1; }
        http.addHeader("app-platform", "izy");
        http.addHeader("Accept-Language", "fr-FR,fr;q=0.9");
        if (_hasToken) http.addHeader("x-tts-access-token", _token);
        int code = http.GET();
        if (code > 0) body = http.getString();
        else _lastError = IZY_ERR_CONNECT;
        http.end();
        return code;
    }

    // GET authentifié : re-login automatique sur 401/403 (1 tentative).
    int httpJsonGet(const char* url, JsonDocument& doc) {
        for (int t = 0; t < 2; t++) {
            String body;
            int code = httpGet(url, body);
            if (code == 401 || code == 403) {
                if (t == 0 && login() == IZY_OK) continue;
                _lastError = IZY_ERR_TOKEN;
                return _lastError;
            }
            if (code < 0) return IZY_ERR_CONNECT;
            if (code != 200) { _lastError = IZY_ERR_HTTP; return _lastError; }
            if (deserializeJson(doc, body)) { _lastError = IZY_ERR_PARSE; return _lastError; }
            return IZY_OK;
        }
        return _lastError;
    }

    int login() {
        String url = String(BASE_URL) + "/login";
        JsonDocument payload;
        payload["username"] = _email;
        payload["password"] = _password;
        String body;
        serializeJson(payload, body);

        HTTPClient http;
        http.setTimeout(_httpTimeoutMs);
        if (!http.begin(_client, url)) { _lastError = IZY_ERR_CONNECT; return _lastError; }
        http.addHeader("Content-Type", "application/json");
        http.addHeader("app-platform", "izy");
        http.addHeader("Accept-Language", "fr-FR,fr;q=0.9");
        int code = http.POST(body);
        String resp;
        if (code > 0) resp = http.getString();
        http.end();

        if (code != 200) { _lastError = IZY_ERR_HTTP; return _lastError; }

        JsonDocument doc;
        if (deserializeJson(doc, resp)) { _lastError = IZY_ERR_PARSE; return _lastError; }
        const char* token = doc["data"]["token"] | doc["token"] | "";
        if (!token[0]) { _lastError = IZY_ERR_PARSE; return _lastError; }
        strncpy(_token, token, sizeof _token - 1);
        _hasToken  = true;
        _loginAtMs = millis();
        return IZY_OK;
    }

    // Auto-découverte : première station du compte.
    int fetchStations() {
        String url = String(BASE_URL) + "/powerStations/page?page=1&limit=100";
        JsonDocument doc;
        int r = httpJsonGet(url.c_str(), doc);
        if (r != IZY_OK) return r;

        JsonArray records = doc["data"]["records"].as<JsonArray>();
        if (records.isNull() || records.size() == 0) {
            _lastError = IZY_ERR_NO_STATION;
            return _lastError;
        }
        _stationId = records[0]["stationsId"] | 0UL;
        return IZY_OK;
    }

    // Informations station : puissances + énergies + date + signal.
    int fetchStationInfo() {
        String url = String(BASE_URL) + "/v3/powerStations/info/" + String(_stationId);
        JsonDocument doc;
        int r = httpJsonGet(url.c_str(), doc);
        if (r != IZY_OK) return r;

        JsonVariantConst data = doc["data"];
        _powerW          = (float)(doc["power"] | data["power"] | 0.0f);
        _todayKWh        = data["todaySum"]  | 0.0f;
        _monthKWh        = data["monthSum"]  | 0.0f;
        _yearKWh         = data["yearSum"]   | 0.0f;
        _totalKWh        = data["allSum"]    | 0.0f;
        _signal          = data["signal"]    | 0;
        _stationOnline   = ((data["onlineState"] | 0) == 1);

        const char* name  = data["stationName"] | "";
        const char* upd   = data["lastUpdate"] | "";
        uint32_t    cap   = data["installedCapacity"] | 0U;
        uint32_t    sid   = data["stationsId"] | _stationId;

        if (sid) _stationId = sid;
        if (cap && cap <= 65535) _installedCapacity = (uint16_t)cap;
        strncpy(_stationName, name, sizeof _stationName - 1);
        strncpy(_lastUpdate, upd, sizeof _lastUpdate - 1);

        // La date du jour = les 10 premiers caractères de lastUpdate.
        if (strlen(_lastUpdate) >= 10) {
            memcpy(_searchDate, _lastUpdate, 10);
            _searchDate[10] = 0;
        }
        return IZY_OK;
    }

    // Chaînes PV : regroupe les pvData par numéro de série d'onduleur.
    int fetchComponent() {
        String url = String(BASE_URL) + "/component/" + String(_stationId);
        if (_searchDate[0]) url += String("?searchTime=") + _searchDate;

        JsonDocument doc;
        int r = httpJsonGet(url.c_str(), doc);
        if (r != IZY_OK) return r;

        _count = 0;
        JsonArray pvs = doc["pvData"].as<JsonArray>();
        if (pvs.isNull()) return IZY_OK;   // pas de pvData → onduleurs inconnus

        for (JsonVariantConst p : pvs) {
            const char* sn = p["sn"] | "";
            if (!sn[0]) continue;

            IzypowerCloudInverter* inv = _findOrAdd(sn);
            if (!inv) break;   // tableau plein

            // NB : dans la réponse réelle, deviceId est une STRING
            // ("12345") et totalRatePower un FLOAT (2000.0) → conversions
            // explicites (ArduinoJson v7 ne fait pas string→int ni float→int).
            if (inv->deviceId == 0) {
                const char* did = p["deviceId"] | "";
                if (did[0]) inv->deviceId = (uint32_t)strtoul(did, nullptr, 10);
            }
            if (inv->ratedW == 0) {
                float trp = p["totalRatePower"] | 0.0f;
                if (trp > 0) inv->ratedW = (uint16_t)trp;
            }

            int idx = pvIndex(p["pv"] | "");
            if (idx < 0 || idx >= IZY_MAX_PV) continue;
            if (idx + 1 > inv->pvCount) inv->pvCount = idx + 1;

            IzypowerCloudPv& pv = inv->pv[idx];
            pv.exists   = (bool)(p["exists"] | true);
            pv.powerW   = p["pvPower"] | 0.0f;
            pv.energyKWh = p["energy"] | 0.0f;
        }
        return IZY_OK;
    }

    IzypowerCloudInverter* _findOrAdd(const char* sn) {
        for (uint8_t i = 0; i < _count; i++)
            if (strcmp(_inverters[i].sn, sn) == 0) return &_inverters[i];
        if (_count >= IZY_MAX_INVERTERS) return nullptr;
        IzypowerCloudInverter& inv = _inverters[_count++];
        inv.sn[0] = 0;
        strncpy(inv.sn, sn, sizeof inv.sn - 1);
        inv.deviceId = 0;
        inv.ratedW   = 0;
        inv.pvCount  = 0;
        memset(inv.pv, 0, sizeof inv.pv);
        return &inv;
    }

    // "pv1" → 0, "pv2" → 1… ; -1 si non reconnu.
    static int pvIndex(const char* pv) {
        if (!pv || strncmp(pv, "pv", 2) != 0) return -1;
        return atoi(pv + 2) - 1;
    }
};

const char* IzypowerCloud::BASE_URL =
    "http://application.izypowercloud.fr/photo_voltaic/api";

#endif  // IZYPOWERCLOUD_H
