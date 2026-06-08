#include "wmbus_decoder.h"
#include "nvs_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WMBUS";

// UWAGA: Ten moduł zawiera stub dekodera.
// Docelowo zastąpić wywołaniami biblioteki wmbusmeters
// (portowanej jako komponent IDF z repozytorium wmbusmeters/wmbusmeters).
// Interfejs wmbus_decode_frame() pozostaje taki sam.

static meter_data_t  s_meters[MAX_ACTIVE_METERS] = {0};
static int           s_meter_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

// Parsuj ID licznika z ramki wMbus (bajty 4-7, little-endian BCD)
static void parse_meter_id(const uint8_t *data, char *out, size_t out_len) {
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
    sih_config_t cfg = nvs_config_get();
    for (int i = 0; i < cfg.meter_count; i++) {
        if (strcasecmp(cfg.meters[i].id_hex, m->id_hex) == 0) {
            strlcpy(m->type, cfg.meters[i].type, sizeof(m->type));
            strlcpy(m->name, cfg.meters[i].name, sizeof(m->name));
            return;
        }
    }
    // Nieznany licznik — ustaw ID jako nazwę
    if (strlen(m->name) == 0) strlcpy(m->name, m->id_hex, sizeof(m->name));
}

// STUB — docelowo zastąpić wmbusmeters
// Zwraca true jeśli ramka została zdekodowana
static bool decode_frame_stub(const uint8_t *data, size_t len,
                               meter_data_t *out) {
    if (len < 12) return false;

    char id[12];
    parse_meter_id(data, id, sizeof(id));
    if (strlen(id) == 0) return false;

    strlcpy(out->id_hex, id, sizeof(out->id_hex));
    out->valid = true;

    // Odczytaj typ medium z bajtu 3
    uint8_t medium = data[3];
    switch (medium) {
        case 0x02: strlcpy(out->type, "electricity", sizeof(out->type)); break;
        case 0x07: strlcpy(out->type, "water",       sizeof(out->type)); break;
        case 0x03: strlcpy(out->type, "gas",         sizeof(out->type)); break;
        case 0x06: strlcpy(out->type, "heat",        sizeof(out->type)); break;
        default:   strlcpy(out->type, "unknown",     sizeof(out->type)); break;
    }

    // STUB: dane testowe — zastąpić prawdziwym dekoderem
    // W docelowej implementacji tutaj wywołujemy wmbusmeters:
    // wmb_meter_t *meter = wmb_decode(data, len, key);
    // wmb_get_field(meter, "total_energy_consumption_kwh", &val);
    add_field(out, "rssi_dbm", (float)out->rssi, "dBm");

    return true;
}

void wmbus_decoder_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Dekoder wMbus gotowy (stub — wymaga wmbusmeters)");
}

void wmbus_decoder_on_frame(const wmbus_frame_t *frame) {
    if (!frame || frame->len < 12) return;

    char hex_log[64] = {0};
    for (int i = 0; i < (int)frame->len && i < 16; i++)
        snprintf(hex_log + i*3, sizeof(hex_log) - i*3, "%02X ", frame->data[i]);
    ESP_LOGI(TAG, "Ramka [%d B] RSSI:%ddBm: %s...", 
             (int)frame->len, frame->rssi, hex_log);

    meter_data_t tmp = {0};
    tmp.rssi = frame->rssi;
    tmp.last_seen = (uint32_t)(esp_timer_get_time() / 1000);

    if (!decode_frame_stub(frame->data, frame->len, &tmp)) {
        ESP_LOGW(TAG, "Nie można zdekodować ramki");
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_data_t *m = find_or_create(tmp.id_hex);
    if (m) {
        m->rssi      = tmp.rssi;
        m->last_seen = tmp.last_seen;
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
