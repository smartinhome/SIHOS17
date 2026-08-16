#include "wmbus_decoder.h"
#include "nvs_config.h"
#include "meter_total.h"
#include "history.h"
#include "led_rx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WMBUS";

// Odczyty dla API i historii są wyciągane przez meter_total.  Pełne wsparcie
// kolejnych sterowników można dodawać właśnie do tego komponentu.

static meter_data_t  s_meters[MAX_ACTIVE_METERS] = {0};
static int           s_meter_count = 0;

// Rejestr WSZYSTKICH uslyszanych ID. s_meters[] ma tylko 8 slotow (pelne dane
// pol), a w eterze slychac tez liczniki sasiadow - System pokazywal wiec
// zanizona liczbe. Tu trzymamy same identyfikatory: 64 x 9 B = 576 B.
#define MAX_SEEN_IDS 64
// Licznik "znika" z zestawienia, gdy nie odezwie sie przez ten czas. Bez tego
// wartosc tylko rosla i po testach w innej lokalizacji wisiala na maksimum.
// 6 h jest bezpieczne nawet dla licznikow nadajacych raz na godzine.
#define SEEN_TTL_MS (6UL * 3600UL * 1000UL)

typedef struct {
    char     id[9];
    uint32_t last_ms;   // uptime ostatniej ramki
} seen_t;
static seen_t        s_seen[MAX_SEEN_IDS];
static int           s_seen_used = 0;   // ile pozycji tablicy jest w uzyciu

static void seen_add(const char *id_hex) {
    if (!id_hex || !id_hex[0]) return;
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < s_seen_used; i++)
        if (strcasecmp(s_seen[i].id, id_hex) == 0) { s_seen[i].last_ms = now_ms; return; }
    if (s_seen_used < MAX_SEEN_IDS) {
        snprintf(s_seen[s_seen_used].id, sizeof(s_seen[0].id), "%.8s", id_hex);
        s_seen[s_seen_used].last_ms = now_ms;
        s_seen_used++;
        return;
    }
    // Tablica pelna - nadpisz najdawniej slyszany wpis.
    int oldest = 0;
    for (int i = 1; i < MAX_SEEN_IDS; i++)
        if ((uint32_t)(now_ms - s_seen[i].last_ms) > (uint32_t)(now_ms - s_seen[oldest].last_ms))
            oldest = i;
    snprintf(s_seen[oldest].id, sizeof(s_seen[0].id), "%.8s", id_hex);
    s_seen[oldest].last_ms = now_ms;
}
static SemaphoreHandle_t s_mutex = NULL;

// Bufor pierścieniowy surowych ramek
// Bufor podgladu ramek (~21KB) alokowany PRZY PIERWSZEJ RAMCE, nie w .bss -
// nie obciaza startu, gdy najwiecej RAM potrzebuja WiFi/OTA.
static raw_frame_t  *s_raw = NULL;
static int           s_raw_head  = 0;   // następny indeks do zapisu
static int           s_raw_count = 0;   // ile ramek w buforze (max MAX_RAW_FRAMES)

static void store_raw_frame(const wmbus_frame_t *frame) {
    if (!s_raw) {
        s_raw = calloc(MAX_RAW_FRAMES, sizeof(raw_frame_t));
        if (!s_raw) return;   // brak RAM - pomijamy podglad ramek (odbior dziala dalej)
    }
    raw_frame_t *r = &s_raw[s_raw_head];
    size_t n = frame->len;
    if (n > MAX_RAW_HEX / 2) n = MAX_RAW_HEX / 2;   // przytnij do pojemności hex
    // Konwersja hex przez tablice zamiast snprintf() na KAZDY bajt: snprintf
    // parsuje format i wola vsnprintf per bajt (~46x wolniej na tej samej
    // maszynie). To jest goraca sciezka - wolane dla kazdej odebranej ramki.
    static const char HEXD[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        r->hex[i * 2]     = HEXD[frame->data[i] >> 4];
        r->hex[i * 2 + 1] = HEXD[frame->data[i] & 0x0F];
    }
    r->hex[n * 2] = 0;
    r->rssi  = frame->rssi;
    r->lqi   = frame->lqi;
    r->ts_ms = (uint32_t)(esp_timer_get_time() / 1000);
    time_t now = time(NULL);
    r->ts_unix = (now > 1700000000) ? (uint32_t)now : 0;  // tylko gdy czas zsynchronizowany (SNTP)

    s_raw_head = (s_raw_head + 1) % MAX_RAW_FRAMES;
    if (s_raw_count < MAX_RAW_FRAMES) s_raw_count++;
}

// Parsuj ID licznika z ramki wMbus (bajty 4-7, little-endian BCD)
static void parse_meter_id(const uint8_t *data, char *out, size_t out_len) {
    if (out_len > 0) out[0] = 0;   // wynik zawsze zdefiniowany (strlen u wolajacego)
    if (data[0] < 0x0C) return; // za krótka ramka
    snprintf(out, out_len, "%02x%02x%02x%02x",
             data[7], data[6], data[5], data[4]);
}

static meter_data_t *find_or_create(const char *id_hex) {
    for (int i = 0; i < s_meter_count; i++) {
        if (strcmp(s_meters[i].id_hex, id_hex) == 0)
            return &s_meters[i];
    }
    if (s_meter_count >= MAX_ACTIVE_METERS) return NULL;
    meter_data_t *m = &s_meters[s_meter_count++];
    memset(m, 0, sizeof(meter_data_t));
    strlcpy(m->id_hex, id_hex, sizeof(m->id_hex));
    return m;
}

static void add_field(meter_data_t *m, const char *field,
                      float value, const char *unit) {
    // Znajdź istniejące pole lub dodaj nowe
    for (int i = 0; i < m->field_count; i++) {
        if (strcmp(m->fields[i].field, field) == 0) {
            m->fields[i].value = value;
            return;
        }
    }
    if (m->field_count >= MAX_METER_VALUES) return;
    meter_field_t *f = &m->fields[m->field_count++];
    strlcpy(f->field, field, sizeof(f->field));
    strlcpy(f->unit,  unit,  sizeof(f->unit));
    f->value = value;
}

// Mapuj typ z konfiguracji na nazwę
static void apply_config(meter_data_t *m) {
    const sih_config_t *cfg = nvs_config_ptr();
    for (int i = 0; i < cfg->meter_count; i++) {
        if (strcasecmp(cfg->meters[i].id_hex, m->id_hex) == 0) {
            strlcpy(m->type, cfg->meters[i].type, sizeof(m->type));
            strlcpy(m->name, cfg->meters[i].name, sizeof(m->name));
            return;
        }
    }
    // Nieznany licznik — ustaw ID jako nazwę
    if (strlen(m->name) == 0) strlcpy(m->name, m->id_hex, sizeof(m->name));
}

// Zwraca zapisany klucz AES dla licznika albo pusty napis.  Dekoder i
// historia muszą korzystać z identycznego klucza; inaczej API mogłoby
// pokazywać inne dane niż te zapisane na flashu.
static const char *meter_key_for_id(const char *id_hex) {
    const sih_config_t *cfg = nvs_config_ptr();
    for (int i = 0; i < cfg->meter_count; i++) {
        if (strcasecmp(cfg->meters[i].id_hex, id_hex) == 0)
            return cfg->meters[i].key;
    }
    return "";
}

// Dekoduje pola obsługiwane przez meter_total.  Ten sam ekstraktor zasila
// trwałą historię, dzięki czemu /api/meters nie jest już atrapą z samym RSSI.
// Zwraca true, gdy udało się rozpoznać poprawne ID ramki; brak pól oznacza np.
// nieobsługiwany typ licznika albo brak klucza AES.
static bool decode_frame(const uint8_t *data, size_t len, meter_data_t *out) {
    if (len < 12) return false;

    char id[12];
    parse_meter_id(data, id, sizeof(id));
    if (strlen(id) == 0) return false;

    strlcpy(out->id_hex, id, sizeof(out->id_hex));
    out->valid = true;

    // Medium jest w adresie DLL, w bajcie 9 (bajt 3 należy do producenta).
    uint8_t medium = data[9];
    switch (medium) {
        case 0x02: strlcpy(out->type, "electricity", sizeof(out->type)); break;
        case 0x03:
        case 0x0C: strlcpy(out->type, "gas",         sizeof(out->type)); break;
        case 0x04:
        case 0x0A:
        case 0x0B:
        case 0x0D: strlcpy(out->type, "heat",        sizeof(out->type)); break;
        // 0x06 woda ciepla, 0x07 woda, 0x16 woda zimna (standard),
        // 0x62/0x72 warianty Techem (ciepla/zimna) - bez nich licznik Techem
        // mial typ "unknown" mimo poprawnie zdekodowanego stanu.
        case 0x06:
        case 0x07:
        case 0x16:
        case 0x62:
        case 0x72: strlcpy(out->type, "water",       sizeof(out->type)); break;
        default:   strlcpy(out->type, "unknown",     sizeof(out->type)); break;
    }

    mtf_field_t fields[MTF_MAX_FIELDS];
    int kind = 0;
    int nf = meter_total_extract_fields(data, len, meter_key_for_id(id),
                                        fields, MTF_MAX_FIELDS, &kind);
    for (int i = 0; i < nf; i++)
        add_field(out, fields[i].field, (float)fields[i].value, fields[i].unit);

    return true;
}

void wmbus_decoder_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Dekoder wMbus gotowy");
}


// Sprawdza czy ramka jest zaszyfrowana (na podstawie CI-field i tpl-cfg)
// oraz czy dla danego ID jest zapisany klucz w konfiguracji.
// UWAGA: ramka z eteru zawiera CRC blokow (CI jest na [12], nie [10]),
// a tryb szyfrowania to bity 8-12 slowa CFG - cala logika (usuwanie CRC,
// naglowek krotki/dlugi, wykrywanie payloadu juz jawnego 2F2F) jest
// w meter_total_needs_key(), przetestowanym na wektorach wmbusmeters.
static void check_encryption_key(const uint8_t *data, size_t len, const char *id) {
    if (!meter_total_needs_key(data, len)) return;

    // Sprawdz czy jest klucz w konfiguracji dla tego ID
    const sih_config_t *cfg = nvs_config_ptr();
    bool has_key = false;
    for (int i = 0; i < cfg->meter_count; i++) {
        if (strcasecmp(cfg->meters[i].id_hex, id) == 0) {
            if (strlen(cfg->meters[i].key) >= 32) has_key = true;
            break;
        }
    }
    if (!has_key) {
        ESP_LOGW(TAG, "Licznik %s jest ZASZYFROWANY (AES) - podaj klucz w zakladce Liczniki", id);
    }
}

void wmbus_decoder_on_frame(const wmbus_frame_t *frame) {
    if (!frame || frame->len < 12) return;

    // Blysk diody RX: domyslnie przy KAZDYM telegramie; w trybie
    // "tylko dashboard" wylacznie gdy ramka pochodzi od licznika
    // przypietego do dashboardu.
    {
        char blink_id[12];
        parse_meter_id(frame->data, blink_id, sizeof(blink_id));
        if (!nvs_config_led_only_pinned() || nvs_config_is_pinned(blink_id))
            led_rx_blink();
    }

    int maxb = (int)frame->len;
    if (maxb > 290) maxb = 290;  // zabezpieczenie bufora

    // Wersja ze spacjami - czytelna w logach
    char hex_log[900] = {0};
    for (int i = 0; i < maxb; i++)
        snprintf(hex_log + i*3, sizeof(hex_log) - i*3, "%02X ", frame->data[i]);
    ESP_LOGI(TAG, "Ramka [%d B] RSSI:%ddBm: %s",
             (int)frame->len, frame->rssi, hex_log);

    // Wersja bez spacji + gotowy link do analizatora wmbusmeters.org
    // (wmbusmeters sam usuwa CRC blokow, wiec podajemy surowa ramke z CRC)
    char hex_raw[600] = {0};
    for (int i = 0; i < maxb; i++)
        snprintf(hex_raw + i*2, sizeof(hex_raw) - i*2, "%02x", frame->data[i]);
    ESP_LOGI(TAG, "  -> https://wmbusmeters.org/analyze/%s", hex_raw);

    // Zachowaj surową ramkę do bufora (nawet jeśli dekoder jej nie rozpozna)
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    store_raw_frame(frame);
    xSemaphoreGive(s_mutex);

    meter_data_t tmp = {0};
    tmp.rssi = frame->rssi;
    tmp.last_seen = (uint32_t)(esp_timer_get_time() / 1000);
    time_t frame_time = time(NULL);
    tmp.last_seen_unix = (frame_time > 1700000000) ? (uint32_t)frame_time : 0;

    if (!decode_frame(frame->data, frame->len, &tmp)) {
        ESP_LOGW(TAG, "Nie można zdekodować ramki");
        return;
    }

    seen_add(tmp.id_hex);   // licz KAZDY uslyszany licznik, nie tylko 8 aktywnych

    // Sygnalizuj w logach jesli licznik zaszyfrowany a brak klucza
    check_encryption_key(frame->data, frame->len, tmp.id_hex);

    // --- HISTORIA 24/7: wyciagnij wszystkie pola i zapisz sledzone ---
    {
        const char *key_hex = meter_key_for_id(tmp.id_hex);
        time_t now = time(NULL);
        uint32_t ts_unix = (now > 1700000000) ? (uint32_t)now : 0;
        if (ts_unix) {
            mtf_field_t fields[MTF_MAX_FIELDS];
            int kind = 0;
            int nf = meter_total_extract_fields(frame->data, frame->len, key_hex,
                                                fields, MTF_MAX_FIELDS, &kind);
            for (int i = 0; i < nf; i++) {
                // zapis tylko sledzonych pol (filtr w history_on_field)
                history_on_field(tmp.id_hex, fields[i].field, fields[i].value,
                                 kind, fields[i].cumulative, ts_unix);
            }
        }
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_data_t *m = find_or_create(tmp.id_hex);
    if (m) {
        m->rssi      = tmp.rssi;
        m->last_seen = tmp.last_seen;
        m->last_seen_unix = tmp.last_seen_unix;
        m->valid     = true;
        // Kopiuj pola z tmp
        for (int i = 0; i < tmp.field_count; i++)
            add_field(m, tmp.fields[i].field,
                      tmp.fields[i].value, tmp.fields[i].unit);
        apply_config(m);
    }
    xSemaphoreGive(s_mutex);
}

int wmbus_decoder_get_count(void) { return s_meter_count; }

int wmbus_decoder_get_seen_count(void) {
    // Licz tylko te, ktore odezwaly sie w ostatnich SEEN_TTL_MS.
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    int n = 0;
    for (int i = 0; i < s_seen_used; i++)
        if ((uint32_t)(now_ms - s_seen[i].last_ms) <= SEEN_TTL_MS) n++;
    return n;
}

meter_data_t *wmbus_decoder_get_meter(int index) {
    if (index < 0 || index >= s_meter_count) return NULL;
    return &s_meters[index];
}

meter_data_t *wmbus_decoder_find_by_id(const char *id_hex) {
    for (int i = 0; i < s_meter_count; i++)
        if (strcmp(s_meters[i].id_hex, id_hex) == 0)
            return &s_meters[i];
    return NULL;
}

int wmbus_decoder_raw_count(void) { return s_raw_count; }

// newest_index 0 = najnowsza ramka, 1 = poprzednia, ...
const raw_frame_t *wmbus_decoder_raw_get(int newest_index) {
    if (!s_raw || newest_index < 0 || newest_index >= s_raw_count) return NULL;
    // s_raw_head wskazuje na następne wolne miejsce, więc najnowsza jest head-1
    int idx = (s_raw_head - 1 - newest_index + 2 * MAX_RAW_FRAMES) % MAX_RAW_FRAMES;
    return &s_raw[idx];
}
