/*
 * Izypower - Example History
 * https://github.com/Fo170/izypower
 *
 * Récupère l'HISTORIQUE (courbes) d'une centrale Izypower : les courbes de
 * l'application mobile ne viennent pas de `v3/powerStations/info` mais des
 * endpoints `report/*`, exploités par fetchStationHistory() /
 * fetchDeviceHistory() / fetchPvHistory().
 *
 * Trois granularités : journée (points), mois (jours), année (mois) —
 * plus IZY_RANGE_ALL pour tout l'historique.
 *
 * ⚠️ Les `time` des points et le `searchTime` attendu sont en heure LOCALE
 *    DU SITE (pas en UTC). Utiliser IzypowerCloud::toCloudEpoch() pour cibler
 *    un instant, et point->epochUtc() pour l'horodatage Unix.
 */

#include <Arduino.h>
#ifdef ESP32
  #include <WiFi.h>
#else
  #include <ESP8266WiFi.h>
#endif
#include <IzypowerCloud.h>

const char* SSID     = "VOTRE_SSID";
const char* PASSWORD = "VOTRE_MOT_DE_PASSE";

// Compte Izypower Cloud (application.izypowercloud.fr)
const char* IZY_EMAIL    = "votre@email.fr";
const char* IZY_PASSWORD = "votre_mot_de_passe";

// Identifiant de la centrale (stationsId) ; 0 = auto-découverte
const uint32_t IZY_STATION_ID = 0;

IzypowerCloud cloud;

// Affiche toutes les séries d'historique actuellement en mémoire.
static void afficherSeries()
{
    Serial.printf("%u série(s) — %s\n", cloud.historySeriesCount(),
                  cloud.historySearchTime());
    for (uint8_t i = 0; i < cloud.historySeriesCount(); i++) {
        const IzypowerCloudSeries* s = cloud.historySeries(i);
        if (!s) continue;
        Serial.printf("  %-12s %-20s %-5s %u point(s)",
                      s->key(), s->name(), s->unit(), s->pointCount());
        if (s->decimated())
            Serial.printf(" (décimé depuis %u)", s->pointTotal());
        Serial.println();

        // Les 3 premiers points, en heure locale puis en Unix (UTC)
        for (uint16_t p = 0; p < s->pointCount() && p < 3; p++) {
            const IzypowerCloudPoint* pt = s->point(p);
            if (!pt) continue;
            Serial.printf("      t=%lu (utc %lu) -> %.2f %s\n",
                          (unsigned long)pt->epoch(),
                          (unsigned long)pt->epochUtc(),
                          pt->value(), s->unit());
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);

    WiFi.begin(SSID, PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(400);
        Serial.print(".");
    }
    Serial.println("\nWiFi connecté");

    cloud.begin(IZY_EMAIL, IZY_PASSWORD, IZY_STATION_ID);

    // L'historique réutilise la session (login/token) ouverte par poll().
    if (cloud.poll() != IZY_OK) {
        Serial.printf("Échec login : %s\n", cloud.lastErrorText());
        return;
    }

    // ── 1) Courbes de la journée de la centrale ──────────────────────
    if (cloud.fetchStationHistory(IZY_RANGE_DAY) == IZY_OK) {
        Serial.println("\n== Centrale, jour ==");
        afficherSeries();
    } else {
        Serial.printf("Échec historique station : %s\n", cloud.lastErrorText());
    }

    // ── 2) Valeur interpolée à un instant précis ─────────────────────
    // toCloudEpoch() attend l'heure LOCALE DU SITE ("YYYY-MM-DD HH:MM").
    const uint32_t t = IzypowerCloud::toCloudEpoch("2026-11-04 14:30");
    bool couvert = false;
    const float prod = cloud.historyValueAt("production", t, &couvert);
    Serial.printf("\nProduction à 14:30 : %.2f W (%s)\n", prod,
                  couvert ? "interpolée" : "hors plage : point extrême");

    // ── 3) Courbes d'un onduleur (PV1..PV4 × tension/courant) ────────
    if (cloud.inverterCount() > 0) {
        const char* sn = cloud.inverter(0)->serialNumber();
        if (cloud.fetchPvHistory(sn, IZY_RANGE_DAY) == IZY_OK) {
            Serial.printf("\n== Onduleur %s, jour ==\n", sn);
            afficherSeries();
        } else {
            Serial.printf("Échec historique PV : %s\n", cloud.lastErrorText());
        }
    }

    // ── 4) Historique mensuel de la centrale ────────────────────────
    if (cloud.fetchStationHistory(IZY_RANGE_MONTH) == IZY_OK) {
        Serial.println("\n== Centrale, mois ==");
        afficherSeries();
    }

    // Libère le buffer de séries (réalloué au prochain fetch).
    cloud.releaseHistory();
}

void loop() {
    // lecture ponctuelle ; ici le programme s'arrête après setup()
}
