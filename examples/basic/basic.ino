/*
 * Izypower - Example Basic
 * https://github.com/Fo170/izypower
 *
 * Récupère via l'API cloud Izypower les données des micro-onduleurs
 * iP-1000-W2 / iP-2000-W4 (station + chaînes PV + métrologie Smart
 * Meter iP-SmartIA-W3) et les affiche sur la console série.
 *
 * NB : ces iP n'exposent aucun accès local (ni Solarman V5/8899 ni
 * INDEVOLT RPC/8080, vérifié sur matériel) : la lecture passe
 * obligatoirement par le cloud (application.izypowercloud.fr),
 * rafraîchi toutes les ~3 min.
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
    if (cloud.poll() != IZY_OK) {
        Serial.printf("Échec : %s\n", cloud.lastErrorText());
        return;
    }

    Serial.printf("Station %lu (%s) : %d W, aujourd'hui %.2f kWh\n",
                  (unsigned long)cloud.stationId(), cloud.stationName(),
                  (int)cloud.productionW(), cloud.productionKwhToday());
    // Métrologie du Smart Meter (top-level de v3/powerStations/info).
    Serial.printf("  Réseau : import %d W (négatif = injection) | foyer %d W | EV %d W\n",
                  (int)cloud.gridImportW(), (int)cloud.houseConsumptionW(),
                  (int)cloud.evChargeW());

    for (uint8_t i = 0; i < cloud.inverterCount(); i++) {
        const IzypowerCloudInverter* inv = cloud.inverter(i);
        Serial.printf("  %s (deviceId %lu, %u W) : %d W, jour %.2f kWh, %u chaîne(s)\n",
                      inv->serialNumber(), (unsigned long)inv->deviceId(),
                      inv->ratedPowerW(), (int)inv->productionW(),
                      inv->productionKwhToday(), inv->pvCount());
        for (uint8_t p = 0; p < inv->pvCount(); p++)
            Serial.printf("    PV%u : %.1f W / %.2f kWh\n", p + 1,
                          inv->pvPowerW(p), inv->pvEnergyKwh(p));
    }

    // Appareils non-PV (device/page) : Smart Meter / Passerelle IA, batterie, EV…
    for (uint8_t i = 0; i < cloud.deviceCount(); i++) {
        const IzypowerCloudDevice* dev = cloud.device(i);
        Serial.printf("  %s (%s, modèle %s, %u chaîne(s) PV) : %s, RSSI %d dBm\n",
                      dev->serialNumber(), dev->type(), dev->model(), dev->pvNum(),
                      dev->online() ? "en ligne" : "hors ligne", (int)dev->rssiDbm());
        for (uint8_t d = 0; d < dev->dtoCount(); d++) {
            const IzypowerCloudDto* dto = dev->dto(d);
            Serial.printf("      %s = %s\n", dto->key(), dto->value());
        }
    }
}

void loop() {
    // lecture ponctuelle ; ici le programme s'arrête après setup()
}
