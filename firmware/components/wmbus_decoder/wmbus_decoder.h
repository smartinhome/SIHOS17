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
    uint32_t     last_seen;        // monotoniczny czas uruchomienia (ms)
    uint32_t     last_seen_unix;   // czas rzeczywisty, 0 przed synchronizacja SNTP
    bool         valid;
    uint8_t      field_count;
    meter_field_t fields[MAX_METER_VALUES];
} meter_data_t;

// 24 sloty (bylo 8): od kiedy pola sa dekodowane w firmware, panel bierze
// stad wartosci dla dashboardu. Przy 8 slotach czesc licznikow dostawalaby
// dane tylko z przegladarki, wiec inne niz te zapisane w historii.
// Koszt: 940 B na slot, ok. 22 KB w .bss zamiast 7 KB.
#define MAX_ACTIVE_METERS 24

// ── Bufor surowych ramek (do "łapania" liczników w Web UI) ──────────
// Przechowujemy ostatnie N ramek w postaci hex, niezależnie od tego czy
// udało się je zdekodować. Web UI pobiera je przez /api/frames i dekoduje
// po stronie przeglądarki (parsowanie wMbus + AES/Diehl-LFSR + DIF/VIF).
#define MAX_RAW_FRAMES 40
#define MAX_RAW_BYTES  260   // najdluzsza spotykana ramka (Amiplus 193 B) z zapasem
#define MAX_RAW_HEX    (MAX_RAW_BYTES * 2)

// Ramki trzymamy BINARNIE, nie jako tekst hex. Tekst zajmowal dwa razy wiecej
// (521 B na ramke zamiast 260 B), a i tak jest potrzebny dopiero w chwili
// wysylania do panelu - konwersja odbywa sie tam, gdzie budowana jest odpowiedz.
// Oszczednosc na stercie: ok. 10 KB.
typedef struct {
    uint8_t  data[MAX_RAW_BYTES];
    uint16_t len;
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

// Surowe ramki — indeks 0 = najnowsza
int                wmbus_decoder_raw_count(void);
const raw_frame_t *wmbus_decoder_raw_get(int newest_index);

// FAZA 7: wyczysc liste widzianych/aktywnych licznikow (s_seen[] i s_meters[]),
// ZACHOWUJAC tylko te ktore sa dodane do konfiguracji (cfg->meters[]) lub
// sledzone w historii (history_tracked_meter_ids). Zwraca liczbe skasowanych ID.
// Uzyteczne przy zmianie lokalizacji modulu (np. inkasent) - kasuje sasiadow
// z eteru, ale zachowuje wlasne liczniki. Nowa ramka z licznikiem po skasowaniu
// automatycznie odbuduje jego wpis.
int                wmbus_decoder_clear_untracked(void);
