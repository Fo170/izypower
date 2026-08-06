# Changelog

## [0.3.0] — 2026-08-06

- **Diagnostic matériel définitif** : les iP n'exposent **ni INDEVOLT RPC (8080) ni SOLARMAN V5 (8899)** en local (aucun port TCP ouvert, aucune réponse UDP) → voie principale = **API cloud Izypower** (`application.izypowercloud.fr`, rafraîchi ~3 min).
- `src/IzypowerCloud.h` (nouveau) : client **API cloud** header-only — login (token `x-tts-access-token`, re-login auto sur 401/22 h), auto-découverte de la centrale, station (puissance W, énergies jour/mois/an/total, signal, `lastUpdate`), onduleurs et **chaînes PV individuelles** (PV1..PV4 W + kWh/jour) via l'endpoint `component`. Dépend d'ArduinoJson.
- `library.json` / `library.properties` : version 0.3.0, header `IzypowerCloud.h` déclaré.
- Validation sur matériel : réponses cloud réelles dumpées et vérifiées (stations, device page, chaînes PV, wifi) avec une sonde Python (`tools/cloud_probe.py`).

## [0.2.0] — 2026-08-06

- **Diagnostic matériel** : les micro-onduleurs iP n'exposent pas l'API INDEVOLT RPC (port 8080 fermé). Ce sont des **Deye** (module WiFi WE-P2, logger intégré) → nouvelle voie de lecture **locale Solarman V5 (TCP 8899, Modbus RTU)**, temps réel.
- `src/IzypowerSolarman.h` (nouveau) : client Solarman V5 header-only (framing, checksum, double-CRC DEYE, réponse heartbeat/handshake) + décodeur `DeyeMicro` des registres Deye micro-onduleur (S/N, état, PV1-4 V/A/W, AC, énergies, température, alarmes/défauts).
- `library.json` / `library.properties` : version 0.2.0, deux headers déclarés.
- `tools/test_local.py` : scan ports + découverte UDP Solarman + test de lecture optionnel (`--pysolarman --serial <SN>`).
- Cartes de registres croisées sur protocole officiel Deye V118 + intégrations Home Assistant (StephanJoubert, davidrapan).

## [0.1.0] — 2026-08-06

- Première version.
- `src/Izypower.h` : client INDEVOLT RPC header-only (GetData / Sys.GetConfig / WiFi.GetConfig / Cloud.GetStatus), registres PV/AC/réseau.
- Compatible ESP32 et ESP8266, dépendance ArduinoJson v7.
