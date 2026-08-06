// ============================================================
// Izypower.h — Client INDEVOLT RPC (OpenData) pour micro-onduleurs
// IZYPOWER iP-1000-W2 / iP-2000-W4 (et appareils de la gamme).
// https://github.com/Fo170/izypower
//
// Principe : les appareils IZYPOWER exposent une API HTTP locale
// "Indevolt" (framework OpenData) sur le port 8080 :
//   POST /rpc/Indevolt.GetData?config={"t":[<registres>]}
//   → réponse JSON : { "1664": 312, "2108": 586, ... }
// Les clés de la réponse sont les identifiants de registres (string).
//
// Ce fichier est header-only (tout est inline). Aucune dépendance
// globale : seule ArduinoJson est requise. Compatible ESP8266/ESP32.
// ============================================================

#ifndef IZYPOWER_H
#define IZYPOWER_H

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
// REGISTRES (cJson points) — cf. indevolt-api (xirt) / khirale
// ============================================================

// Identité / système
#define IZP_SERIAL          0    // Numéro de série
#define IZP_OPERATING_MODE  606  // 1000=Master 1001=Slave 1002=Standalone
#define IZP_WORKING_MODE    7101 // Mode de fonctionnement
#define IZP_ALARM_CODE      8100 // Code alarme

// Production solaire
#define IZP_PV_DC_OUTPUT_POWER  1501 // Puissance DC totale (W)
#define IZP_DAILY_PRODUCTION    1502 // Production du jour (kWh)
#define IZP_CUMUL_PRODUCTION    1505 // Production cumulée (kWh)

// PV par entrée : tension / courant / puissance (1..4)
#define IZP_PV_VOLTAGE_1  1600
#define IZP_PV_VOLTAGE_2  1601
#define IZP_PV_VOLTAGE_3  1602
#define IZP_PV_VOLTAGE_4  1603
#define IZP_PV_CURRENT_1  1632
#define IZP_PV_CURRENT_2  1633
#define IZP_PV_CURRENT_3  1634
#define IZP_PV_CURRENT_4  1635
#define IZP_PV_POWER_1    1664
#define IZP_PV_POWER_2    1665
#define IZP_PV_POWER_3    1666
#define IZP_PV_POWER_4    1667

// Côté AC
#define IZP_AC_VOLTAGE       2083 // V
#define IZP_AC_CURRENT       2086 // A
#define IZP_AC_FREQUENCY     2095 // Hz
#define IZP_AC_REACTIVE_POW  2097 // var
#define IZP_AC_APPARENT_POW  2098 // VA
#define IZP_AC_POWER_FACTOR  2099 // cos phi
#define IZP_AC_INPUT_POWER   2101 // W (import)
#define IZP_AC_OUTPUT_POWER  2108 // W (export)
#define IZP_AC_NET_POWER     2278 // W (flux net entrée/sortie)
#define IZP_TOTAL_IN_ENERGY  2107 // kWh (énergie totale importée)
#define IZP_TOTAL_OUT_ENERGY 2104 // kWh (énergie totale exportée)
#define IZP_BYPASS_POWER     667  // W

// Réseau / smart meter
#define IZP_GRID_VOLTAGE   2600 // V
#define IZP_GRID_FREQUENCY 2612 // Hz
#define IZP_METER_CONNECTED 7120 // 1000=ON 1001=OFF
#define IZP_METER_POWER    11016 // W (puissance mesurée)

// ============================================================
// CLASSE IZYPOWER
// ============================================================

class Izypower {
public:
    // host      : IP ou hostname de l'appareil (ex. "192.168.1.100")
    // port      : port RPC local (défaut 8080)
    // timeoutMs : timeout de la requête HTTP (défaut 3000)
    Izypower(const char* host, uint16_t port = 8080, uint16_t timeoutMs = 3000)
        : _timeoutMs(timeoutMs) {
        _baseUrl = "http://";
        _baseUrl += host;
        _baseUrl += ":";
        _baseUrl += String(port);
        _baseUrl += "/rpc";
    }

    // Lit une liste de registres en une seule requête GetData.
    // Remplit `out` (objet JSON) : clé = registre (string), valeur = brut.
    // Retourne false si la requête a échoué ou si le JSON est invalide.
    bool readRegisters(const uint16_t* ids, size_t n, JsonObject out) {
        if (n == 0 || ids == nullptr) return false;

        JsonDocument cfg;
        JsonArray arr = cfg["t"].to<JsonArray>();
        for (size_t i = 0; i < n; i++) arr.add(ids[i]);

        String configStr;
        serializeJson(cfg, configStr);

        String url = _baseUrl + "/Indevolt.GetData?config=" + configStr;

        String body;
        if (!_rpc(url, true, body)) return false;

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, body);
        if (err) return false;

        JsonObject root = doc.as<JsonObject>();
        if (root.isNull()) return false;

        out.clear();
        for (JsonPair kv : root) {
            if (kv.value().isNull()) continue;   // registre absent / non supporté
            out[kv.key().c_str()] = kv.value();
        }
        return true;
    }

    // Lecture d'un seul registre → `value`. Retourne false si absent.
    bool readRegister(uint16_t id, float& value) {
        JsonDocument doc;
        JsonObject out = doc.to<JsonObject>();
        if (!readRegisters(&id, 1, out)) return false;
        JsonVariant v = out[String(id)];
        if (v.isNull()) return false;
        value = v.as<float>();
        return !isnan(value);
    }

    // GET /rpc/Sys.GetConfig → configuration système brute (JSON)
    bool getConfig(JsonDocument& out) {
        String body;
        if (!_rpc(_baseUrl + "/Sys.GetConfig", false, body)) return false;
        DeserializationError err = deserializeJson(out, body);
        return !err;
    }

    // GET /rpc/WiFi.GetConfig → { "sta": { "rssi", "ssid", "ip", ... } }
    bool getWifiConfig(JsonDocument& out) {
        String body;
        if (!_rpc(_baseUrl + "/WiFi.GetConfig", false, body)) return false;
        DeserializationError err = deserializeJson(out, body);
        return !err;
    }

    // GET /rpc/Cloud.GetStatus → liste d'états MQTT/cloud
    bool getCloudStatus(JsonDocument& out) {
        String body;
        if (!_rpc(_baseUrl + "/Cloud.GetStatus", false, body)) return false;
        DeserializationError err = deserializeJson(out, body);
        return !err;
    }

private:
    String     _baseUrl;
    uint16_t   _timeoutMs;
    WiFiClient _client;

    // Exécute une requête RPC. `post` = true pour POST (GetData/SetData),
    // false pour GET (Sys.GetConfig, WiFi.GetConfig, Cloud.GetStatus).
    bool _rpc(const String& url, bool post, String& response) {
        HTTPClient http;
        bool ok = false;

        if (!http.begin(_client, url)) return false;
        http.setTimeout(_timeoutMs);

        int code;
        if (post) {
            code = http.POST("");     // corps vide, paramètres dans l'URL
        } else {
            code = http.GET();
        }

        if (code == HTTP_CODE_OK) {
            response = http.getString();
            ok = true;
        }

        http.end();
        return ok;
    }
};

#endif // IZYPOWER_H
