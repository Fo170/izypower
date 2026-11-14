# Izypower

Monitoring des **appareils IZYPOWER** (Materfrance) : **micro-onduleurs iP-1000-W2** (1000 W, 2 MPPT) / **iP-2000-W4** (2000 W, 4 MPPT) et **Smart Meter iP-SmartIA-W3** (import/injection réseau, conso foyer, borne EV).

⚠️ **Accès aux données (vérifié sur matériel avec `tools/test_local.py`)** : ces appareils (micro-onduleurs iP et Smart Meter iP-SmartIA-W3) sont des **Deye** (module WiFi WE-P2) mais **n'exposent NI l'API INDEVOLT RPC (8080) NI SOLARMAN V5 (8899)** en local : aucun port TCP ouvert, aucune réponse UDP Solarman. Ils sont **100 % orientés cloud**. La lecture passe donc par l'**API cloud Izypower** (`application.izypowercloud.fr`) : login → token `x-tts-access-token`, données rafraîchies **toutes les ~3 min** (pas de temps réel).

Deux niveaux de lecture sont disponibles par cette API :

| Niveau | Ce que l'on obtient | Méthodes |
|---|---|---|
| **État** (instantané + totaux) | puissance, énergies jour/mois/an/total, chaînes PV, métrologie Smart Meter, appareils | `poll()` + getters |
| **Historique** (courbes) | séries horodatées production / consommation / réseau / PV, par jour, mois, année ou tout l'historique, et **valeur interpolée à un instant quelconque** | `fetchStationHistory()`, `fetchPvHistory()`, `fetchDeviceHistory()`, `historyValueAt()` |

Trois clients header-only sont fournis, selon le matériel :

| Client | Matériel | Protocole |
|---|---|---|
| `IzypowerCloud.h` | iP-1000-W2 / iP-2000-W4 / **iP-SmartIA-W3** (**voie principale**) | API cloud `application.izypowercloud.fr` |
| `IzypowerSolarman.h` | onduleurs Deye exposant un logger local | Solarman V5, TCP 8899, Modbus RTU |
| `Izypower.h` | Titan battery / batteries | INDEVOLT RPC, port 8080 |

## Accès aux données selon le type d'appareil

Tous les appareils de la gamme ne sont **pas accessibles de la même façon** : selon le matériel, la lecture passe par l'API cloud, par un protocole local, ou est simplement **impossible en local**. Ce tableau récapitule ce qui est possible, vérifié sur matériel.

| Appareil | Accès local | Accès cloud | Client à utiliser | Remarques |
|---|---|---|---|---|
| **iP-1000-W2** (1000 W, 2 MPPT) | ❌ **Aucun** (8080, 8899, 502, 80… fermés ; pas d'UDP Solarman) | ✅ API Izypower (~3 min) | `IzypowerCloud.h` | Module WiFi WE-P2 = Deye, mais **100 % cloud** |
| **iP-2000-W4** (2000 W, 4 MPPT) | ❌ **Aucun** (idem) | ✅ API Izypower (~3 min) | `IzypowerCloud.h` | Idem |
| **iP-SmartIA-W3** (Smart Meter / Passerelle IA) | ❌ **Aucun** (cloud uniquement) | ✅ API Izypower (~3 min) | `IzypowerCloud.h` | **Testé et fonctionnel avec la librairie** : import/injection réseau, conso foyer, borne EV, tension/fréquence, RSSI (via `device/page` + top-level `grid_power`/`consumption`/`ev_power`) |
| **Micro-onduleurs Deye SUN\*** avec logger local | ✅ **Solarman V5** (TCP 8899) | — | `IzypowerSolarman.h` | Temps réel si le port 8899 répond |
| **Titan battery / batteries** | ✅ **INDEVOLT RPC** (TCP 8080) | — | `Izypower.h` | API HTTP locale `Indevolt.GetData` |

**Pourquoi ?** Les appareils iP utilisent le module WiFi **Deye WE-P2** qui ne répond à **aucun port TCP** ni à la **découverte UDP Solarman** (48899) : impossible d'y accéder autrement que par le cloud du fabricant. Seuls les onduleurs Deye dotés d'un *Built-in datalogger* (port 8899 ouvert) et les appareils Titan (port 8080) sont lisibles en local.

**Comment déterminer le bon chemin sur votre installation ?** → exécutez `python3 tools/test_local.py` (voir section [Outils Python](#outils-python)) : il scanne les ports TCP et fait la découverte UDP Solarman pour vous dire quel protocole répond.

## État de la validation sur matériel

Ce qui a été **réellement testé** sur mon installation (un iP-1000-W2, un iP-2000-W4 et un Smart Meter iP-SmartIA-W3), et ce qui **reste à valider** par vos soins :

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
| **Device page + wifi** | `GET /device/page` → onduleurs **et Smart Meter** (`sn`, `deviceType`, `device` modèle `"VMP2"`/`"VMP4"`/`"Meter"`, `pvNum` 0 pour le Meter, `mac`, `softwareVersion`, `onlineState`, `rssi`, `dataDtos`) ; `GET /v3/device/wifi/{sn}` → rssi/ip/wifi. |
| **Métrologie Smart Meter** | `v3/powerStations/info` en top-level → `grid_power` (import + / injection −), `consumption`, `ev_power`. Sur matériel : `v_ac=236.3V`, `freq=49.97Hz`, import/export cumulés, rssi −48 dBm via `device/page` (iP-SmartIA-W3). |
| **`IzypowerCloud.h` sur ESP réel (avec Smart Meter)** | Client **testé et fonctionnel sur matériel** (ESP8266, firmware passerelle) : `/json` → **3 appareils auto-listés** (Smart Meter iP-SmartIA-W3 `"Meter"`, onduleurs `ip-2000-w4` `"VMP4"` et `ip-1000-w2` `"VMP2"`), métrologie réseau/foyer/EV cohérente avec la consommation réelle. L'exemple `basic.ino` compile sur les deux envs (à flasher pour reproduire). |
| **Historique `report/*`** (relevés réels du 04/11/2026) | `fetchStationHistory(day)` → 443 à 480 points/jour (`production`/`consumption`/`grid` en W), `month`/`year`/`all` en kWh ; `fetchPvHistory(day)` → **8 séries** `pvN_volt`/`pvN_curr` (207 points) ; `fetchDeviceHistory(meter, "v_ac,a_ac")` → 2 séries. `searchTime` **obligatoire** et **formaté selon la granularité** (`code 500` sinon). |
| **Fuseaux** | les points d'historique sont en **heure locale du site** (dernier point `22:54` pour `22:56 CET` réels) ; `lastUpdate` est en **heure serveur UTC+8** (`2026-11-05 05:32` à 22:36 CET) → ne pas en déduire la date de recherche. |
| **Interpolation linéaire** (`valueAt`) | Valeurs recalculées à 08:00 / 12:00 / 14:30 / 21:15 sur une journée réelle (13,00 / 858,90 / 375,90 / 2,60 W) ; le milieu d'un segment redonne exactement la moyenne des deux points. |

### 🟡 Validé hors matériel (simulation / compilation)

- **Wire-format Solarman V5** : validé par `tools/validate_solarman.py` (portage exact du header, décodage 32 bits, signé, température, double-CRC DEYE → « TOUS LES TESTS PASSENT »). **Non testé sur un onduleur Deye réel** car le port 8899 est fermé sur mes iP.
- **Décodage de l'historique** : `tools/validate_history.py` (dates, granularités partielles, deux formes de réponse, décimation, interpolation et ses cas limites → « TOUS LES TESTS PASSENT »).
- **Compilation** : les 3 headers + les exemples `basic.ino` et `history.ino` compilent sur ESP32 (`esp32dev`) et ESP8266 (`nodemcuv2`).

### ⏳ Reste à valider

- **`IzypowerSolarman.h` sur un onduleur Deye local** : nécessite un appareil avec le port 8899 réellement ouvert (logger *Built-in datalogger* accessible). Impossible à tester sur mes iP.
- **`Izypower.h` sur un Titan battery** : nécessite un appareil Titan exposant le port 8080. Non testé sur mes iP (8080 fermé).

> Si vous validez l'un de ces points sur votre installation, indiquez-le dans une issue/PR : je compléterai ce tableau.

## Caractéristiques

- **Header-only**, sans `.cpp`, compatible **ESP8266** et **ESP32** :
  - `src/IzypowerCloud.h` — client **API cloud Izypower** : login, auto-découverte de la centrale, station (puissance W + énergies jour/mois/an/total + signal + dernier rafraîchissement), onduleurs et **chaînes PV individuelles** (PV1..PV4 en W + kWh du jour), + **métrologie Smart Meter iP-SmartIA-W3** (import/injection réseau, conso foyer, borne EV, tension/fréquence via `device/page`), + **historique** (courbes `report/*` : séries production/consommation/réseau/PV par jour, mois, année ou tout l'historique, décimation configurable, **interpolation linéaire** pour une valeur à un instant donné). Dépend d'ArduinoJson. **Accès 100 % par getters.**
  - `src/IzypowerSolarman.h` — client **Solarman V5** (TCP 8899) + décodeur `DeyeMicro` (registres Deye micro-onduleur, protocole officiel V118). Pas de dépendance externe.
  - `src/Izypower.h` — client **INDEVOLT RPC** (port 8080) : Titan battery / batteries uniquement.

**Compatibilité ESP8266** : tous les clients utilisent `HTTPClient::begin(WiFiClient, url)` avec un `WiFiClient` membre (nécessaire sur ESP8266, où la forme `begin(url)` seule est obsolète). Le client cloud communique en **HTTP simple (port 80, pas de TLS)** — volontairement, pour tenir dans un ESP8266 sans accélération cryptographique.

## Installation (PlatformIO)

```ini
lib_deps =
    https://github.com/Fo170/izypower.git
    bblanchon/ArduinoJson@^7.0.0
```

## Communication avec le cloud Izypower

Cette section décrit **exactement** ce que la librairie envoie et reçoit. Tout
est vérifié sur matériel (compte réel, station `Fo170`) — pas d'extrapolation
depuis la documentation.

### Transport

| Élément | Valeur |
|---|---|
| Base | `http://application.izypowercloud.fr/photo_voltaic/api` |
| Schéma | **HTTP simple, port 80** — pas de TLS (choix assumé pour tenir sur un ESP8266 sans accélération cryptographique) |
| Client | `HTTPClient::begin(WiFiClient&, url)` avec un `WiFiClient` membre (forme obligatoire sur ESP8266) |
| Timeout | 10 s (`_httpTimeoutMs`) |
| Encodage | JSON UTF-8 |

### En-têtes envoyés

| En-tête | Valeur | Quand |
|---|---|---|
| `app-platform` | `izy` | **toutes** les requêtes (le cloud refuse sans) |
| `x-tts-access-token` | JWT renvoyé par `/login` | toutes les requêtes authentifiées |
| `Accept-Language` | `fr-FR,fr;q=0.9` | toutes |
| `Content-Type` | `application/json` | `POST /login` uniquement |

### Enveloppe de réponse

```json
{ "code": 200, "msg": "…", "data": { … } }
```

C'est **`code` dans le corps** qui décide, pas le statut HTTP. Valeurs
rencontrées :

| `code` | Sens | Comportement de la librairie |
|---|---|---|
| `200` | succès | `data` est exploité |
| `401` | jeton invalide/expiré | re-login automatique puis **un** nouvel essai |
| `500` | erreur applicative, `msg` explicite | remonté en `IZY_ERR_HTTP` |

Deux `msg` de `code 500` observés et neutralisés en amont :

- `Erreur de format de date` → `searchTime` trop précis pour la granularité ;
- `veuillez vous connecter` → jeton absent sur un endpoint `report/*`.

### Session

```
POST /login   {"username": "<email>", "password": "<mot de passe>"}
              → data.token
```

Le jeton est conservé en mémoire (jamais sur disque), renvoyé dans
`x-tts-access-token`, et renouvelé automatiquement :

- après **22 h** d'ancienneté (`_loginAtMs`) ;
- sur `401`/`403` (un nouvel essai, pas de boucle) ;
- avant tout appel `report/*` via `_ensureLogin()`.

### Fraîcheur et débit

| | Valeur |
|---|---|
| Rafraîchissement des données côté cloud | **~3 min** (observé sur `lastUpdate`) |
| Fréquence d'appel recommandée | 30 à 60 s pour `poll()` |
| Historique | à recharger seulement quand l'écran/l'usage le demande |
| Limite de débit | non documentée ; l'app mobile ne dépasse pas un appel par écran |

### Fuseau horaire — point critique

L'API mélange **deux fuseaux**. Vérifié en comparant à l'heure NTP réelle :

| Donnée | Fuseau | Preuve |
|---|---|---|
| `time` des points d'historique, `searchTime` | **heure locale du site** (Europe/Paris) | dernier point `22:54` pour `22:56 CET` réels ; production de 07:12 à la tombée du jour |
| `lastUpdate` (`v3/powerStations/info`) | **heure serveur, UTC+8** | à `22:36 CET`, le cloud annonçait `2026-11-05 05:32` |

Conséquences :

| Point | Détail |
|---|---|
| `epoch()` | heure locale du site « comme si » c'était UTC |
| `epochUtc(offset)` | heure UTC réelle ; `offset` = décalage local (1 h en hiver, 2 h en été — `IZY_LOCAL_UTC_OFFSET_HOURS`) |
| Date par défaut de `searchTime` | **jour local**, lu sur l'horloge système (`localtime_r`) |
| `lastUpdate` | **ne sert pas** à dériver la date : il donnerait le jour du serveur, décalé d'un jour en soirée |
| Sans horloge synchronisée | repli sur la date de `lastUpdate`, potentiellement décalée d'un jour |

C'est ce qui explique la « courbe du jour vide » observée auparavant : la
librairie demandait `2026-11-05` (jour du serveur) au lieu de `2026-11-04`
(jour local). Corrigé : la journée locale se charge désormais correctement.

### Les huit endpoints utilisés

| Méthode | Endpoint | Paramètres | Réponse exploitée |
|---|---|---|---|
| `POST` | `/login` | corps `{username, password}` | `data.token` |
| `GET` | `/powerStations/page` | `page=1&limit=100` | `data.records[].stationsId` |
| `GET` | `/v3/powerStations/info/{stationsId}` | — | `power`, `data.{todaySum,monthSum,yearSum,allSum,stationName,installedCapacity,lastUpdate,signal,onlineState}` + top-level `grid_power`, `consumption`, `ev_power` |
| `GET` | `/component/{stationsId}` | `searchTime` | `pvData[]` : `sn`, `pv`, `pvPower`, `energy`, `totalRatePower`, `deviceId` |
| `GET` | `/device/page` | `powerId`, `deviceType=all`, `page`, `limit` | `data.records[]` : `sn`, `deviceType`, `device`, `pvNum`, `mac`, `softwareVersion`, `deviceId`, `onlineState`, `rssi`, `dataDtos[]` |
| `GET` | `/report/v2/powerStations/data/{stationsId}` | `timeType`, `dataFlag=energy`, `searchTime`, `showSoc` | `data[]` = séries (voir ci-dessous) |
| `GET` | `/report/izy/device/data/{deviceSn}` | `timeType`, `queryType=pv`, `searchTime` | `data[]` = séries |
| `GET` | `/report/device/data/{deviceSn}` | `timeType`, `dataFlag` (CSV), `searchTime` | `data[]` = séries |

### Forme d'une série

```json
{ "key": "production", "name": "Production", "unit": "W", "lineType": "line",
  "data": [ { "time": "2026-11-04 14:30:00", "val": 375.9 }, … ] }
```

| Champ | Remarque |
|---|---|
| `data` (racine) | **tableau de séries** directement (pas `data.data`) — les deux formes sont acceptées |
| `key` | `production`, `consumption`, `grid`, `pvN_volt`, `pvN_curr`, `energy`, ou la valeur du `dataFlag` |
| `grid` | peut apparaître **deux fois** (Importation / Injection) |
| `unit` | `W` en `day`, `kWh`/`KWh` en `month`/`year`/`all` |
| `val` | peut être `null` → converti en 0 ; `value` accepté en repli |
| `time` | **heure locale du site** ; `"YYYY-MM-DD HH:MM:SS"` en `day`, `"YYYY-MM-DD"` en `month`, `"YYYY-MM"` en `year`, `"YYYY"` en `all` |

## Utilisation — API cloud (`IzypowerCloud.h`)

Voie principale des iP et du Smart Meter (aucun accès local). Client HTTP de `application.izypowercloud.fr`, **accès 100 % par getters** (champs privés) :

```cpp
#include <IzypowerCloud.h>

IzypowerCloud cloud;
cloud.begin("email@compte.fr", "mot_de_passe", 0);   // stationId = 0 → auto-découverte
if (cloud.poll() == IZY_OK) {
    Serial.printf("Station %s : P=%d W, aujourd'hui %.2f kWh\n",
                  cloud.stationName(), (int)cloud.productionW(), cloud.productionKwhToday());
    // Métrologie Smart Meter (top-level de v3/powerStations/info).
    Serial.printf("Réseau : import %d W (négatif = injection) | foyer %d W | EV %d W\n",
                  (int)cloud.gridImportW(), (int)cloud.houseConsumptionW(), (int)cloud.evChargeW());
    for (uint8_t i = 0; i < cloud.inverterCount(); i++) {
        const IzypowerCloudInverter* inv = cloud.inverter(i);
        Serial.printf("  %s : P=%d W, jour %.2f kWh, %u chaînes\n",
                      inv->serialNumber(), (int)inv->productionW(),
                      inv->productionKwhToday(), inv->pvCount());
        for (uint8_t p = 0; p < inv->pvCount(); p++)
            Serial.printf("    PV%u : %.1f W / %.2f kWh\n", p + 1,
                          inv->pvPowerW(p), inv->pvEnergyKwh(p));
    }
    // Appareils non-PV (device/page) : Smart Meter iP-SmartIA-W3, batterie, EV…
    for (uint8_t i = 0; i < cloud.deviceCount(); i++) {
        const IzypowerCloudDevice* dev = cloud.device(i);
        Serial.printf("  %s (modèle %s, %u chaîne(s) PV) : RSSI %d dBm\n",
                      dev->serialNumber(), dev->model(), dev->pvNum(), (int)dev->rssiDbm());
    }
}
```

- `poll()` retourne `IZY_OK` ou une erreur `IZY_ERR_*` (`cloud.lastErrorText()` donne le détail). Le token JWT est géré automatiquement (re-login sur 401/403 ou après 22 h) ; la date du jour (`searchTime`) est dérivée de `data.lastUpdate` pour que `energy` ne soit pas `null`. `poll()` appelle aussi `device/page` (**best-effort** : une erreur sur cet appel n'empêche pas la lecture PV).
- Accesseurs **station** : `stationId()`, `stationName()`, `installedCapacityW()`, `productionW()` (W), `productionKwhToday()` / `productionKwhMonth()` / `productionKwhYear()` / `productionKwhTotal()` (kWh), `gridImportW()` (import + / injection −), `houseConsumptionW()`, `evChargeW()`, `lastUpdate()`, `signalDbm()` (dBm), `stationOnline()`, `valid()`, `updatedMs()`.
- **Onduleurs** : `inverterCount()`, `inverter(i)`, `find("SN")` → `IzypowerCloudInverter` : `serialNumber()`, `deviceId()`, `ratedPowerW()`, `pvCount()`, `pv(i)` (`connected()`, `powerW()`, `energyKwh()`), et les calculs `productionW()` / `productionKwhToday()`.
- **Appareils (device/page)** : `deviceCount()`, `device(i)`, `findDevice("SN")` → `IzypowerCloudDevice` : `serialNumber()`, `type()`, `model()` (`"VMP2"`/`"VMP4"`/`"Meter"`), `pvNum()` (**0 pour le Smart Meter**), `macAddress()`, `firmware()`, `deviceId()`, `online()`, `rssiDbm()`, `dtoCount()`, `dto(i)`, `dtoValue(key)` (ex. `"v_ac"` → `"237.2V"`, `""` si absent).
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
        if (cloud.poll() == IZY_OK) { /* afficher cloud.productionW() … */ }
    }
}
```

## Utilisation — Historique (courbes)

Les courbes affichées par l'application mobile ne viennent **pas** de
`v3/powerStations/info` : elles sont servies par les endpoints `report/*`.
`IzypowerCloud` les récupère avec la même session que `poll()` (login et jeton
partagés), et les expose par getters.

### Les trois fonctions

| Fonction | Endpoint appelé | Ce qu'elle renvoie |
|---|---|---|
| `fetchStationHistory(range, searchTime, showSoc)` | `/report/v2/powerStations/data/{id}` | production, consommation, réseau, (batterie) de la centrale |
| `fetchPvHistory(deviceSn, range, searchTime)` | `/report/izy/device/data/{sn}?queryType=pv` | chaînes PV d'un onduleur |
| `fetchDeviceHistory(deviceSn, range, searchTime, dataFlag)` | `/report/device/data/{sn}` | un signal par `dataFlag` (Smart Meter, onduleur…) |

```cpp
int fetchStationHistory(IzyHistoryRange range = IZY_RANGE_DAY,
                        const char* searchTime = nullptr,
                        bool showSoc = true);

int fetchPvHistory(const char* deviceSn,
                   IzyHistoryRange range = IZY_RANGE_DAY,
                   const char* searchTime = nullptr);

int fetchDeviceHistory(const char* deviceSn,
                       IzyHistoryRange range = IZY_RANGE_DAY,
                       const char* searchTime = nullptr,
                       const char* dataFlag = "energy");
```

### Paramètres

| Paramètre | Type | Défaut | Rôle |
|---|---|---|---|
| `range` | `IzyHistoryRange` | `IZY_RANGE_DAY` | granularité (voir tableau ci-dessous) |
| `searchTime` | `const char*` | `nullptr` | date de référence ; `nullptr` → celle de `lastUpdate`, lue automatiquement si `poll()` n'a pas tourné |
| `showSoc` | `bool` | `true` | ajoute la série batterie quand elle existe |
| `dataFlag` | `const char*` | `"energy"` | signal demandé ; **liste CSV acceptée** (`"v_ac,a_ac"` → deux séries) |
| `deviceSn` | `const char*` | — | numéro de série (`55180000F6CF`, `981100005C6C`…) |

### Granularités (`IzyHistoryRange`)

| Constante | `timeType` | `searchTime` réellement envoyé | Contenu observé |
|---|---|---|---|
| `IZY_RANGE_DAY` | `day` | `2026-11-04` | 440 à 480 points/jour (~3 min), unité **W** |
| `IZY_RANGE_MONTH` | `month` | `2026-11` | 1 point par jour, unité **kWh** |
| `IZY_RANGE_YEAR` | `year` | `2026` | 1 point par mois, unité **kWh** |
| `IZY_RANGE_ALL` | `all` | date libre (ignorée) | 1 point, cumuls **kWh** |

La troncature est faite par la librairie (`_normalizeDate()`). Envoyer
`2026-11-04` avec `month` provoque `code 500 — Erreur de format de date`.

### Codes de retour

Identiques à `poll()` (`cloud.lastErrorText()` pour le libellé) :

| Code | Sens dans un appel d'historique |
|---|---|
| `IZY_OK` | séries chargées (même si certaines sont **vides**) |
| `IZY_ERR_LOGIN` | re-login impossible avant l'appel |
| `IZY_ERR_CONNECT` | cloud injoignable (DNS/TCP) |
| `IZY_ERR_HTTP` | statut ≠ 200, ou `code` ≠ 200 dans le corps |
| `IZY_ERR_PARSE` | JSON illisible, ou `data` sans tableau de séries |
| `IZY_ERR_NO_STATION` | `stationId` introuvable (station) |
| `IZY_ERR_TOKEN` | jeton refusé malgré le re-login |

### Séries réellement renvoyées

| Appel | Séries |
|---|---|
| `fetchStationHistory(day)` | `production`, `consumption`, `grid` (W) |
| `fetchStationHistory(month/year/all)` | mêmes clés en kWh ; `grid` apparaît **deux fois** (Importation / Injection) |
| `fetchPvHistory(sn, day)` | `pv1_volt`…`pv4_volt`, `pv1_curr`…`pv4_curr` → **8 séries** |
| `fetchPvHistory(sn, month/year)` | `energy` (kWh) |
| `fetchDeviceHistory(meterSn, day, "v_ac,a_ac")` | `v_ac`, `a_ac` (une série par flag) |

### Lire le résultat

```cpp
cloud.fetchStationHistory(IZY_RANGE_DAY, "2026-11-04");

// accès par index
for (uint8_t s = 0; s < cloud.historySeriesCount(); s++) {
    const IzypowerCloudSeries* serie = cloud.historySeries(s);
    for (uint16_t p = 0; p < serie->pointCount(); p++) {
        const IzypowerCloudPoint* pt = serie->point(p);
        Serial.printf("%s %.2f %s @ %lu\n", serie->key(), pt->value(),
                      serie->unit(), (unsigned long)pt->epoch());
    }
}

// accès par clé (nullptr si absente)
const IzypowerCloudSeries* prod = cloud.findSeries("production");
```

| Objet | Getter | Renvoie |
|---|---|---|
| `IzypowerCloud` | `historySeriesCount()` | nombre de séries conservées |
| | `historySeries(i)` | `const IzypowerCloudSeries*` ou `nullptr` |
| | `findSeries("production")` | `const IzypowerCloudSeries*` ou `nullptr` |
| | `historyRange()` | granularité du dernier appel |
| | `historySearchTime()` | `searchTime` effectivement utilisé |
| | `historyUpdatedMs()` | `millis()` du dernier chargement |
| `IzypowerCloudSeries` | `key()`, `name()`, `unit()`, `lineType()` | chaînes |
| | `pointCount()` | points **conservés** |
| | `pointTotal()` | points **reçus** du cloud |
| | `decimated()` | `true` si `pointTotal > pointCount` |
| | `point(i)` | `const IzypowerCloudPoint*` ou `nullptr` |
| | `valueAt(epoch, &couvert)` | valeur interpolée |
| `IzypowerCloudPoint` | `epoch()` | secondes Unix, **heure du cloud** |
| | `epochUtc(offset)` | secondes Unix, UTC réel (retire le décalage local, 1 h hiver / 2 h été) |
| | `value()` | valeur mesurée (`float`) |

### Valeur à un instant quelconque

Le cloud ne sait interroger que des **périodes**. Pour une valeur à un instant
précis, on charge la période contenante puis on **interpole linéairement** entre
les deux points encadrants :

```cpp
cloud.fetchStationHistory(IZY_RANGE_DAY, "2026-11-04");      // 1. charger la journée

uint32_t t = IzypowerCloud::toCloudEpoch("2026-11-04 14:30"); // 2. cibler l'instant
bool couvert = false;
float watts = cloud.historyValueAt("production", t, &couvert); // 3. interpoler

// équivalent, sur une série déjà obtenue :
float w2 = cloud.historySeries(0)->valueAt(t, &couvert);
```

| Fonction | Rôle |
|---|---|
| `IzypowerCloud::toCloudEpoch("YYYY-MM-DD HH:MM[:SS]")` | horodatage (heure du cloud) → epoch ; accepte aussi `"YYYY-MM-DD"`, `"YYYY-MM"`, `"YYYY"` ; `0` si invalide |
| `IzypowerCloudSeries::valueAt(epoch, bool* couvert = nullptr)` | interpolation linéaire sur la série |
| `IzypowerCloud::historyValueAt(clé, epoch, bool* couvert = nullptr)` | `findSeries(clé)` + `valueAt()` ; `0` si la clé n'existe pas |

Comportement de `*couvert` :

| Situation | Valeur renvoyée | `*couvert` |
|---|---|---|
| instant entre deux points | interpolation linéaire | `true` |
| instant égal à un point | valeur du point | `true` |
| instant avant le premier point | valeur du premier point (**pas d'extrapolation**) | `false` |
| instant après le dernier point | valeur du dernier point | `false` |
| série vide | `0` | `false` |
| série à un seul point | sa valeur si l'instant correspond, sinon la valeur | `true`/`false` |
| segment dégénéré ou `epoch` = 0 | valeur du point suivant | `true` |

**Précision** : elle suit la décimation. Avec `IZY_MAX_POINTS=96` sur une
journée de 440 points, l'écart entre points conservés est d'environ 15 min ;
pour un pas de 3 min, compiler avec `-DIZY_MAX_POINTS=288` (ou 480).

Vérification sur données réelles (relevés du 04/11/2026) :

```
2026-11-04 08:00 : production=13.00   consumption=886.00   grid=873.00
2026-11-04 12:00 : production=858.90  consumption=2043.90  grid=1185.00
2026-11-04 14:30 : production=375.90  consumption=4499.90  grid=4124.00
2026-11-04 21:15 : production=2.60    consumption=951.60   grid=949.00
controle milieu de segment : OK (moyenne exacte des deux points)
```

### Lecture en flux (mémoire)

Le corps d'une réponse `day` pèse **75 à 90 Ko** : impossible à loger dans le
tas d'un ESP32 (≈50 Ko libres sur un firmware chargé), et une telle allocation
fragmente le tas. Les trois fonctions d'historique lisent donc la réponse **en
flux**, octet par octet, avec une machine à états minimale :

- aucune copie du corps, aucune désérialisation ArduinoJson pour cette partie ;
- empreinte ≈ 100 o de pile + le buffer de séries ;
- décimation appliquée **pendant** la lecture : on remplit `IZY_MAX_POINTS`,
  puis on divise par deux et on double le pas (adaptatif, quelle que soit la
  taille de la réponse) ; le dernier point reçu est toujours conservé ;
- `pointTotal()` reste le nombre réellement reçu du cloud.

Disposition du JSON exploitée (vérifiée) : dans chaque série, `data` précède
`key`/`unit`/`name`/`lineType` — les points sont accumulés puis étiquetés à
l'arrivée de la clé, qui clôt la série.

### Mémoire et décimation

Chaque appel **remplace** le buffer de séries (pas d'accumulation).

| Macro | Défaut | Effet | Surcharge |
|---|---|---|---|
| `IZY_MAX_SERIES` | `8` | séries conservées (8 = historique PV en `day`) | `-DIZY_MAX_SERIES=4` |
| `IZY_MAX_POINTS` | `96` | points par série | `-DIZY_MAX_POINTS=288` |
| `IZY_LOCAL_UTC_OFFSET_HOURS` | `1` | décalage local↔UTC pour `epochUtc()` (1 h hiver, 2 h été) | `-DIZY_LOCAL_UTC_OFFSET_HOURS=2` |

Au-delà de `IZY_MAX_POINTS`, les points sont **décimés à pas régulier**, en
conservant toujours le premier et le dernier ; `pointTotal()` donne le nombre
réellement reçu et `decimated()` signale la réduction (ex. 452 reçus → 92
conservés).

Coût mesuré sur ESP8266 avec `basic.ino` : **+6 776 octets** de RAM
(43,9 % → 52,2 %), flash quasi inchangé (+48 o).

### Exemple complet

```cpp
#include <IzypowerCloud.h>

IzypowerCloud cloud;

void setup() {
    cloud.begin("email@compte.fr", "mot_de_passe", 0);
    if (cloud.poll() != IZY_OK) return;                 // état courant + date de référence

    // Courbes du jour de la centrale
    if (cloud.fetchStationHistory(IZY_RANGE_DAY) == IZY_OK) {
        const IzypowerCloudSeries* p = cloud.findSeries("production");
        if (p && p->pointCount()) {
            Serial.printf("production %s : %.2f -> %.2f %s (%u/%u points)\n",
                          cloud.historySearchTime(),
                          p->point(0)->value(),
                          p->point(p->pointCount() - 1)->value(),
                          p->unit(), p->pointCount(), p->pointTotal());
        }
    }

    // Valeur à un instant précis
    uint32_t t = IzypowerCloud::toCloudEpoch("2026-11-04 14:30");
    bool ok = false;
    Serial.printf("a 14:30 : %.1f W\n", cloud.historyValueAt("production", t, &ok));
}

void loop() {}
```

### Pièges à connaître

| Piège | Détail |
|---|---|
| Courbe du **jour en cours** | peut être **vide** tant que le cloud ne l'a pas matérialisée (constaté à 05 h 10 : `IZY_OK` mais 0 point). Tester `pointCount()` avant d'afficher. |
| `searchTime` obligatoire | sans lui, `code 500`. La librairie lit `lastUpdate` automatiquement. |
| Format de date | lié à la granularité ; la librairie tronque, ne pas le faire soi-même avec un format local. |
| Fuseau | les points sont en heure locale du site, mais `lastUpdate` est en heure serveur (UTC+8) : ne pas en déduire la date. |
| `grid` en double | Importation et Injection partagent la même clé ; les distinguer par `name()`. |
| `val` à `null` | converti en `0` (pas d'erreur). |
| 8 séries en PV `day` | avec `IZY_MAX_SERIES=4`, les 4 séries de courant sont perdues. |

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

### `tools/validate_history.py` — valider le décodage des courbes

Portage **exact en Python** de la partie « historique » de `IzypowerCloud.h` :
conversion `time` → epoch (y compris les granularités partielles `"2026"`,
`"2026-11"`), normalisation de `searchTime` par granularité, sélection de la
forme de réponse (`data.data[]` station ou `data[]` appareil/PV), lecture
`val`/`value`, décimation à `IZY_MAX_POINTS`, limites de séries, casdégénérés, et **interpolation linéaire** (`value_at` : bornes exactes, milieu,
1/10, hors plage, série vide, point unique, passage par zéro, horodatage nul).
Aucune dépendance, aucun réseau, aucun identifiant :

```bash
python3 tools/validate_history.py    # → « TOUS LES TESTS PASSENT »
```

**À relancer à chaque modification de `_fetchHistory()` ou de `valueAt()`.**

## Récapitulatif des endpoints (rappel)

Le détail — en-têtes, enveloppe, codes, fuseau, forme des séries — est dans
[Communication avec le cloud Izypower](#communication-avec-le-cloud-izypower).
Les huit endpoints appelés par la librairie :

| # | Endpoint | Fonction qui l'utilise |
|---|---|---|
| 1 | `POST /login` | `login()` / `_ensureLogin()` |
| 2 | `GET /powerStations/page` | `fetchStations()` |
| 3 | `GET /v3/powerStations/info/{id}` | `fetchStationInfo()` |
| 4 | `GET /component/{id}` | `fetchComponent()` |
| 5 | `GET /device/page` | `fetchDevices()` |
| 6 | `GET /report/v2/powerStations/data/{id}` | `fetchStationHistory()` |
| 7 | `GET /report/izy/device/data/{sn}` | `fetchPvHistory()` |
| 8 | `GET /report/device/data/{sn}` | `fetchDeviceHistory()` |

Endpoints **non** implémentés (présents dans l'app mobile, utiles si besoin) :

| Endpoint | Données |
|---|---|
| `GET /v3/device/wifi/{sn}` | `rssi`, `wifi`, `ip` de l'appareil |
| `GET /report/device/rssi/data/{sn}` | historique du signal WiFi |
| `GET /v2/device/dataFlags/{deviceId}` | signaux disponibles pour un appareil (à utiliser pour choisir `dataFlag`) |
| `GET /v2/device/realInfo/{deviceId}` | instantané complet d'un appareil |

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
