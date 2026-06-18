#pragma once
#include <stdint.h>
#include <stdbool.h>

// Dioda RGB WS2812 (status WiFi) na GPIO8.
// Zielona = polaczono z WiFi, czerwona = brak/utrata polaczenia z zapisana siecia.
// Jasnosc regulowana, mozna wylaczyc calkowicie.

// Inicjalizacja. enabled/brightness wczytane z konfiguracji.
void led_status_init(bool enabled, uint8_t brightness);

// Zmiana ustawien w locie. brightness 0-100 (%).
void led_status_set(bool enabled, uint8_t brightness);
