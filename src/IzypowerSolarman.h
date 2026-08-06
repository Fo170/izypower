// ============================================================
// IzypowerSolarman.h — Client local SOLARMAN V5 (TCP 8899)
// pour micro-onduleurs IZYPOWER iP-1000-W2 / iP-2000-W4.
//
// Les micro-onduleurs IZYPOWER sont des Deye (module WiFi WE-P2,
// logger "Built-in datalogger") : SUN600G3 / SUN1000G3 (2 MPPT)
// et SUN2000G3 (4 MPPT). Ils exposent le protocole SOLARMAN V5
// (Modbus RTU encapsulé) sur le port TCP 8899.
//
//   Frame V5 (requête, payload 15 octets) :
//     A5 [len:2LE] 10 [ctrl] [seq:2LE] [serial:4LE] 02 0000
//     00000000 00000000 00000000 [modbus RTU] [checksum] 15
//
//   Frame V5 (réponse, payload 14 octets — "Sensor Type" 2 octets →
//   "Status" 1 octet) : le Modbus RTU commence à l'octet 25.
//   Certains loggers DEYE ajoutent en plus une double CRC (2 octets 0x00)
//   en fin de trame Modbus : le client la détecte et la retire.
//
// Ce fichier est header-only (tout est inline), sans dépendance
// externe (pas même ArduinoJson). Compatible ESP8266/ESP32.
//
// Registres micro-onduleur Deye (holding, fc 0x03, adresses 0-based) :
// cf. protocol officiel Deye V118 + intégrations Home Assistant
// (StephanJoubert/home_assistant_solarman, davidrapan/ha-solarman).
// Les valeurs 32 bits sont stockées mot faible d'abord.
// ============================================================

#ifndef IZYPOWERSOLARMAN_H
#define IZYPOWERSOLARMAN_H

#include <Arduino.h>
#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif

// ============================================================
// CODES DE CONTRÔLE SOLARMAN V5
// ============================================================
#define S5_CTRL_HANDSHAKE  0x41
#define S5_CTRL_DATA       0x42
#define S5_CTRL_INFO       0x43
#define S5_CTRL_REQUEST    0x45   // réponse = 0x45 - 0x30 = 0x15
#define S5_CTRL_HEARTBEAT  0x47
#define S5_CTRL_REPORT     0x48

// ============================================================
// REGISTRES MICRO-ONDULEUR DEYE (holding registers, fc 0x03)
// ============================================================
#define DM_REG_START       0x0001  // première adresse lue (0-based)
#define DM_REG_COUNT       0x007D  // 0x0001..0x007D = 125 registres

#define DM_SERIAL_LO       0x0003  // S/N micro-onduleur (5 reg., 2 car./reg.)
#define DM_SERIAL_HI       0x0007
#define DM_RATED_POWER     0x0010  // W × 0,1
#define DM_STATUS          0x003B  // 0 Standby,1 Self-test,2 Normal,3 Alarm,4 Fault
#define DM_TODAY_KWH       0x003C  // kWh × 0,1
#define DM_TOTAL_KWH_LO    0x003F  // kWh × 0,1 (32 bits, mot faible d'abord)
#define DM_GRID_VOLTAGE    0x0049  // V × 0,1
#define DM_GRID_CURRENT    0x004C  // A × 0,1 (signé)
#define DM_GRID_FREQ       0x004F  // Hz × 0,01
#define DM_POWER_LO        0x0056  // W × 0,1 (32 bits)
#define DM_TEMPERATURE     0x005A  // (°C + 1000) × 0,01
#define DM_ALARM_LO        0x0065  // 2 registres (bits)
#define DM_FAULT_LO        0x0067  // 4 registres (bits)
#define DM_PV1_VOLTAGE     0x006D  // V × 0,1 (2 reg. par MPPT : V puis A)

// ============================================================
// CLIENT SOLARMAN V5
// ============================================================

class SolarmanV5 {
public:
    // host         : IP du module WiFi (ex. "192.168.1.100")
    // loggerSerial : S/N du "Built-in datalogger" (étiquette/app),
    //                renvoyé aussi par la découverte UDP de tools/test_local.py
    // port         : port TCP (défaut 8899)
    // timeoutMs    : timeout global échange (défaut 3000)
    SolarmanV5(const char* host, uint32_t loggerSerial,
               uint16_t port = 8899, uint16_t timeoutMs = 3000)
        : _host(host), _port(port), _timeoutMs(timeoutMs), _seq(0) {
        _serialBytes[0] = (uint8_t)(loggerSerial & 0xFF);
        _serialBytes[1] = (uint8_t)((loggerSerial >> 8) & 0xFF);
        _serialBytes[2] = (uint8_t)((loggerSerial >> 16) & 0xFF);
        _serialBytes[3] = (uint8_t)((loggerSerial >> 24) & 0xFF);
    }

    // Lit `quantity` holding registers à partir de `addr` (adresse 0-based).
    // Remplit `out` (quantity ≥ capacity non autorisé).
    // Connexion courte : ouverture, requête, réponse, fermeture.
    bool readHoldingRegisters(uint16_t addr, uint16_t quantity,
                              uint16_t* out, size_t capacity) {
        if (quantity == 0 || quantity > 125 || capacity < quantity) return false;
        if (!_client.connect(_host.c_str(), _port)) return false;
        _client.setTimeout(_timeoutMs);

        bool ok = false;
        if (_sendRequest(addr, quantity)) {
            uint32_t deadline = millis() + _timeoutMs;
            while (millis() < deadline) {
                uint8_t rx[512];
                size_t rxLen;
                if (!_readFrame(rx, sizeof(rx), deadline)) break;
                if (!_validFrame(rx, rxLen)) continue;

                uint8_t ctrl = rx[4];
                if (ctrl == (S5_CTRL_REQUEST - 0x30)) {        // réponse à notre requête
                    if (rx[5] != _seq || rx[11] != 0x02) continue;
                    size_t mbLen = rxLen - 27;                 // modbus = rx[25 .. rxLen-2[
                    const uint8_t* mb = rx + 25;
                    if (mbLen < 5) continue;

                    // Double CRC (bug DEYE) : 2 octets 0x00 en fin de trame
                    if (mbLen >= 7 && mb[mbLen - 2] == 0x00 && mb[mbLen - 1] == 0x00) {
                        uint16_t crc2 = _crc16(mb, mbLen - 4);
                        if (mb[mbLen - 4] == (crc2 & 0xFF) &&
                            mb[mbLen - 3] == (crc2 >> 8))
                            mbLen -= 2;                        // retire la double CRC
                    }

                    uint16_t crc = _crc16(mb, mbLen - 2);
                    if (mb[mbLen - 2] != (crc & 0xFF) ||
                        mb[mbLen - 1] != (crc >> 8)) continue; // CRC modbus invalide

                    if (mb[1] & 0x80) break;                   // exception Modbus
                    if (mb[2] != quantity * 2) break;          // longueur inattendue

                    for (uint16_t i = 0; i < quantity; i++)
                        out[i] = (mb[3 + 2 * i] << 8) | mb[4 + 2 * i];
                    ok = true;
                    break;
                } else if (ctrl == S5_CTRL_HANDSHAKE || ctrl == S5_CTRL_DATA ||
                           ctrl == S5_CTRL_INFO || ctrl == S5_CTRL_HEARTBEAT ||
                           ctrl == S5_CTRL_REPORT) {
                    _sendTimeResponse(rx, rxLen);              // garde la liaison vivante
                }
                // autre trame inattendue : ignorée, on continue de lire
            }
        }
        _client.stop();
        return ok;
    }

private:
    String      _host;
    uint16_t    _port;
    uint16_t    _timeoutMs;
    uint8_t     _seq;
    uint8_t     _serialBytes[4];
    WiFiClient  _client;

    // CRC-16/Modbus
    static uint16_t _crc16(const uint8_t* data, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (uint8_t b = 0; b < 8; b++)
                crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
        return crc;
    }

    // Construit et envoie la requête V5 (Modbus fc 0x03).
    bool _sendRequest(uint16_t addr, uint16_t quantity) {
        _seq = (_seq + 1) & 0xFF;

        uint8_t mb[8];                                   // requête Modbus RTU
        mb[0] = 0x01;                                    // slave ID
        mb[1] = 0x03;                                    // read holding registers
        mb[2] = (uint8_t)(addr >> 8);
        mb[3] = (uint8_t)(addr & 0xFF);
        mb[4] = (uint8_t)(quantity >> 8);
        mb[5] = (uint8_t)(quantity & 0xFF);
        uint16_t crc = _crc16(mb, 6);
        mb[6] = (uint8_t)(crc & 0xFF);
        mb[7] = (uint8_t)(crc >> 8);

        uint8_t f[64];
        uint16_t length = 15 + sizeof(mb);               // 23
        f[0] = 0xA5;
        f[1] = (uint8_t)(length & 0xFF);
        f[2] = (uint8_t)(length >> 8);
        f[3] = 0x10;
        f[4] = S5_CTRL_REQUEST;
        f[5] = _seq;
        f[6] = 0x00;
        f[7] = _serialBytes[0];
        f[8] = _serialBytes[1];
        f[9] = _serialBytes[2];
        f[10] = _serialBytes[3];
        f[11] = 0x02;                                    // frametype
        f[12] = 0x00; f[13] = 0x00;                      // sensortype
        f[14] = 0x00; f[15] = 0x00; f[16] = 0x00; f[17] = 0x00; // delivery time
        f[18] = 0x00; f[19] = 0x00; f[20] = 0x00; f[21] = 0x00; // power on time
        f[22] = 0x00; f[23] = 0x00; f[24] = 0x00; f[25] = 0x00; // offset time
        memcpy(f + 26, mb, sizeof(mb));
        size_t payloadEnd = 26 + sizeof(mb);             // 34

        uint8_t sum = 0;
        for (size_t i = 1; i < payloadEnd; i++) sum += f[i];
        f[payloadEnd] = sum;
        f[payloadEnd + 1] = 0x15;

        return _client.write(f, payloadEnd + 2) == payloadEnd + 2;
    }

    // Lit une trame V5 complète (resynchronise sur 0xA5, suit la longueur).
    bool _readFrame(uint8_t* buf, size_t cap, uint32_t deadline) {
        size_t pos = 0;
        while (millis() < deadline) {
            if (_client.available() > 0) {
                int b = _client.read();
                if (b < 0) continue;
                if (pos == 0 && b != 0xA5) continue;     // octet parasite : resync
                buf[pos++] = (uint8_t)b;
                if (pos >= 3) {
                    uint16_t length = buf[1] | (buf[2] << 8);
                    size_t total = 13 + length;          // 11 header + checksum + 0x15
                    if (total < 16 || total > cap) return false;
                    if (pos >= total) return true;
                }
            } else {
                delay(1);
            }
        }
        return false;
    }

    // Validation générique d'une trame V5 reçue.
    bool _validFrame(const uint8_t* buf, size_t len) {
        if (len < 16 || buf[0] != 0xA5 || buf[len - 1] != 0x15) return false;
        if (buf[3] != 0x10) return false;
        uint8_t sum = 0;
        for (size_t i = 1; i < len - 2; i++) sum += buf[i];
        if (sum != buf[len - 2]) return false;           // checksum V5 invalide
        return memcmp(buf + 7, _serialBytes, 4) == 0;    // S/N logger
    }

    // Répond aux trames heartbeat/handshake/info comme pysolarmanv5.
    void _sendTimeResponse(const uint8_t* rx, size_t /*rxLen*/) {
        uint8_t r[23];
        uint16_t length = 10;
        r[0] = 0xA5;
        r[1] = (uint8_t)(length & 0xFF);
        r[2] = (uint8_t)(length >> 8);
        r[3] = 0x10;
        r[4] = rx[4] - 0x30;                             // code de réponse
        r[5] = (rx[5] + 1) & 0xFF;                       // séquence écho (+1)
        r[6] = rx[6];
        r[7] = _serialBytes[0];
        r[8] = _serialBytes[1];
        r[9] = _serialBytes[2];
        r[10] = _serialBytes[3];
        r[11] = 0x00; r[12] = 0x01;                      // 0x0100 (LE)
        uint32_t t = (uint32_t)(millis() / 1000);        // heure (placeholdier)
        r[13] = (uint8_t)(t & 0xFF);
        r[14] = (uint8_t)((t >> 8) & 0xFF);
        r[15] = (uint8_t)((t >> 16) & 0xFF);
        r[16] = (uint8_t)((t >> 24) & 0xFF);
        r[17] = 0x00; r[18] = 0x00; r[19] = 0x00; r[20] = 0x00;
        uint8_t sum = 0;
        for (size_t i = 1; i < 21; i++) sum += r[i];
        r[21] = sum;
        r[22] = 0x15;
        _client.write(r, sizeof(r));
    }
};

// ============================================================
// DONNÉES DÉCODÉES D'UN MICRO-ONDULEUR DEYE
// ============================================================

struct DeyeMicro {
    char     serial[11]   = {0};   // S/N micro-onduleur (10 caractères)
    uint8_t  mppts        = 0;
    uint8_t  status       = 0;     // 0 Standby,1 Self-test,2 Normal,3 Alarm,4 Fault
    uint16_t ratedPowerW  = 0;     // puissance nominale (W)
    float    pvVoltage[4] = {0, 0, 0, 0};   // V
    float    pvCurrent[4] = {0, 0, 0, 0};   // A
    float    pvPower[4]   = {0, 0, 0, 0};   // W (V × I)
    float    todayKWh     = 0;     // production du jour
    float    totalKWh     = 0;     // production totale
    float    gridVoltage  = 0;     // V
    float    gridCurrent  = 0;     // A (signé)
    float    gridFreq     = 0;     // Hz
    float    powerW       = 0;     // puissance AC active (W)
    float    temperature  = 0;     // °C
    uint32_t alarm        = 0;     // bitmap alarmes
    uint64_t fault        = 0;     // bitmap défauts

    // Décode `n` registres bruts (regs[0] = DM_REG_START).
    void decode(const uint16_t* regs, size_t n, uint8_t mpptCount) {
        mppts = (mpptCount > 4) ? 4 : mpptCount;
        auto R = [&](uint16_t addr) -> uint16_t {
            uint16_t idx = addr - DM_REG_START;
            return (idx < n) ? regs[idx] : 0;
        };

        // S/N : 5 registres × 2 caractères (octet haut puis bas)
        size_t k = 0;
        for (uint16_t a = DM_SERIAL_LO; a <= DM_SERIAL_HI && k + 2 <= 10; a++) {
            uint16_t r = R(a);
            char hi = (char)(r >> 8), lo = (char)(r & 0xFF);
            if (hi >= 0x20 && hi <= 0x7E) serial[k++] = hi;
            if (lo >= 0x20 && lo <= 0x7E) serial[k++] = lo;
        }
        serial[k] = '\0';

        status      = (uint8_t)(R(DM_STATUS) & 0x0F);
        ratedPowerW = (uint16_t)(R(DM_RATED_POWER) * 0.1f + 0.5f);

        for (uint8_t i = 0; i < mppts; i++) {
            uint16_t v = R(DM_PV1_VOLTAGE + 2 * i);
            uint16_t c = R(DM_PV1_VOLTAGE + 2 * i + 1);
            pvVoltage[i] = v * 0.1f;
            pvCurrent[i] = c * 0.1f;
            pvPower[i]   = pvVoltage[i] * pvCurrent[i];
        }

        todayKWh    = R(DM_TODAY_KWH) * 0.1f;
        totalKWh    = _u32(R(DM_TOTAL_KWH_LO), R(DM_TOTAL_KWH_LO + 1)) * 0.1f;
        gridVoltage = R(DM_GRID_VOLTAGE) * 0.1f;
        gridCurrent = _s16(R(DM_GRID_CURRENT)) * 0.1f;
        gridFreq    = R(DM_GRID_FREQ) * 0.01f;
        powerW      = _u32(R(DM_POWER_LO), R(DM_POWER_LO + 1)) * 0.1f;
        temperature = (R(DM_TEMPERATURE) - 1000) * 0.01f;

        alarm = _u32(R(DM_ALARM_LO), R(DM_ALARM_LO + 1));
        fault = 0;
        for (int i = 3; i >= 0; i--)
            fault = (fault << 16) | R(DM_FAULT_LO + i);
    }

    // Libellé de l'état (0 Standby, 1 Self-test, 2 Normal, 3 Alarm, 4 Fault)
    static const char* statusLabel(uint8_t s) {
        switch (s) {
            case 0: return "Standby";
            case 1: return "Self-test";
            case 2: return "Normal";
            case 3: return "Alarm";
            case 4: return "Fault";
            default: return "?";
        }
    }

private:
    static uint32_t _u32(uint16_t lo, uint16_t hi) {
        return (uint32_t)lo | ((uint32_t)hi << 16);
    }
    static int16_t _s16(uint16_t v) {
        return (v & 0x8000) ? (int16_t)(v - 0x10000) : (int16_t)v;
    }
};

#endif // IZYPOWERSOLARMAN_H
