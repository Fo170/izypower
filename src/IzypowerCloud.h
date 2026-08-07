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
//   GET  /photo_voltaic/api/device/page?powerId={id}&deviceType=all&page=1&limit=100
//       → data.records[] : sn, deviceType, device (modèle "VMP2"/"VMP4"/"Meter"),
//         pvNum (0 = Smart Meter), mac, softwareVersion, deviceId,
//         onlineState, rssi, dataDtos[] (paires key/value). Best-effort.
//
// Ce fichier est header-only (tout est inline), dépend uniquement
// d'ArduinoJson. Compatible ESP8266/ESP32.
//
// Utilisation (accès en lecture uniquement, via getters) :
//   IzypowerCloud cloud;
//   cloud.begin("email@ex.fr", "mdp", 0);      // stationId = 0 → auto
//   int err = cloud.poll();                    // 0 = OK (cf. IZY_ERR_*)
//   cloud.productionW(), cloud.gridImportW(), cloud.houseConsumptionW(), ...
//   cloud.inverterCount(), cloud.inverter(i), cloud.find("SN"), ...
//   cloud.deviceCount(), cloud.device(i), cloud.findDevice("SN"),
//   cloud.findDevice("SN")->dtoValue("v_ac"), ->rssiDbm(), ->macAddress(), ...
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
#define IZY_MAX_DEVICES   8      // appareils device/page max (Smart Meter, batterie, EV…)
#define IZY_MAX_DTOS      8      // dataDtos max par appareil
#define IZY_DTO_KEY_LEN  24
#define IZY_DTO_VAL_LEN  40
#define IZY_MAC_LEN      24
#define IZY_SW_LEN       40

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
// Chaîne PV d'un onduleur.
struct IzypowerCloudPv {
    bool   connected() const  { return _connected; }   // chaîne présente physiquement
    float  powerW() const     { return _powerW; }      // puissance DC (W)
    float  energyKwh() const  { return _energyKwh; }   // énergie du jour (kWh)
private:
    bool   _connected = false;
    float  _powerW = 0;
    float  _energyKwh = 0;
    friend class IzypowerCloud;
};

// Onduleur (chaînes PV issues de component/).
struct IzypowerCloudInverter {
    const char* serialNumber() const  { return _serialNumber; }
    uint32_t    deviceId() const      { return _deviceId; }
    uint16_t    ratedPowerW() const   { return _ratedPowerW; }
    uint8_t     pvCount() const       { return _pvCount; }
    float       productionW() const;        // somme des puissances PV (W)
    float       productionKwhToday() const; // somme des énergies du jour (kWh)

    const IzypowerCloudPv* pv(uint8_t i) const { return (i < _pvCount) ? &_pv[i] : nullptr; }
    bool  pvConnected(uint8_t i) const { return pv(i) ? pv(i)->connected() : false; }
    float pvPowerW(uint8_t i) const    { return pv(i) ? pv(i)->powerW() : 0; }
    float pvEnergyKwh(uint8_t i) const { return pv(i) ? pv(i)->energyKwh() : 0; }
private:
    char     _serialNumber[IZY_SN_LEN];
    uint32_t _deviceId;
    uint16_t _ratedPowerW;
    uint8_t  _pvCount;
    IzypowerCloudPv _pv[IZY_MAX_PV];
    friend class IzypowerCloud;
};

inline float IzypowerCloudInverter::productionW() const {
    float sum = 0;
    for (uint8_t i = 0; i < _pvCount && i < IZY_MAX_PV; i++) sum += _pv[i].powerW();
    return sum;
}
inline float IzypowerCloudInverter::productionKwhToday() const {
    float sum = 0;
    for (uint8_t i = 0; i < _pvCount && i < IZY_MAX_PV; i++) sum += _pv[i].energyKwh();
    return sum;
}

// dataDto (device/page) : paire clé → valeur.
struct IzypowerCloudDto {
    const char* key() const   { return _key; }
    const char* value() const { return _value; }
private:
    char _key[IZY_DTO_KEY_LEN] = {0};
    char _value[IZY_DTO_VAL_LEN] = {0};
    friend class IzypowerCloud;
};

// Appareil non-PV (device/page) : Smart Meter / Passerelle IA, batterie, EV…
struct IzypowerCloudDevice {
    const char* serialNumber() const { return _serialNumber; }
    const char* type() const         { return _type; }
    // Modèle matériel du cloud (device/page) : "VMP2", "VMP4", "Meter"…
    const char* model() const        { return _model; }
    // Nombre de chaînes PV exposé par le cloud (0 pour Meter, 2/4 pour VMP2/VMP4).
    uint8_t     pvNum() const        { return _pvNum; }
    const char* macAddress() const   { return _macAddress; }
    const char* firmware() const     { return _firmware; }
    uint32_t    deviceId() const     { return _deviceId; }
    bool        online() const       { return _online; }
    int16_t     rssiDbm() const      { return _rssiDbm; }
    uint8_t     dtoCount() const     { return _dtoCount; }
    const IzypowerCloudDto* dto(uint8_t i) const {
        return (i < _dtoCount) ? &_dto[i] : nullptr;
    }
    // Valeur d'une dataDto ("" si absente). Ex. "v_ac" → "237.2V".
    const char* dtoValue(const char* key) const {
        if (!key) return "";
        for (uint8_t i = 0; i < _dtoCount; i++)
            if (strcmp(_dto[i].key(), key) == 0) return _dto[i].value();
        return "";
    }
private:
    char     _serialNumber[IZY_SN_LEN] = {0};
    char     _type[16] = {0};
    char     _model[16] = {0};
    uint8_t  _pvNum = 0;
    char     _macAddress[IZY_MAC_LEN] = {0};
    char     _firmware[IZY_SW_LEN] = {0};
    uint32_t _deviceId = 0;
    bool     _online = false;
    int16_t  _rssiDbm = 0;
    uint8_t  _dtoCount = 0;
    IzypowerCloudDto _dto[IZY_MAX_DTOS];
    friend class IzypowerCloud;
};

// ============================================================
// CLIENT CLOUD
// ============================================================
class IzypowerCloud {
public:
    IzypowerCloud()
        : _hasToken(false), _loginAtMs(0), _cfgStationId(0),
          _httpTimeoutMs(10000),
          _valid(false), _stationId(0), _installedCapacity(0),
          _productionW(0), _todayKwh(0), _monthKwh(0), _yearKwh(0), _totalKwh(0),
          _signalDbm(0), _stationOnline(false), _count(0),
          _deviceCount(0), _gridImportW(0), _houseConsumptionW(0), _evChargeW(0),
          _updatedMs(0), _lastError(IZY_OK) {
        _email[0] = _password[0] = _token[0] = 0;
        _stationName[0] = _lastUpdate[0] = _searchDate[0] = 0;
        memset(_inverters, 0, sizeof _inverters);
        memset(_devices, 0, sizeof _devices);
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

        fetchDevices();   // métrologie (Smart Meter) — best-effort, non fatal

        _valid = true;
        _updatedMs = millis();
        return IZY_OK;
    }

    // ---- Station -------------------------------------------------
    bool        valid() const             { return _valid; }
    uint32_t    stationId() const         { return _stationId; }
    const char* stationName() const       { return _stationName; }
    uint16_t    installedCapacityW() const{ return _installedCapacity; }
    float       productionW() const       { return _productionW; }        // W, production PV station
    float       productionKwhToday() const{ return _todayKwh; }
    float       productionKwhMonth() const{ return _monthKwh; }
    float       productionKwhYear() const { return _yearKwh; }
    float       productionKwhTotal() const{ return _totalKwh; }
    const char* lastUpdate() const        { return _lastUpdate; }         // "YYYY-MM-DD HH:MM:SS"
    int16_t     signalDbm() const         { return _signalDbm; }          // RSSI (dBm), meilleur device
    bool        stationOnline() const     { return _stationOnline; }
    int16_t     gridImportW() const       { return _gridImportW; }        // W : import (+) / injection (-) réseau
    int16_t     houseConsumptionW() const { return _houseConsumptionW; }  // W : consommation du foyer (Smart Meter)
    int16_t     evChargeW() const         { return _evChargeW; }          // W : puissance de la borne EV

    // ---- Onduleurs (ordre de découverte du cloud) ---------------
    uint8_t  inverterCount() const { return _count; }
    const IzypowerCloudInverter* inverter(uint8_t i) const {
        return (i < _count) ? &_inverters[i] : nullptr;
    }
    const IzypowerCloudInverter* find(const char* serialNumber) const {
        if (!serialNumber) return nullptr;
        for (uint8_t i = 0; i < _count; i++)
            if (strcmp(_inverters[i].serialNumber(), serialNumber) == 0) return &_inverters[i];
        return nullptr;
    }

    // ---- Appareils (device/page) : Smart Meter / Passerelle IA, batterie, EV… ----
    uint8_t  deviceCount() const { return _deviceCount; }
    const IzypowerCloudDevice* device(uint8_t i) const {
        return (i < _deviceCount) ? &_devices[i] : nullptr;
    }
    const IzypowerCloudDevice* findDevice(const char* serialNumber) const {
        if (!serialNumber) return nullptr;
        for (uint8_t i = 0; i < _deviceCount; i++)
            if (strcmp(_devices[i].serialNumber(), serialNumber) == 0) return &_devices[i];
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
    float    _productionW, _todayKwh, _monthKwh, _yearKwh, _totalKwh;
    char     _lastUpdate[IZY_UPDATE_LEN];
    char     _searchDate[IZY_DATE_LEN];   // date extraite de lastUpdate
    int16_t  _signalDbm;
    bool     _stationOnline;
    uint8_t  _count;
    IzypowerCloudInverter _inverters[IZY_MAX_INVERTERS];
    uint8_t  _deviceCount;
    IzypowerCloudDevice _devices[IZY_MAX_DEVICES];
    int16_t  _gridImportW, _houseConsumptionW, _evChargeW;
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
        _productionW      = (float)(doc["power"] | data["power"] | 0.0f);
        _todayKwh         = data["todaySum"]  | 0.0f;
        _monthKwh         = data["monthSum"]  | 0.0f;
        _yearKwh          = data["yearSum"]   | 0.0f;
        _totalKwh         = data["allSum"]    | 0.0f;
        _signalDbm        = data["signal"]    | 0;
        _stationOnline    = ((data["onlineState"] | 0) == 1);
        // Métrologie du Smart Meter, en top-level de la réponse v3/info.
        _gridImportW      = doc["grid_power"]  | 0;
        _houseConsumptionW = doc["consumption"] | 0;
        _evChargeW        = doc["ev_power"]    | 0;

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

            IzypowerCloudInverter* inverter = _findOrAdd(sn);
            if (!inverter) break;   // tableau plein

            // NB : dans la réponse réelle, deviceId est une STRING
            // ("12345") et totalRatePower un FLOAT (2000.0) → conversions
            // explicites (ArduinoJson v7 ne fait pas string→int ni float→int).
            if (inverter->_deviceId == 0) {
                const char* did = p["deviceId"] | "";
                if (did[0]) inverter->_deviceId = (uint32_t)strtoul(did, nullptr, 10);
            }
            if (inverter->_ratedPowerW == 0) {
                float trp = p["totalRatePower"] | 0.0f;
                if (trp > 0) inverter->_ratedPowerW = (uint16_t)trp;
            }

            int idx = pvIndex(p["pv"] | "");
            if (idx < 0 || idx >= IZY_MAX_PV) continue;
            if (idx + 1 > inverter->_pvCount) inverter->_pvCount = idx + 1;

            IzypowerCloudPv& chain = inverter->_pv[idx];
            chain._connected = (bool)(p["exists"] | true);
            chain._powerW    = p["pvPower"] | 0.0f;
            chain._energyKwh = p["energy"] | 0.0f;
        }
        return IZY_OK;
    }

    // Métrologie : device/page (Smart Meter / Passerelle IA, batterie, EV…).
    // Best-effort : une erreur ici ne bloque pas la lecture PV (non fatal).
    int fetchDevices() {
        _deviceCount = 0;
        String url = String(BASE_URL) + "/device/page?powerId=" + String(_stationId)
                     + "&deviceType=all&page=1&limit=100";
        JsonDocument doc;
        int r = httpJsonGet(url.c_str(), doc);
        if (r != IZY_OK) return r;

        JsonArray recs = doc["data"]["records"].as<JsonArray>();
        if (recs.isNull()) return IZY_OK;

        for (JsonVariantConst rec : recs) {
            if (_deviceCount >= IZY_MAX_DEVICES) break;
            IzypowerCloudDevice& device = _devices[_deviceCount++];
            memset(&device, 0, sizeof device);

            const char* sn = rec["sn"] | "";
            if (!sn[0]) sn = rec["deviceName"] | "";
            strncpy(device._serialNumber, sn, sizeof device._serialNumber - 1);
            strncpy(device._type, rec["deviceType"] | "", sizeof device._type - 1);
            strncpy(device._model, rec["device"] | "", sizeof device._model - 1);
            device._pvNum     = rec["pvNum"] | 0;
            strncpy(device._macAddress, rec["mac"] | "", sizeof device._macAddress - 1);
            strncpy(device._firmware, rec["softwareVersion"] | "", sizeof device._firmware - 1);
            device._deviceId = rec["deviceId"] | 0UL;
            device._online   = ((rec["onlineState"] | 0) == 1);
            device._rssiDbm  = rec["rssi"] | 0;

            // dataDtos : paires clé → valeur, valeur souvent une chaîne avec
            // unité ("237.2V") mais parfois nombre ou tableau (alarm: []).
            JsonArrayConst dtos = rec["dataDtos"].as<JsonArrayConst>();
            for (JsonVariantConst d : dtos) {
                if (device._dtoCount >= IZY_MAX_DTOS) break;
                const char* key = d["key"] | "";
                if (!key[0]) continue;
                IzypowerCloudDto& out = device._dto[device._dtoCount];
                strncpy(out._key, key, sizeof out._key - 1);

                JsonVariantConst val = d["value"];
                const char* sv = val | "";
                if (sv[0]) {
                    strncpy(out._value, sv, sizeof out._value - 1);
                } else if (!val.isNull()) {
                    String v;
                    serializeJson(val, v);
                    strncpy(out._value, v.c_str(), sizeof out._value - 1);
                }
                device._dtoCount++;
            }
        }
        return IZY_OK;
    }

    IzypowerCloudInverter* _findOrAdd(const char* serialNumber) {
        for (uint8_t i = 0; i < _count; i++)
            if (strcmp(_inverters[i]._serialNumber, serialNumber) == 0) return &_inverters[i];
        if (_count >= IZY_MAX_INVERTERS) return nullptr;
        IzypowerCloudInverter& inverter = _inverters[_count++];
        inverter._serialNumber[0] = 0;
        strncpy(inverter._serialNumber, serialNumber, sizeof inverter._serialNumber - 1);
        inverter._deviceId    = 0;
        inverter._ratedPowerW = 0;
        inverter._pvCount     = 0;
        memset(inverter._pv, 0, sizeof inverter._pv);
        return &inverter;
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
