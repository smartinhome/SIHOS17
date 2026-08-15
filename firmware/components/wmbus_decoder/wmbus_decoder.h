#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cc1101.h"

// Amiplus może zwrócić do 20+ pól (taryfy, napięcia, prądy i energia bierna).
// Musi odpowiadać pojemności ekstraktora historii, by API nie ucinało danych.
#define MAX_METER_VALUES 24

typedef struct {
    char    field[32];    // np. "total_energy_consumption_kwh"
    float   value;
    char    unit[8];      // "kWh", "m3", "W", "V"
} meter_field_t;

typedef struct {
    char         id_hex[12];
    char         type[16];
    char         name[32];
    int8_t       rssi;
    uint32_t     last_seen;   // unix timestamp (uptime ms)
    bool         valid;
    uint8_t      field_count;
    meter_field_t fields[MAX_METER_VALUES];
} meter_data_t;

#define MAX_ACTIVE_METERS 8

// ── Bufor surowych ramek (do "łapania" liczników w Web UI) ──────────
// Przechowujemy ostatnie N ramek w postaci hex, niezależnie od tego czy
// udało się je zdekodować. Web UI pobiera je przez /api/frames i dekoduje
// po stronie przeglądarki (parsowanie wMbus + AES/Diehl-LFSR + DIF/VIF).
#define MAX_RAW_FRAMES 40
#define MAX_RAW_HEX    520   // maks. długość hex (=> 260 bajtów ramki, Amiplus 193B)

typedef struct {
    char     hex[MAX_RAW_HEX + 1];
    int8_t   rssi;
    uint8_t  lqi;
    uint32_t ts_ms;
    uint32_t ts_unix;   // czas rzeczywisty (unix) odbioru ramki, 0 gdy brak SNTP
} raw_frame_t;

void          wmbus_decoder_init(void);
void          wmbus_decoder_on_frame(const wmbus_frame_t *frame);
int           wmbus_decoder_get_count(void);
// Ile ROZNYCH licznikow uslyszano w eterze (nie tylko 8 aktywnych slotow).
int           wmbus_decoder_get_seen_count(void);
meter_data_t *wmbus_decoder_get_meter(int index);
meter_data_t *wmbus_decoder_find_by_id(const char *id_hex);

// Surowe ramki — indeks 0 = najnowsza
int                wmbus_decoder_raw_count(void);
const raw_frame_t *wmbus_decoder_raw_get(int newest_index);
