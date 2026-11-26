#pragma once
#include <stdint.h>
#include <stdbool.h>

// beta446: pogoda z Open-Meteo (bez klucza i konta) - TYLKO na stronie e-inka.
// Nie trafia do MQTT ani do API ESPHome. Domyslnie wylaczona.
// Pobieranie w krotko zyjacym zadaniu (TLS ok. 40 KB tylko na czas pobrania).

#define WEATHER_DAYS 4   // dzis + 3 kolejne dni

typedef struct {
    uint8_t code;        // kod pogody WMO
    uint8_t wday;        // 0 = niedziela ... 6 = sobota
    float   tmax, tmin;
} weather_day_t;

typedef struct {
    bool     valid;
    uint32_t ts;         // czas pomiaru z odpowiedzi (unix, czas lokalny Polski)
    float    temp, apparent, wind, pressure, precip;
    uint8_t  humidity;
    uint8_t  code;
    int      ndays;
    weather_day_t day[WEATHER_DAYS];
} weather_data_t;

void weather_init(void);
bool weather_enabled(void);
// Kopia ostatnich danych; false gdy jeszcze nic nie pobrano.
bool weather_get(weather_data_t *out);
const char *weather_city(void);
// Konfiguracja z panelu. city/lat/lon pomijane, gdy city == NULL.
void weather_configure(bool enabled, const char *city, float lat, float lon, int interval_min);
void weather_refresh_now(void);
int  weather_status_json(char *buf, int cap);
