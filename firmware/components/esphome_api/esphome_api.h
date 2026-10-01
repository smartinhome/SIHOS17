#pragma once
#include <stdbool.h>
#include <stdint.h>

// Serwer natywnego API ESPHome - Home Assistant laczy sie z modulem sam,
// bez brokera MQTT. Tylko odczyt: czujniki z wartosciami licznikow.
//
// Protokol ZAMROZONY na ESPHome 2025.6.3 = API 1.10, zgodnie z api.proto
// z tego wydania. Bez szyfrowania (Noise) i bez mDNS - urzadzenie dodaje sie
// w HA recznie po adresie IP, port 6053.
//
// Publikowane sa te same pola co przez MQTT: przypiete do dashboardu lub
// sledzone w historii.

#define ESPHOME_API_PORT 6053

// Start serwera po uzyskaniu adresu IP. Nic nie robi, gdy wylaczony.
void esphome_api_start(void);
// Zatrzymanie (tryb AP, OTA, wylaczenie w panelu). Czeka na koniec zadania.
void esphome_api_stop(void);

bool esphome_api_enabled(void);
// Zapis ustawienia w NVS. Start/stop robi wolajacy.
void esphome_api_set_enabled(bool on);

// Odczyt pola z ramki. Wolane z zadania dekodera - nie blokuje na sieci.
void esphome_api_field(const char *id_hex, const char *field,
                       double value, const char *unit);

// Stan do zakladki System (JSON).
int  esphome_api_status_json(char *buf, int cap);
