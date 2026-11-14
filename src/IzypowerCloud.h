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
//   HISTORIQUE (courbes) — mêmes endpoints que l'application mobile :
//
//   GET  /photo_voltaic/api/report/v2/powerStations/data/{id}
//        ?timeType=day|month|year|all&dataFlag=energy&searchTime=YYYY-MM-DD[&showSoc=true]
//       → data.data[] = séries {key, name, unit, lineType,
//         data:[{time:"YYYY-MM-DD HH:MM:SS", val:nombre}]}
//
//   GET  /photo_voltaic/api/report/device/data/{sn}
//        ?searchTime=YYYY-MM-DD&timeType=…&dataFlag=<signaux,csV>
//       → data[] = séries (même forme)
//
//   GET  /photo_voltaic/api/report/izy/device/data/{sn}
//        ?searchTime=YYYY-MM-DD&timeType=…&queryType=pv
//       → data[] = séries PV de l'onduleur (même forme)
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
//
//   // Historique (courbes) — remplace le buffer des séries à chaque appel
//   cloud.fetchStationHistory(IZY_RANGE_DAY);            // toute la centrale
//   cloud.fetchPvHistory("55180000F6CF", IZY_RANGE_DAY); // un onduleur
//   cloud.historySeriesCount(), cloud.historySeries(i),
//   cloud.findSeries("production"), ->point(n)->epoch() / ->value()
//
//   // Valeur à un instant quelconque (interpolation linéaire)
//   bool ok;
//   float w = cloud.historyValueAt("production",
//                                  IzypowerCloud::toCloudEpoch("2026-11-04 14:30"), &ok);
// ============================================================

#ifndef IZYPOWERCLOUD_H
#define IZYPOWERCLOUD_H


// Version de la librairie (source UNIQUE — agrégée par liste_lib.h)
#define __IZYPOWERCLOUD_H_VERSION__ "0.5.0"
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

// ---- Historique -------------------------------------------------
// Nombre de séries conservées et de points par série. IZY_MAX_POINTS est
// surchargeable à la compilation :  build_flags = -DIZY_MAX_POINTS=288
// Au-delà, les points sont décimés (pas régulier, dernier point conservé).
// IZY_MAX_SERIES = 8 par défaut : l'historique PV d'un onduleur en mode "day"
// renvoie 8 séries (pv1..pv4 × tension/courant).
#ifndef IZY_MAX_SERIES
  #define IZY_MAX_SERIES  8
#endif
#ifndef IZY_MAX_POINTS
  #define IZY_MAX_POINTS 96
#endif
#define IZY_SERIES_KEY_LEN   16
#define IZY_SERIES_NAME_LEN  32
#define IZY_SERIES_UNIT_LEN  12
#define IZY_LINE_TYPE_LEN    10

// Fuseau : les `time` des points d'historique et le `searchTime` attendu sont
// en heure LOCALE DU SITE (vérifié : dernier point 22:54 pour 22:56 CET réels,
// production de 07:12 à la tombée du jour). En revanche `lastUpdate` est en
// heure SERVEUR (UTC+8) — il ne sert donc PAS à dériver la date de recherche.
// Décalage local↔UTC, utilisé uniquement par epochUtc() : 1 en hiver (CET),
// 2 en été (CEST). Surchargeable : -DIZY_LOCAL_UTC_OFFSET_HOURS=2
#ifndef IZY_LOCAL_UTC_OFFSET_HOURS
  #define IZY_LOCAL_UTC_OFFSET_HOURS 1
#endif

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
// GRANULARITÉ D'HISTORIQUE (paramètre timeType de l'API)
// ============================================================
enum IzyHistoryRange {
    IZY_RANGE_DAY   = 0,   // "day"   : points de la journée
    IZY_RANGE_MONTH = 1,   // "month" : jours du mois
    IZY_RANGE_YEAR  = 2,   // "year"  : mois de l'année
    IZY_RANGE_ALL   = 3,   // "all"   : tout l'historique
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
// HISTORIQUE : point et série
// ============================================================
// Un point de courbe. `epoch` est l'horodatage du champ "time"
// ("YYYY-MM-DD HH:MM:SS", ou "YYYY-MM"/"YYYY" selon la granularité) converti
// en secondes Unix ; 0 si non analysable.
//
// ATTENTION : l'API renvoie l'heure LOCALE DU SITE (vérifié : dernier point
// 22:54 pour 22:56 CET réels). `epoch()` est donc cette heure locale
// « comme si » elle était UTC. Pour l'heure UTC réelle, utiliser
// `epochUtc()` (retire le décalage local, 1 h en hiver, 2 h en été).
struct IzypowerCloudPoint {
    uint32_t epoch() const { return _epoch; }
    uint32_t epochUtc(int offsetHours = IZY_LOCAL_UTC_OFFSET_HOURS) const {
        const uint32_t off = (uint32_t)(offsetHours > 0 ? offsetHours : 0) * 3600UL;
        return (_epoch > off) ? (_epoch - off) : 0;
    }
    float    value() const { return _value; }
private:
    uint32_t _epoch = 0;
    float    _value = 0;
    friend class IzypowerCloud;
};

// Une série (production, batterie, EV, signal…).
struct IzypowerCloudSeries {
    const char* key() const       { return _key; }        // "production", "battery"…
    const char* name() const      { return _name; }       // libellé renvoyé par le cloud
    const char* unit() const      { return _unit; }       // "kWh", "W"…
    const char* lineType() const  { return _lineType; }   // "line" ou "bar"

    uint16_t pointCount() const   { return _pointCount; } // points conservés
    uint16_t pointTotal() const   { return _pointTotal; } // points reçus du cloud
    bool     decimated() const    { return _pointTotal > _pointCount; }

    const IzypowerCloudPoint* point(uint16_t i) const {
        return (i < _pointCount) ? &_points[i] : nullptr;
    }

    // Valeur interpolée LINÉAIREMENT à l'instant demandé (heure du cloud).
    //   *found = true  → l'instant est couvert par la série (bornes incluses)
    //   *found = false → hors plage : renvoie la valeur du point extrême
    //                    (aucune extrapolation), ou 0 si la série est vide.
    // Les points sont supposés triés par ordre chronologique (c'est le cas).
    float valueAt(uint32_t epochCloud, bool* found = nullptr) const {
        if (found) *found = false;
        if (_pointCount == 0) return 0.0f;
        if (_pointCount == 1) {
            if (found) *found = (epochCloud == _points[0].epoch());
            return _points[0].value();
        }

        const uint32_t t = epochCloud;
        if (t <= _points[0].epoch()) {          // au début ou avant
            if (found) *found = (t == _points[0].epoch());
            return _points[0].value();
        }

        for (uint16_t i = 1; i < _pointCount; i++) {
            const uint32_t t1 = _points[i].epoch();
            if (t == t1) {
                if (found) *found = true;
                return _points[i].value();
            }
            if (t < t1) {
                const uint32_t t0 = _points[i - 1].epoch();
                const float v0 = _points[i - 1].value();
                const float v1 = _points[i].value();
                // Horodatage inexploitable ou segment dégénéré : pas d'interpolation.
                if (t1 <= t0 || t0 == 0) return v1;
                const float k = (float)(t - t0) / (float)(t1 - t0);
                if (found) *found = true;
                return v0 + (v1 - v0) * k;
            }
        }
        return _points[_pointCount - 1].value();                  // après la fin
    }

private:
    char     _key[IZY_SERIES_KEY_LEN] = {0};
    char     _name[IZY_SERIES_NAME_LEN] = {0};
    char     _unit[IZY_SERIES_UNIT_LEN] = {0};
    char     _lineType[IZY_LINE_TYPE_LEN] = {0};
    uint16_t _pointCount = 0;
    uint16_t _pointTotal = 0;
    IzypowerCloudPoint _points[IZY_MAX_POINTS];
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
          _seriesCount(0), _historyRange(IZY_RANGE_DAY), _historyUpdatedMs(0),
          _updatedMs(0), _lastError(IZY_OK) {
        _email[0] = _password[0] = _token[0] = 0;
        _stationName[0] = _lastUpdate[0] = _searchDate[0] = 0;
        _historySearchTime[0] = 0;
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

    // ---- Historique (courbes) ------------------------------------
    // Chaque appel REMPLACE le contenu du buffer de séries.
    // searchTime = nullptr → date du dernier lastUpdate (ou aujourd'hui côté
    // serveur si aucune date connue). Format "YYYY-MM-DD".
    int fetchStationHistory(IzyHistoryRange range = IZY_RANGE_DAY,
                            const char* searchTime = nullptr,
                            bool showSoc = true);
    int fetchDeviceHistory(const char* deviceSn,
                           IzyHistoryRange range = IZY_RANGE_DAY,
                           const char* searchTime = nullptr,
                           const char* dataFlag = "energy");
    int fetchPvHistory(const char* deviceSn,
                       IzyHistoryRange range = IZY_RANGE_DAY,
                       const char* searchTime = nullptr);

    uint8_t historySeriesCount() const { return _seriesCount; }
    const IzypowerCloudSeries* historySeries(uint8_t i) const {
        return (i < _seriesCount) ? &_series[i] : nullptr;
    }
    // Série par clé ("production", "battery", "ev"…), nullptr si absente.
    const IzypowerCloudSeries* findSeries(const char* key) const {
        if (!key) return nullptr;
        for (uint8_t i = 0; i < _seriesCount; i++)
            if (strcmp(_series[i].key(), key) == 0) return &_series[i];
        return nullptr;
    }
    // Rend la memoire du buffer de series (a appeler apres avoir exploite
    // le resultat ; le prochain fetchStationHistory()/fetchPvHistory()/
    // fetchDeviceHistory() le realloue).
    void releaseHistory() {
        if (_series) { free(_series); _series = nullptr; }
        _seriesCount = 0;
    }

    IzyHistoryRange historyRange() const { return _historyRange; }
    const char* historySearchTime() const { return _historySearchTime; }
    uint32_t historyUpdatedMs() const { return _historyUpdatedMs; }
    // Diagnostic de la derniere lecture d'historique (mise au point).
    int32_t  historyDebugBytes() const { return _histDbgBytes; }
    int      historyDebugWrite() const { return _histDbgWrite; }
    int      historyDebugFirst() const { return _histDbgFirst; }
    const char* historyDebugHead() const { return _histDbgHead; }
    const char* historyDebugUrl() const { return _histDbgUrl; }
    uint32_t historyDebugNow() const { return _histDbgNow; }
    int      historyDebugMday() const { return _histDbgMd; }

    // Valeur interpolée d'une série par sa clé ("production", "pv1_volt"…).
    // Voir IzypowerCloudSeries::valueAt() pour la sémantique de *found.
    float historyValueAt(const char* key, uint32_t epochCloud, bool* found = nullptr) const {
        const IzypowerCloudSeries* serie = findSeries(key);
        if (!serie) { if (found) *found = false; return 0.0f; }
        return serie->valueAt(epochCloud, found);
    }

    // "YYYY-MM-DD HH:MM[:SS]" (ou "YYYY-MM-DD", "YYYY-MM", "YYYY") en heure
    // du cloud → epoch utilisable avec valueAt()/historyValueAt().
    // 0 si la chaîne n'est pas une date exploitable.
    static uint32_t toCloudEpoch(const char* dateTime) {
        return _epochFromDateTime(dateTime);
    }
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
    uint8_t  _seriesCount;
    IzypowerCloudSeries* _series = nullptr;   // alloue a la demande (voir _ensureSeriesBuffer)
    IzyHistoryRange _historyRange;
    char     _historySearchTime[IZY_DATE_LEN];
    uint32_t _historyUpdatedMs;
    int32_t  _histDbgBytes = 0;   // octets recus (diagnostic)
    int      _histDbgWrite = 0;   // retour de writeToStream()
    int      _histDbgFirst = -1;  // premier octet du corps
    char     _histDbgHead[64] = {0}; // debut du corps (diagnostic)
    char     _histDbgUrl[192] = {0};  // URL appelee (diagnostic)
    uint32_t _histDbgNow = 0;         // time(nullptr) au moment du calcul
    int      _histDbgMd  = -1;        // tm_mday vu par localtime_r
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

        // ⚠️ lastUpdate est en heure SERVEUR (UTC+8) : on ne s'en sert pas pour
        // la date de recherche. On prend le jour local ; le repli sur
        // lastUpdate n'a lieu que si l'horloge n'est pas synchronisée.
        if (!_searchDate[0]) {
            _refreshSearchDateFromClock();
            if (!_searchDate[0] && strlen(_lastUpdate) >= 10) {
                memcpy(_searchDate, _lastUpdate, 10);
                _searchDate[10] = 0;
            }
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

    // ---- Historique : construction d'URL et analyse --------------
    static const char* _rangeName(IzyHistoryRange range) {
        switch (range) {
            case IZY_RANGE_MONTH: return "month";
            case IZY_RANGE_YEAR:  return "year";
            case IZY_RANGE_ALL:   return "all";
            case IZY_RANGE_DAY:
            default:              return "day";
        }
    }

    // Date à utiliser : celle passée, sinon celle de lastUpdate.
    const char* _resolveDate(const char* searchTime) {
        if (searchTime && searchTime[0]) return searchTime;
        if (_searchDate[0]) return _searchDate;
        return "";
    }

    // Le paramètre searchTime est OBLIGATOIRE et son format dépend de
    // timeType (vérifié sur le cloud) :
    //   day   → "YYYY-MM-DD"   month → "YYYY-MM"
    //   year  → "YYYY"         all   → date libre
    // Une date trop précise en month/year renvoie code 500
    // « Erreur de format de date ».
    static void _normalizeDate(IzyHistoryRange range, const char* in, char* out, size_t len) {
        size_t want = 10;
        if (range == IZY_RANGE_MONTH)     want = 7;
        else if (range == IZY_RANGE_YEAR) want = 4;

        size_t n = in ? strlen(in) : 0;
        if (n > want) n = want;
        if (n >= len) n = len - 1;
        if (n) memcpy(out, in, n);
        out[n] = 0;
    }

    // Date de référence : le jour LOCAL du site. `lastUpdate` est en heure
    // serveur (UTC+8) → l'utiliser donnerait le mauvais jour en soirée.
    void _refreshSearchDateFromClock() {
        time_t now = time(nullptr);
        _histDbgNow = (uint32_t)now;
        if (now < 1600000000L) return;        // horloge non synchronisée (pré-2020)
        struct tm b;
        localtime_r(&now, &b);
        _histDbgMd = b.tm_mday;
        char tmp[16];
        snprintf(tmp, sizeof tmp, "%04d-%02d-%02d",
                 b.tm_year + 1900, b.tm_mon + 1, b.tm_mday);
        memcpy(_searchDate, tmp, IZY_DATE_LEN - 1);
        _searchDate[IZY_DATE_LEN - 1] = 0;
    }

    // Date de référence garantie : horloge locale si elle est valide, sinon
    // (repli) la date de lastUpdate — qui est en heure serveur, donc
    // potentiellement décalée d'un jour en soirée.
    // Station connue : auto-decouverte si poll() n'a pas encore tourne.
    int _ensureStation() {
        if (_stationId != 0) return IZY_OK;
        int r = fetchStations();
        if (r != IZY_OK) return r;
        if (_stationId == 0) { _lastError = IZY_ERR_NO_STATION; return _lastError; }
        return IZY_OK;
    }

    int _ensureDate() {
        // Toujours recalculer : si le premier poll a eu lieu avant la synchro
        // NTP, _searchDate contient la date du SERVEUR (UTC+8) et resterait
        // figée sur le mauvais jour toute la session.
        _refreshSearchDateFromClock();
        if (_searchDate[0]) return IZY_OK;
        if (_stationId == 0) {
            int r = fetchStations();
            if (r != IZY_OK) return r;
            if (_stationId == 0) { _lastError = IZY_ERR_NO_STATION; return _lastError; }
        }
        return fetchStationInfo();
    }

    // Jeton valide ou re-login (les endpoints report ne renvoient pas
    // toujours 401 : un 500 « veuillez vous connecter » est possible).
    int _ensureLogin() {
        if (!_hasToken || (millis() - _loginAtMs > (22UL * 3600UL * 1000UL))) {
            if (login() != IZY_OK) { _lastError = IZY_ERR_LOGIN; return _lastError; }
        }
        return IZY_OK;
    }

    // "YYYY", "YYYY-MM", "YYYY-MM-DD" ou "YYYY-MM-DD HH:MM:SS" → secondes
    // Unix (UTC naïf). Les granularités partielles retombent sur le premier
    // jour de la période : l'API renvoie "2026" (all), "2026-11" (year) et
    // "2026-11-01" (month) selon timeType. Aucune conversion de fuseau.
    // Retourne 0 si la chaîne n'est pas exploitable ("HH:MM" seul, vide…).
    static uint32_t _epochFromDateTime(const char* s) {
        if (!s || !s[0]) return 0;
        int Y = 0, M = 1, D = 1, h = 0, mi = 0, sec = 0;
        int n = sscanf(s, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &mi, &sec);
        if (n < 3) {
            int n2 = sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec);
            if (n2 > n) n = n2;
        }
        if (n < 1) return 0;
        if (n == 1) { M = 1; D = 1; }        // "2026"        → 1er janvier
        else if (n == 2) { D = 1; }          // "2026-11"     → 1er du mois
        if (Y < 1970 || Y > 2200 || M < 1 || M > 12 || D < 1 || D > 31) return 0;
        int32_t days = _daysFromCivil(Y, (unsigned)M, (unsigned)D);
        if (days < 0) return 0;
        return (uint32_t)days * 86400UL
             + (uint32_t)h * 3600UL + (uint32_t)mi * 60UL + (uint32_t)sec;
    }

    // Jours depuis 1970-01-01 (algorithme days_from_civil de H. Hinnant).
    static int32_t _daysFromCivil(int y, unsigned m, unsigned d) {
        y -= (m <= 2);
        const int32_t  era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153U * (m + (m > 2 ? (unsigned)-3 : 9U)) + 2U) / 5U + d - 1U;
        const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
        return era * 146097 + (int32_t)doe - 719468;
    }

    // GET + remplissage du buffer de séries.
    // ---- Lecture EN FLUX de l'historique -------------------------------
    // Le corps d'une journée pèse 75 à 90 Ko : impossible à loger dans le tas
    // d'un ESP32 (≈50 Ko libres). On analyse donc le flux octet par octet et
    // on ne conserve que des points décimés — aucune copie du corps, aucune
    // dépendance à ArduinoJson pour cette partie.
    //
    // Disposition réelle du JSON (vérifiée sur le cloud) : dans chaque série,
    // "data" vient AVANT "key"/"unit"/"name"/"lineType". Les points sont donc
    // accumulés dans la série courante, puis étiquetés quand la clé arrive ;
    // c'est cette clé qui clôt la série et ouvre la suivante.
    struct _HistCtx {
        uint8_t  slot        = 0;      // série en cours
        uint16_t seen        = 0;      // points vus dans la série en cours
        uint32_t stride      = 1;      // décimation adaptative
        uint32_t pendingEp   = 0;      // "time" en attente du "val"
        uint32_t lastEp      = 0;      // dernier point vu (pour conserver la fin)
        float    lastVal     = 0;
        bool     havePending = false;
        bool     haveLast    = false;
    };

    // Décimation adaptative : on remplit jusqu'à IZY_MAX_POINTS, puis on
    // divise par deux (un point sur deux) et on double le pas. Quelle que soit
    // la taille de la réponse, on conserve ~IZY_MAX_POINTS points.
    static void _histAppend(IzypowerCloudSeries& s, _HistCtx& c, uint32_t ep, float v) {
        c.lastEp = ep; c.lastVal = v; c.haveLast = true;
        if (c.seen % c.stride != 0) { c.seen++; return; }

        if (s._pointCount < IZY_MAX_POINTS) {
            IzypowerCloudPoint& p = s._points[s._pointCount++];
            p._epoch = ep; p._value = v;
        } else {
            uint16_t w = 0;
            for (uint16_t i = 0; i < s._pointCount; i += 2) s._points[w++] = s._points[i];
            s._pointCount = w;
            c.stride *= 2;
            if (s._pointCount < IZY_MAX_POINTS) {
                IzypowerCloudPoint& p = s._points[s._pointCount++];
                p._epoch = ep; p._value = v;
            }
        }
        c.seen++;
    }

    // Clôt la série courante : conserve le dernier point réellement reçu.
    static void _histClose(IzypowerCloudSeries& s, _HistCtx& c) {
        s._pointTotal = c.seen;
        if (c.haveLast && s._pointCount) {
            IzypowerCloudPoint& last = s._points[s._pointCount - 1];
            if (last._epoch != c.lastEp) {
                if (s._pointCount < IZY_MAX_POINTS) {
                    IzypowerCloudPoint& p = s._points[s._pointCount++];
                    p._epoch = c.lastEp; p._value = c.lastVal;
                } else {
                    last._epoch = c.lastEp; last._value = c.lastVal;
                }
            }
        }
        c.seen = 0; c.stride = 1; c.haveLast = false; c.havePending = false;
    }

    // Affecte une valeur de champ à la série concernée.
    void _histField(const char* key, const char* val, _HistCtx& c) {
        if (!key || !key[0]) return;
        if (strcmp(key, "key") == 0) {
            if (c.slot >= IZY_MAX_SERIES) return;            // buffer plein
            IzypowerCloudSeries& s = _series[c.slot];
            _histClose(s, c);
            strncpy(s._key, val, sizeof s._key - 1);
            if (!s._lineType[0]) strncpy(s._lineType, "line", sizeof s._lineType - 1);
            c.slot++;                                        // série suivante
            return;
        }
        if (strcmp(key, "time") == 0) {                      // précède le "val"
            c.pendingEp = _epochFromDateTime(val);
            c.havePending = true;
            return;
        }
        if (c.slot == 0) return;                             // hors série (unit/name…)
        IzypowerCloudSeries& s = _series[c.slot - 1];
        if (strcmp(key, "unit") == 0)          strncpy(s._unit, val, sizeof s._unit - 1);
        else if (strcmp(key, "name") == 0)     strncpy(s._name, val, sizeof s._name - 1);
        else if (strcmp(key, "lineType") == 0) strncpy(s._lineType, val, sizeof s._lineType - 1);
    }

    // Affecte un point (le "val" suit toujours le "time").
    // Les points précèdent la "key" de leur propre série → ils vont dans la
    // série en cours d'accumulation, c'est-à-dire _series[c.slot].
    void _histValue(const char* key, float v, _HistCtx& c) {
        if (!key || (strcmp(key, "val") != 0 && strcmp(key, "value") != 0)) return;
        if (c.slot >= IZY_MAX_SERIES) return;
        if (!c.havePending) return;
        _histAppend(_series[c.slot], c, c.pendingEp, v);
        c.havePending = false;
    }

    struct _HistScan;   // defini juste apres

    // Puits de sortie pour HTTPClient::writeToStream() : chaque octet du
    // corps (deja dechunke) part directement dans le scanner.
    struct _HistSink : public Stream {
        IzypowerCloud* self  = nullptr;
        _HistCtx*      ctx   = nullptr;
        _HistScan*     scan  = nullptr;
        bool           first = true;

        int32_t nbytes = 0;
        int     firstByte = -1;
        char    head[64] = {0};
        int     headLen = 0;

        int available() override { return 0; }   // ecriture seule
        int read() override      { return -1; }
        int peek() override      { return -1; }

        size_t write(uint8_t c) override {
            if (firstByte < 0) firstByte = (int)c;
            if (headLen < (int)sizeof(head) - 1) head[headLen++] = (char)c;
            nbytes++;
            if (first) {
                if (c != '{') return 1;
                first = false;
            }
            scan->byte((char)c, self, *ctx);
            return 1;
        }
        size_t write(const uint8_t* b, size_t n) override {
            for (size_t i = 0; i < n; i++) write(b[i]);
            return n;
        }
    };

    // Analyseur incrémental (machine à états minimale).
    struct _HistScan {
        enum St { IDLE, STR, NUM } st = IDLE;
        char tok[IZY_UPDATE_LEN + 8];
        int  len = 0;
        char key[16] = {0};
        bool closed = false;      // une chaîne vient de se fermer

        void byte(char ch, IzypowerCloud* self, _HistCtx& c) {
            for (;;) {
                if (st == STR) {
                    if (ch == '"') { tok[len] = 0; st = IDLE; closed = true; return; }
                    if (ch == '\\') { /* échappement : on ignore le suivant */ }
                    else if (len < (int)sizeof(tok) - 1) tok[len++] = ch;
                    return;
                }
                if (st == NUM) {
                    if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' ||
                        ch == '.' || ch == 'e' || ch == 'E') {
                        if (len < (int)sizeof(tok) - 1) tok[len++] = ch;
                        return;
                    }
                    tok[len] = 0;
                    self->_histValue(key, (float)atof(tok), c);
                    st = IDLE; len = 0;
                    continue;                       // retraiter ce caractère
                }
                // IDLE
                if (closed) {
                    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') return;
                    closed = false;
                    if (ch == ':') {                // la chaîne était une CLÉ
                        strncpy(key, tok, sizeof key - 1);
                        key[sizeof key - 1] = 0;
                        len = 0;
                        return;
                    }
                    self->_histField(key, tok, c);  // la chaîne était une VALEUR
                    len = 0;
                    continue;
                }
                if (ch == '"') { st = STR; len = 0; return; }
                if ((ch >= '0' && ch <= '9') || ch == '-') { st = NUM; len = 0; tok[len++] = ch; return; }
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
                    // null / true / false : mot-clé sans guillemets
                    st = NUM; len = 0; tok[len++] = ch;
                    return;
                }
                return;
            }
        }
    };

    // Le buffer des series (IZY_MAX_SERIES x IZY_MAX_POINTS x 8 o, ~6,7 Ko par
    // defaut) n'est PAS statique : sur un firmware charge, 6,7 Ko de BSS en
    // moins, c'est 6,7 Ko de tas en plus pour les pages web et le serveur MCP
    // (Guard_ESP refuse une reponse > 65 % du tas libre). Il est alloue au
    // premier chargement d'historique puis rendu par releaseHistory().
    bool _ensureSeriesBuffer() {
        if (_series) return true;
        _series = (IzypowerCloudSeries*)calloc(IZY_MAX_SERIES, sizeof(IzypowerCloudSeries));
        return _series != nullptr;
    }

    int _fetchHistory(const String& url, IzyHistoryRange range, const char* searchTime) {
        _seriesCount = 0;
        _historyRange = range;
        _historySearchTime[0] = 0;

        strncpy(_histDbgUrl, url.c_str(), sizeof _histDbgUrl - 1);
        _histDbgUrl[sizeof _histDbgUrl - 1] = 0;

        if (!_ensureSeriesBuffer()) { _lastError = IZY_ERR_PARSE; return _lastError; }
        memset(_series, 0, sizeof(IzypowerCloudSeries) * IZY_MAX_SERIES);

        for (int attempt = 0; attempt < 2; attempt++) {
            HTTPClient http;
            http.setTimeout(_httpTimeoutMs);
            if (!http.begin(_client, url)) { _lastError = IZY_ERR_CONNECT; return _lastError; }
            http.addHeader("app-platform", "izy");
            http.addHeader("Accept-Language", "fr-FR,fr;q=0.9");
            if (_hasToken) http.addHeader("x-tts-access-token", _token);

            int code = http.GET();
            if ((code == 401 || code == 403) && attempt == 0) {
                http.end();
                if (login() != IZY_OK) { _lastError = IZY_ERR_LOGIN; return _lastError; }
                continue;
            }
            if (code < 0)   { http.end(); _lastError = IZY_ERR_CONNECT; return _lastError; }
            if (code != 200){ http.end(); _lastError = IZY_ERR_HTTP;    return _lastError; }

            // Lecture du corps via writeToStream() : HTTPClient y decode
            // lui-meme le Transfer-Encoding chunked (l'API repond en chunked)
            // et n'alloue RIEN pour le corps : on alimente le scanner au fil
            // de l'eau avec un Print minimal.
            _HistCtx  ctx;
            _HistScan scan;

            _HistSink sink;
            sink.self = this;
            sink.ctx  = &ctx;
            sink.scan = &scan;
            sink.first = true;
            _histDbgBytes = 0; _histDbgFirst = -1;
            _histDbgWrite = http.writeToStream(&sink);
            _histDbgBytes = sink.nbytes;
            _histDbgFirst = sink.firstByte;
            memcpy(_histDbgHead, sink.head, sizeof _histDbgHead);
            _histDbgHead[sizeof _histDbgHead - 1] = 0;

            if (sink.first) { _lastError = IZY_ERR_PARSE; return _lastError; }
            if (ctx.slot == 0) { _lastError = IZY_ERR_PARSE; return _lastError; }

            _seriesCount = (ctx.slot > IZY_MAX_SERIES) ? IZY_MAX_SERIES : ctx.slot;
            const char* used = _resolveDate(searchTime);
            strncpy(_historySearchTime, used, sizeof _historySearchTime - 1);
            _historySearchTime[sizeof _historySearchTime - 1] = 0;
            _historyUpdatedMs = millis();
            return IZY_OK;
        }
        _lastError = IZY_ERR_TOKEN;
        return _lastError;
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

inline int IzypowerCloud::fetchStationHistory(IzyHistoryRange range,
                                              const char* searchTime,
                                              bool showSoc) {
    if (_ensureLogin() != IZY_OK) return _lastError;
    if (_ensureStation() != IZY_OK) return _lastError;
    if (_ensureDate() != IZY_OK) return _lastError;

    String url = String(BASE_URL) + "/report/v2/powerStations/data/" + String(_stationId);
    url += "?timeType=";
    url += _rangeName(range);
    url += "&dataFlag=energy";
    char date[IZY_DATE_LEN];
    _normalizeDate(range, _resolveDate(searchTime), date, sizeof date);
    if (date[0]) { url += "&searchTime="; url += date; }
    if (showSoc) url += "&showSoc=true";

    return _fetchHistory(url, range, date);
}

inline int IzypowerCloud::fetchDeviceHistory(const char* deviceSn,
                                             IzyHistoryRange range,
                                             const char* searchTime,
                                             const char* dataFlag) {
    if (!deviceSn || !deviceSn[0]) { _lastError = IZY_ERR_PARSE; return _lastError; }
    if (_ensureLogin() != IZY_OK) return _lastError;
    if (_ensureDate() != IZY_OK) return _lastError;

    String url = String(BASE_URL) + "/report/device/data/" + deviceSn;
    url += "?timeType=";
    url += _rangeName(range);
    if (dataFlag && dataFlag[0]) { url += "&dataFlag="; url += dataFlag; }
    char date[IZY_DATE_LEN];
    _normalizeDate(range, _resolveDate(searchTime), date, sizeof date);
    if (date[0]) { url += "&searchTime="; url += date; }

    return _fetchHistory(url, range, date);
}

inline int IzypowerCloud::fetchPvHistory(const char* deviceSn,
                                         IzyHistoryRange range,
                                         const char* searchTime) {
    if (!deviceSn || !deviceSn[0]) { _lastError = IZY_ERR_PARSE; return _lastError; }
    if (_ensureLogin() != IZY_OK) return _lastError;
    if (_ensureDate() != IZY_OK) return _lastError;

    String url = String(BASE_URL) + "/report/izy/device/data/" + deviceSn;
    url += "?timeType=";
    url += _rangeName(range);
    url += "&queryType=pv";
    char date[IZY_DATE_LEN];
    _normalizeDate(range, _resolveDate(searchTime), date, sizeof date);
    if (date[0]) { url += "&searchTime="; url += date; }

    return _fetchHistory(url, range, date);
}

const char* IzypowerCloud::BASE_URL =
    "http://application.izypowercloud.fr/photo_voltaic/api";

#endif  // IZYPOWERCLOUD_H
