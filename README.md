# Izypower

Monitoring des **micro-onduleurs IZYPOWER** (Materfrance) : **iP-1000-W2** (1000 W, 2 MPPT) et **iP-2000-W4** (2000 W, 4 MPPT).

⚠️ **Accès aux données (vérifié sur matériel avec `tools/test_local.py`)** : ces micro-onduleurs sont des **Deye** (module WiFi WE-P2) mais **n'exposent NI l'API INDEVOLT RPC (8080) NI SOLARMAN V5 (8899)** en local : aucun port TCP ouvert, aucune réponse UDP Solarman. Ils sont **100 % orientés cloud**. La lecture passe donc par l'**API cloud Izypower** (`application.izypowercloud.fr`) : login → token `x-tts-access-token`, données rafraîchies **toutes les ~3 min** (pas de temps réel).

Trois clients header-only sont fournis, selon le matériel :

| Client | Matériel | Protocole |
|---|---|---|
| `IzypowerCloud.h` | iP-1000-W2 / iP-2000-W4 (**voie principale**) | API cloud `application.izypowercloud.fr` |
| `IzypowerSolarman.h` | onduleurs Deye exposant un logger local | Solarman V5, TCP 8899, Modbus RTU |
| `Izypower.h` | Titan battery / batteries | INDEVOLT RPC, port 8080 |

## Accès aux données selon le type d'appareil

Tous les appareils de la gamme ne sont **pas accessibles de la même façon** : selon le matériel, la lecture passe par l'API cloud, par un protocole local, ou est simplement **impossible en local**. Ce tableau récapitule ce qui est possible, vérifié sur matériel.

| Appareil | Accès local | Accès cloud | Client à utiliser | Remarques |
|---|---|---|---|---|
| **iP-1000-W2** (1000 W, 2 MPPT) | ❌ **Aucun** (8080, 8899, 502, 80… fermés ; pas d'UDP Solarman) | ✅ API Izypower (~3 min) | `IzypowerCloud.h` | Module WiFi WE-P2 = Deye, mais **100 % cloud** |
| **iP-2000-W4** (2000 W, 4 MPPT) | ❌ **Aucun** (idem) | ✅ API Izypower (~3 min) | `IzypowerCloud.h` | Idem |
| **Micro-onduleurs Deye SUN\*** avec logger local | ✅ **Solarman V5** (TCP 8899) | — | `IzypowerSolarman.h` | Temps réel si le port 8899 répond |
| **Titan battery / batteries** | ✅ **INDEVOLT RPC** (TCP 8080) | — | `Izypower.h` | API HTTP locale `Indevolt.GetData` |

**Pourquoi ?** Les micro-onduleurs iP utilisent le module WiFi **Deye WE-P2** qui ne répond à **aucun port TCP** ni à la **découverte UDP Solarman** (48899) : impossible d'y accéder autrement que par le cloud du fabricant. Seuls les onduleurs Deye dotés d'un *Built-in datalogger* (port 8899 ouvert) et les appareils Titan (port 8080) sont lisibles en local.

**Comment déterminer le bon chemin sur votre installation ?** → exécutez `python3 tools/test_local.py` (voir section [Outils Python](#outils-python)) : il scanne les ports TCP et fait la découverte UDP Solarman pour vous dire quel protocole répond.

## État de la validation sur matériel

Ce qui a été **réellement testé** sur mes micro-onduleurs iP (un iP-1000-W2 et un iP-2000-W4), et ce qui **reste à valider** par vos soins :

### ✅ Validé sur matériel

| Élément | Résultat |
|---|---|
| **Accès local** (`tools/test_local.py`) | **Aucun port TCP ouvert** (80/443/502/8080/8899/9090/10000/48899/57000), **aucune réponse UDP Solarman** → les iP sont 100 % cloud. |
| **Login cloud** (`tools/cloud_probe.py`) | `POST /login` → token JWT valide (`data.token`), en-tête `app-platform: izy` obligatoire. |
| **Auto-découverte station** | `GET /powerStations/page` → station (stationsId fictif `12345`, cap 3000 W) trouvée automatiquement. |
| **Info station** | `GET /v3/powerStations/info/{id}` → `power` (W), énergies jour/mois/an/total (kWh), `lastUpdate`, `signal`, `onlineState`. |
| **Chaînes PV** (`GET /component`) | `pvData[]` par chaîne : `sn`, `pv` (pv1..pv4), `pvPower` (W), `energy` (kWh/jour), `totalRatePower` (W), `deviceId`. |
| **`dayEnergy` = somme des `energy`** | Vérifié : 1.27+1.18+1.09+1.10 = **4.64 kWh** (cohérence onduleur ↔ chaînes). |
| **Types de champs cloud** | `deviceId` = **string** (`"12345"`), `totalRatePower` = **float** (`2000.0`), `dayEnergy` dans `dataDtos` = **string avec unité** (`"4.64KWh"`). |
| **`searchTime` requis pour `energy`** | Sans lui `energy` vaut `null` → la librairie extrait la date de `data.lastUpdate`. |
| **Rafraîchissement cloud** | Données réactualisées **~3 min** (`lastUpdate` au format `"YYYY-MM-DD HH:MM:SS"`). |
| **Device page + wifi** | `GET /device/page` → onduleurs (`onlineState`, `rssi`, `pvNum`, `dataDtos`) ; `GET /v3/device/wifi/{sn}` → rssi/ip/wifi. |

### 🟡 Validé hors matériel (simulation / compilation)

- **Wire-format Solarman V5** : validé par `tools/validate_solarman.py` (portage exact du header, décodage 32 bits, signé, température, double-CRC DEYE → « TOUS LES TESTS PASSENT »). **Non testé sur un onduleur Deye réel** car le port 8899 est fermé sur mes iP.
- **Compilation** : les 3 headers + l'exemple `basic.ino` compilent sur ESP32 (`esp32dev`) et ESP8266 (`nodemcuv2`).

### ⏳ Reste à valider

- **`IzypowerCloud.h` sur un ESP réel** : l'exemple `basic.ino` doit afficher les puissances sur la console série (la compilation passe, mais je n'ai pas encore branché le client sur une carte).
- **`IzypowerSolarman.h` sur un onduleur Deye local** : nécessite un appareil avec le port 8899 réellement ouvert (logger *Built-in datalogger* accessible). Impossible à tester sur mes iP.
- **`Izypower.h` sur un Titan battery** : nécessite un appareil Titan exposant le port 8080. Non testé sur mes iP (8080 fermé).

> Si vous validez l'un de ces points sur votre installation, indiquez-le dans une issue/PR : je compléterai ce tableau.

## Caractéristiques

- **Header-only**, sans `.cpp`, compatible **ESP8266** et **ESP32** :
  - `src/IzypowerCloud.h` — client **API cloud Izypower** : login, auto-découverte de la centrale, station (puissance W + énergies jour/mois/an/total + signal + dernier rafraîchissement), onduleurs et **chaînes PV individuelles** (PV1..PV4 en W + kWh du jour). Dépend d'ArduinoJson.
  - `src/IzypowerSolarman.h` — client **Solarman V5** (TCP 8899) + décodeur `DeyeMicro` (registres Deye micro-onduleur, protocole officiel V118). Pas de dépendance externe.
  - `src/Izypower.h` — client **INDEVOLT RPC** (port 8080) : Titan battery / batteries uniquement.

**Compatibilité ESP8266** : tous les clients utilisent `HTTPClient::begin(WiFiClient, url)` avec un `WiFiClient` membre (nécessaire sur ESP8266, où la forme `begin(url)` seule est obsolète). Le client cloud communique en **HTTP simple (port 80, pas de TLS)** — volontairement, pour tenir dans un ESP8266 sans accélération cryptographique.

## Installation (PlatformIO)

```ini
lib_deps =
    https://github.com/Fo170/izypower.git
    bblanchon/ArduinoJson@^7.0.0
```

## Utilisation — API cloud (`IzypowerCloud.h`)

Voie principale des iP (aucun accès local). Client HTTP de `application.izypowercloud.fr` :

```cpp
#include <IzypowerCloud.h>

IzypowerCloud cloud;
cloud.begin("email@compte.fr", "mot_de_passe", 0);   // stationId = 0 → auto-découverte
if (cloud.poll() == IZY_OK) {
    Serial.printf("Station %s : P=%d W, aujourd'hui %.2f kWh\n",
                  cloud.stationName(), (int)cloud.powerW(), cloud.todayKWh());
    for (uint8_t i = 0; i < cloud.inverterCount(); i++) {
        const IzypowerCloudInverter* inv = cloud.inverter(i);
        Serial.printf("  %s : P=%d W, jour %.2f kWh, %u chaînes\n",
                      inv->sn, (int)inv->powerW(), inv->todayKWh(), inv->pvCount);
        for (uint8_t p = 0; p < inv->pvCount; p++)
            Serial.printf("    PV%u : %.1f W / %.2f kWh\n", p + 1,
                          inv->pv[p].powerW, inv->pv[p].energyKWh);
    }
}
```

- `poll()` retourne `IZY_OK` ou une erreur `IZY_ERR_*` (`cloud.lastErrorText()` donne le détail). Le token JWT est géré automatiquement (re-login sur 401/403 ou après 22 h) ; la date du jour (`searchTime`) est dérivée de `data.lastUpdate` pour que `energy` ne soit pas `null`.
- Accesseurs **station** : `stationId()`, `stationName()`, `installedCapacityW()`, `powerW()` (W), `todayKWh()` / `monthKWh()` / `yearKWh()` / `totalKWh()` (kWh), `lastUpdate()`, `signal()` (dBm), `stationOnline()`, `valid()`, `updatedMs()`.
- **Onduleurs** : `inverterCount()`, `inverter(i)`, `find("SN")` → `IzypowerCloudInverter` : `sn`, `deviceId`, `ratedW`, `pvCount`, `pv[i]` (`exists`, `powerW`, `energyKWh`), et les calculs `powerW()` / `todayKWh()`.
- Dépendance : ArduinoJson v7.

**Codes de retour de `poll()`** :

| Code | Signification |
|---|---|
| `IZY_OK` | Données station + onduleurs récupérées |
| `IZY_ERR_LOGIN` | Échec d'authentification cloud (identifiants ?) |
| `IZY_ERR_HTTP` | Statut HTTP ≠ 200 |
| `IZY_ERR_PARSE` | Réponse JSON illisible |
| `IZY_ERR_NO_STATION` | Aucune centrale trouvée sur le compte |
| `IZY_ERR_CONNECT` | Impossible de joindre le cloud (TCP/DNS) |
| `IZY_ERR_TOKEN` | Token refusé (401) malgré le re-login |

**Boucle de polling typique** : le cloud ne se rafraîchit que **~3 min**, un appel toutes les 30-60 s est largement suffisant (et évite de se faire limiter).

```cpp
void loop() {
    static uint32_t last = 0;
    if (millis() - last > 60000) {          // 60 s, le cloud est en ~3 min
        last = millis();
        if (cloud.poll() == IZY_OK) { /* afficher cloud.powerW() … */ }
    }
}
```

## Utilisation — Solarman V5 (`IzypowerSolarman.h`)

Pour onduleurs **Deye locaux uniquement** (ces iP : port 8899 fermé). Lecture Modbus RTU encapsulé (TCP 8899) + décodeur des registres micro-onduleur :

```cpp
#include <IzypowerSolarman.h>

SolarmanV5 sol("192.168.1.100", 1234567890);   // IP + S/N du "Built-in datalogger"
uint16_t regs[DM_REG_COUNT];                    // DM_REG_COUNT = 125
if (sol.readHoldingRegisters(DM_REG_START, DM_REG_COUNT, regs, DM_REG_COUNT)) {
    DeyeMicro m;
    m.decode(regs, DM_REG_COUNT, 2);            // 2 MPPT (iP-1000-W2) / 4 (iP-2000-W4)
    Serial.printf("S/N %s, état %s, P=%d W, PV1=%d W\n",
                  m.serial, DeyeMicro::statusLabel(m.status),
                  (int)m.powerW, (int)m.pvPower[0]);
    Serial.printf("Aujourd'hui %.1f kWh, total %.1f kWh, temp %.1f °C\n",
                  m.todayKWh, m.totalKWh, m.temperature);
}
```

- `readHoldingRegisters(addr, quantity, out, capacity)` : échange court (connexion ouverte puis fermée), max 125 registres, gère heartbeat/handshake et le bug de **double CRC DEYE**.
- `DeyeMicro` (après `decode`) : `serial`, `status` (0 Standby…4 Fault, `statusLabel()`), `ratedPowerW`, `pvVoltage`/`pvCurrent`/`pvPower[4]`, `todayKWh`, `totalKWh`, `gridVoltage`/`gridCurrent`/`gridFreq`, `powerW`, `temperature`, `alarm`, `fault`.
- Constantes de registres `DM_*` dans le header (voir tableau ci-dessous). **Aucune dépendance externe.**

**Remarques de mise en œuvre** :
- **Où trouver le S/N du logger ?** il est imprimé sur l'étiquette du *Built-in datalogger* et retourné par la découverte UDP de `tools/test_local.py` (champ `serial`, 4 octets LE dans la trame V5).
- **Nombre de MPPT à passer à `decode()`** : `2` pour un iP-1000-W2 / SUN1000G3, `4` pour un iP-2000-W4 / SUN2000G3. La lecture des registres ne dépend pas de ce nombre (toujours 125 registres), seul le **décodage** des PV en dépend.
- **Session courte** : le client ouvre puis ferme la connexion à chaque appel. Pour un polling régulier, un appel toutes les 5-30 s est raisonnable ; le temps réel est garanti tant que le port 8899 répond.

## Utilisation — INDEVOLT RPC (`Izypower.h`)

Pour le **Titan battery / batteries uniquement** (ces iP : port 8080 fermé). API HTTP locale `Indevolt.GetData` (registres cJson) :

```cpp
#include <Izypower.h>

Izypower izy("192.168.1.100");                  // port par défaut 8080
JsonDocument doc;
JsonObject out = doc.to<JsonObject>();

static const uint16_t REGISTERS[] = {
    IZP_SERIAL, IZP_PV_POWER_1, IZP_PV_POWER_2,
    IZP_AC_OUTPUT_POWER, IZP_GRID_VOLTAGE, IZP_GRID_FREQUENCY,
};
if (izy.readRegisters(REGISTERS, 6, out)) {
    serializeJsonPretty(out, Serial);
    Serial.printf("PV1 = %d W\n", (int)out[String(IZP_PV_POWER_1)].as<float>());
}

float v;                                       // lecture d'un seul registre
if (izy.readRegister(IZP_PV_POWER_1, v)) Serial.printf("PV1 = %.1f W\n", v);

JsonDocument cfg;                              // configurations (GET)
izy.getConfig(cfg);
izy.getWifiConfig(cfg);                        // → { "sta": { "rssi", "ssid", "ip", ... } }
izy.getCloudStatus(cfg);
```

- `readRegisters(ids[], n, out)` : une seule requête `GetData` → objet JSON avec la clé = registre (string) ; les valeurs `null` (registres non supportés) sont ignorées.
- Constantes de registres `IZP_*` dans le header (S/N 0, PV 1600-1667, AC 2083-2278, réseau 2600-11016…).
- Dépendance : ArduinoJson v7.

## Outils Python (`tools/`)

Trois scripts Python accompagnent la librairie. Ils n'ont **aucune utilité dans votre programme ESP** (les ESP32/ESP8266 n'exécutent pas de Python) mais sont là pour **diagnostiquer votre matériel, valider l'accès et vérifier le protocole** avant / pendant le développement.

### `tools/test_local.py` — quel chemin pour vos onduleurs ?

Détermine, depuis un PC du même réseau, si vos appareils sont accessibles **en local** ou **uniquement via le cloud** (voir le tableau comparatif ci-dessus) :

- scan des ports TCP courants (80/443/502/8080/8899/9090/10000/48899/57000) ;
- découverte UDP Solarman (trame `WIFIKIT-214028-READ` → 48899) ;
- test de lecture réel des registres (si `pysolarmanv5` est installé).

```bash
python3 tools/test_local.py
```

Sur les iP testés, la conclusion est : **aucun port ouvert, aucune réponse UDP → chemin cloud**. Pour tester une vraie lecture Solarman (onduleurs Deye avec logger local) :

```bash
pip install pysolarmanv5
python3 tools/test_local.py --pysolarman --serial <SN_LOGGER>
```

### `tools/cloud_probe.py` — vérifier l'API cloud et ses réponses

Joue le rôle de « sonde » : effectue le flux complet **login → stations → info station → device page → component (PV) → wifi** et **dump les réponses JSON brutes** dans `/tmp/opencode/izypower/dump_cloud/` pour analyse. Idéal pour :
- vérifier que vos identifiants cloud sont valides ;
- inspecter la structure réelle des réponses (les champs peuvent changer côté serveur) ;
- confirmer les valeurs attendues (station, chaînes PV, S/N) avant de les utiliser dans votre programme ESP.

```bash
python3 tools/cloud_probe.py <email> <mot_de_passe>
```

### `tools/validate_solarman.py` — valider le wire-format V5

Portage **exact en Python** du client `IzypowerSolarman.h` : il construit une requête V5, simule une réponse de logger avec des registres connus, puis vérifie le décodage 32 bits, signé, température, S/N, et le bug de **double CRC DEYE**. Aucune dépendance, s'exécute hors-ligne :

```bash
python3 tools/validate_solarman.py   # → « TOUS LES TESTS PASSENT »
```

**À relancer à chaque modification du framing/décodage de `IzypowerSolarman.h`** pour s'assurer que le portage reste cohérent.

## API cloud Izypower (résumé des endpoints)

Base : `http://application.izypowercloud.fr/photo_voltaic/api` — headers `app-platform: izy` + `x-tts-access-token` (token JWT).

| Endpoint | Données |
|---|---|
| `POST /login` | `data.token` (JWT) |
| `GET /powerStations/page?page=1&limit=100` | `data.records[].stationsId` (auto-découverte) |
| `GET /v3/powerStations/info/{id}` | `power` (W), `data.{todaySum,monthSum,yearSum,allSum}` (kWh), `data.{stationName,installedCapacity,lastUpdate,signal,onlineState}` |
| `GET /component/{id}?searchTime=YYYY-MM-DD` | `pvData[]` : `sn`, `pv` (pv1..pv4), `pvPower` (W), `energy` (kWh/jour), `totalRatePower` (W), `deviceId` |
| `GET /v3/device/wifi/{sn}` | `rssi`, `wifi`, `ip` |
| `GET /device/page?powerId={id}&deviceType=all` | onduleurs : `onlineState`, `rssi`, `pvNum`, `dataDtos` |

`searchTime` est optionnel (données du jour) mais sans lui `energy` vaut `null` : `IzypowerCloud` extrait donc la date de `data.lastUpdate` de la station.

## Registres micro-onduleur (Deye, Solarman V5, holding fc 0x03, 0-based)

Lecture unique `0x0001..0x007D`. Valeurs **32 bits = mot faible d'abord** ; temp. = (brut − 1000) × 0,01.

| Adresse | Signification | Unités |
|---|---|---|
| 0x0003–0x0007 | S/N micro-onduleur (ASCII) | — |
| 0x0010 | Puissance nominale | W × 0,1 |
| 0x003B | État (0 Standby, 1 Self-test, 2 Normal, 3 Alarme, 4 Défaut) | — |
| 0x003C | Production du jour | kWh × 0,1 |
| 0x003F–0x0040 | Production totale (32 bits) | kWh × 0,1 |
| 0x0049 / 0x004C / 0x004F | Tension / courant / fréquence AC | V/A/Hz (×0,1/×0,1/×0,01) |
| 0x0056–0x0057 | Puissance AC active (32 bits) | W × 0,1 |
| 0x005A | Température | °C |
| 0x0065–0x0066 | Alarme (bits) | — |
| 0x0067–0x006A | Défaut (bits) | — |
| 0x006D/6F/71/73 | Tension PV 1..4 | V × 0,1 |
| 0x006E/70/72/74 | Courant PV 1..4 | A × 0,1 |

Constantes complètes dans `src/IzypowerSolarman.h` (`DM_*`).

## Dépendances

- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) `^7.0.0` (client cloud + INDEVOLT) — le client Solarman V5 n'en a pas besoin.

## Bibliothèques et ressources utiles

Ces bibliothèques, dans le même domaine (onduleurs Deye/micro-onduleurs, protocole Solarman, monitoring solaire), vous seront utiles si vous allez plus loin que cette librairie :

- **Protocole / framings**
  - [jmccrohan/pysolarmanv5](https://github.com/jmccrohan/pysolarmanv5) — implémentation Python de référence du protocole Solarman V5 (framing, heartbeat, double-CRC DEYE). Incontournable pour comprendre la trame.
  - [xirt/indevolt-api](https://github.com/xirt/indevolt-api) — API Python du framework INDEVOLT/OpenData (port 8080), modèle du client `Izypower.h`.
- **Intégrations Home Assistant (cartes de registres Deye)**
  - [StephanJoubert/home_assistant_solarman](https://github.com/StephanJoubert/home_assistant_solarman) — profils `deye_2mppt.yaml` / `deye_4mppt.yaml` (sources des registres).
  - [davidrapan/ha-solarman](https://github.com/davidrapan/ha-solarman) — profil `deye_micro.yaml`.
  - [SchulteDev/deye-microinverter-power-limit](https://github.com/SchulteDev/deye-microinverter-power-limit) — limitation de puissance des micro-onduleurs Deye.
- **API cloud Izypower (implémentations Python)**
  - [khirale/izypower_cloud](https://github.com/khirale/izypower_cloud), [StefanPlizga/izypower_cloud](https://github.com/StefanPlizga/izypower_cloud) — accès à l'API `application.izypowercloud.fr` (structuration des réponses vérifiée sur matériel avec `tools/cloud_probe.py`).
- **Titan battery**
  - [khirale/izypower_titan](https://github.com/khirale/izypower_titan) — lecture INDEVOLT RPC du Titan.
- **Arduino / ESP**
  - [bblanchon/ArduinoJson](https://github.com/bblanchon/ArduinoJson) `^7.0.0` — dépendance des clients cloud et INDEVOLT.

Sources protocolaires d'origine : **protocole officiel Deye Modbus V118** (PDF), carte micro-onduleur.

## Licence

GPL-3.0-only — Olivier FOURNET (`Fo170`).
