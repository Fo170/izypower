/*
 * Izypower - Example Basic
 * https://github.com/Fo170/izypower
 *
 * Récupère via l'API cloud Izypower la production des micro-onduleurs
 * iP-1000-W2 / iP-2000-W4 (station + chaînes PV) et l'affiche sur la
 * console série.
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
                  (int)cloud.powerW(), cloud.todayKWh());

    for (uint8_t i = 0; i < cloud.inverterCount(); i++) {
        const IzypowerCloudInverter* inv = cloud.inverter(i);
        Serial.printf("  %s (deviceId %lu, %u W) : %d W, jour %.2f kWh, %u chaîne(s)\n",
                      inv->sn, (unsigned long)inv->deviceId, inv->ratedW,
                      (int)inv->powerW(), inv->todayKWh(), inv->pvCount);
        for (uint8_t p = 0; p < inv->pvCount; p++)
            Serial.printf("    PV%u : %.1f W / %.2f kWh\n", p + 1,
                          inv->pv[p].powerW, inv->pv[p].energyKWh);
    }
}

void loop() {
    // lecture ponctuelle ; ici le programme s'arrête après setup()
}
