# Changelog

## [0.5.0] - 2026-11-04

### ✨ Added — historique (courbes) dans `IzypowerCloud`

Les courbes affichées par l'application mobile ne viennent pas de
`v3/powerStations/info` mais des endpoints `report/*`. Trois nouvelles méthodes
les récupèrent, avec la même session (login/token) que `poll()` :

- `fetchStationHistory(range, searchTime = nullptr, showSoc = true)`
  → `GET /report/v2/powerStations/data/{id}?timeType=…&dataFlag=energy&searchTime=…`
- `fetchDeviceHistory(deviceSn, range, searchTime = nullptr, dataFlag = "energy")`
  → `GET /report/device/data/{sn}`
- `fetchPvHistory(deviceSn, range, searchTime = nullptr)`
  → `GET /report/izy/device/data/{sn}?queryType=pv`

Nouvelles structures et getters : `IzyHistoryRange`
(`IZY_RANGE_DAY|MONTH|YEAR|ALL`), `IzypowerCloudSeries`
(`key/name/unit/lineType/pointCount/pointTotal/decimated/point(i)`) et
`IzypowerCloudPoint` (`epoch()` en secondes Unix, `value()`), plus
`historySeriesCount()`, `historySeries(i)`, `findSeries(key)`,
`historyRange()`, `historySearchTime()`, `historyUpdatedMs()`.

### ✨ Added — valeur interpolée à un instant quelconque

- `IzypowerCloudSeries::valueAt(epochCloud, bool* couvert = nullptr)` :
  **interpolation linéaire** entre les deux points encadrants. Pas
  d'extrapolation hors plage (valeur du point extrême, `*couvert = false`).
- `IzypowerCloud::historyValueAt(clé, epochCloud, &couvert)` : `findSeries()`
  + `valueAt()` en un appel.
- `IzypowerCloud::toCloudEpoch("YYYY-MM-DD HH:MM[:SS]")` : conversion d'un
  horodatage en epoch du cloud, pour cibler un instant arbitraire.
- Validé sur données réelles (banc hôte local, non publié) : `production` interpolée à
  08:00 / 12:00 / 14:30 / 21:15 donne 13,00 / 858,90 / 375,90 / 2,60 W, et le
  milieu d'un segment redonne exactement la moyenne des deux points.

### ⚙️ Comportement

- Deux formes de réponse gérées : `data.data[]` (station) et `data[]` (appareil/PV).
- Points lus dans `val`, à défaut `value`.
- Décimation régulière au-delà de `IZY_MAX_POINTS` (96 par défaut, surchargeable
  par `-DIZY_MAX_POINTS=288`), premier et dernier point toujours conservés ;
  `pointTotal()` donne le nombre reçu, `decimated()` signale la réduction.
- `searchTime` par défaut = date de `data.lastUpdate` (issue de `poll()`).
- `_ensureLogin()` : re-login avant un appel d'historique, car ces endpoints ne
  renvoient pas toujours `401` (un `500` « veuillez vous connecter » est possible).
- Buffer statique (≈ 3 Ko avec les valeurs par défaut), compatible ESP8266.

### 🏷️ Divers

- `examples/history/history.ino` : démonstration (jour, PV d'un onduleur, mois).
- `tools/validate_history.py` : validation hors matériel du décodage des séries
  et de la décimation (portage Python exact de `_epochFromDateTime()`,
  `_normalizeDate()`, `_histAppend()`/`_histClose()` et `valueAt()`).
- README : section « Utilisation — Historique » + endpoints `report/*` ajoutés
  au tableau de l'API.
- `keywords.txt`, `library.json`, `library.properties` : version 0.5.0,
  historique et nouvel exemple.

### ✅ Validation

- **Contre l'API réelle** (compte de test, station `Fo170`) — via un banc hôte
  local (non publié), donc le code C++ de la librairie lui-même :
  - `poll()` → station 40539, 2 onduleurs (VMP4 4 chaînes, VMP2 2 chaînes) ;
  - `fetchStationHistory(day, "2026-11-04")` → 443 points → 90 conservés,
    `production`/`consumption`/`grid` en W, de 00:00 à 22:06 ;
  - `month` → `searchTime=2026-11` obligatoire (sinon `code 500`), 4 points ;
  - `year` → `searchTime=2026`, points horodatés `"2026-08"` ;
  - `all` → cumuls kWh, points horodatés `"2026"` ;
  - `fetchPvHistory(day)` → 8 séries `pvN_volt`/`pvN_curr`, 207 → 70 points,
    premier point 07:12 ;
  - `fetchDeviceHistory(meter, "v_ac,a_ac")` → 2 séries ;
  - courbe du **jour en cours** vide à 05 h 10 (matérialisée par le cloud plus
    tard) : `IZY_OK` mais séries à 0 point.
- Compilation : `esp32dev` et `nodemcuv2` (RAM 52,4 % / flash 29,8 % sur ESP8266
  avec l'exemple, 8 séries × 96 points).
- Décodage : `tools/validate_history.py` → « TOUS LES TESTS PASSENT ».

### ⚡ Lecture en flux (mémoire)

`_fetchHistory()` ne charge plus le corps de la réponse : il l'analyse **en
flux** (machine à états de ~100 o) et remplit directement le buffer de séries
avec décimation adaptative. Motif : une réponse `day` pèse 75 à 90 Ko, contre
≈50 Ko de tas libre sur l'ESP32 cible — le parseur par document échouait et
fragmentait la mémoire. Le dernier point reçu est conservé ; `pointTotal()`
donne le nombre réellement reçu.

### 🐛 Corrigé pendant la validation

- `searchTime` **obligatoire** : sans lui le cloud répond `code 500`. La
  librairie lit désormais `lastUpdate` automatiquement si `poll()` n'a pas
  encore tourné (`_ensureDate()`).
- Format de `searchTime` **dépendant de la granularité** : `YYYY-MM-DD` en
  `day`, `YYYY-MM` en `month`, `YYYY` en `year`. Une date trop précise
  renvoyait `code 500 « Erreur de format de date »` → troncature automatique
  (`_normalizeDate()`).
- `time` peut être partiel (`"2026"`, `"2026-11"`, `"2026-11-01"`) → ces
  formes sont désormais converties en epoch (premier jour de la période).
- `IZY_MAX_SERIES` porté de 4 à **8** : l'historique PV en `day` renvoie
  8 séries, les 4 dernières étaient perdues.
- **Deux fuseaux distincts identifiés** (vérifié contre l'heure NTP réelle) :
  les `time` des points et le `searchTime` attendu sont en **heure locale du
  site** (dernier point `22:54` pour `22:56 CET`), tandis que `lastUpdate` est
  en **heure serveur UTC+8**. Ajout de `IzypowerCloudPoint::epochUtc(offset)` et
  de la macro `IZY_LOCAL_UTC_OFFSET_HOURS` (1 h hiver / 2 h été).
- **Corrigé — mauvais jour par défaut** : la date de recherche était dérivée de
  `lastUpdate` (heure serveur), donc en soirée elle visait le lendemain et la
  courbe du jour revenait **vide**. `_refreshSearchDateFromClock()` prend
  désormais le jour **local** ; le repli sur `lastUpdate` ne sert que si
  l'horloge n'est pas synchronisée.

---

## [0.4.1] - 2026-10-17

### ✨ Added — macro de version `__<LIB>_H_VERSION__`

Chaque header déclare désormais sa version via une macro préprocesseur,
source **unique** (un projet hôte peut l'agréger, ex. page `/version`).

### 🏷️ Divers

- Version `0.4.0` → `0.4.1` (`library.json` + `library.properties`)

---

## [0.4.0] — 2026-08-07

- **Support du Smart Meter iP-SmartIA-W3** dans `src/IzypowerCloud.h` : nouvel endpoint `fetchDevices()` (`GET /device/page`, best-effort/non fatal dans `poll()`) avec les structures `IzypowerCloudDevice` (`serialNumber/type/model/pvNum/macAddress/firmware/deviceId/online/rssiDbm`) et `IzypowerCloudDto` (`key/value`, `dtoValue(key)`). Le Smart Meter (`model: "Meter"`, `pvNum: 0`) n'apparaît que dans `device/page`, pas dans `component/`.
- **Métrologie station** : `v3/powerStations/info` parse les champs top-level `grid_power`, `consumption`, `ev_power` → nouveaux getters `gridImportW()` (import + / injection −), `houseConsumptionW()`, `evChargeW()`.
- **Refactorisation « getters »** : accès **100 % par getters** (champs privés `_*` remplis via `friend class IzypowerCloud`), plus aucun champ public. Renommages : `powerW→productionW`, `todayKWh/monthKWh/yearKWh/totalKWh→productionKwhToday/Month/Year/Total`, `signal→signalDbm`, `sn→serialNumber()`, `ratedW→ratedPowerW()`, `pv[i].exists/powerW/energyKWh→pv(i)->connected()/powerW()/energyKwh()`.
- `examples/basic/basic.ino` mis à jour : nouvelle API getters + affichage Smart Meter (réseau/foyer/EV) et appareils `device/page`.
- README : API getters, métrologie Smart Meter, endpoint `device/page` détaillé, **Smart Meter iP-SmartIA-W3 présenté comme testé et fonctionnel** (tableau « accès », validation matériel, intro).
- `keywords.txt` : remplacé par la nouvelle API (getters `production*`, `gridImportW`, `houseConsumptionW`, `evChargeW`, `signalDbm`, structures `IzypowerCloudDto`/`IzypowerCloudDevice`, constantes `DM_*`).
- `library.json` / `library.properties` : version 0.4.0 + descriptions/keywords ajoutant l'iP-SmartIA-W3.

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
