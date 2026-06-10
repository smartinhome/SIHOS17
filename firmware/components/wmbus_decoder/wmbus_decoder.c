#include <stdlib.h>
#include "wmbus_decoder.h"
#include "nvs_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "WMBUS";

// ETAP 1: poprawny odbior + logi (ID licznika + hex ramki).
// Dekodowanie wartosci pol (wmbusmeters) bedzie etapem 2.

static meter_data_t  s_meters[MAX_ACTIVE_METERS] = {0};
static int           s_meter_count = 0;
static SemaphoreHandle_t s_mutex = NULL;

// Parsuj ID licznika z ramki wMbus.
// Struktura ramki (po dekodowaniu): L C M M A A A A ...
//   [0] L-field (dlugosc)
//   [1] C-field
//   [2..3] M-field (manufacturer, little-endian)
//   [4..7] A-field (adres/ID, little-endian BCD)
//   [8] wersja, [9] typ medium
static void parse_meter_id(const uint8_t *d, size_t len, char *out, size_t out_len) {
    if (len < 8) { out[0] = 0; return; }
    snprintf(out, out_len, "%02x%02x%02x%02x", d[7], d[6], d[5], d[4]);
}

static void parse_manufacturer(const uint8_t *d, size_t len, char *out, size_t out_len) {
    if (len < 4) { out[0] = 0; return; }
    // M-field: 2 bajty, kazda litera 5 bitow + 0x40
    uint16_t m = d[2] | (d[3] << 8);
    char c1 = ((m >> 10) & 0x1F) + 64;
    char c2 = ((m >>  5) & 0x1F) + 64;
    char c3 = ((m      ) & 0x1F) + 64;
    snprintf(out, out_len, "%c%c%c", c1, c2, c3);
}

static const char *medium_name(uint8_t medium) {
    switch (medium) {
        case 0x02: return "electricity";
        case 0x03: return "gas";
        case 0x06: return "warm_water";
        case 0x07: return "water";
        case 0x16: return "cold_water";
        case 0x0D: return "heat_cost";
        default:   return "unknown";
    }
}

static meter_data_t *find_or_create(const char *id_hex) {
    for (int i = 0; i < s_meter_count; i++)
        if (strcmp(s_meters[i].id_hex, id_hex) == 0)
            return &s_meters[i];
    if (s_meter_count >= MAX_ACTIVE_METERS) return NULL;
    meter_data_t *m = &s_meters[s_meter_count++];
    memset(m, 0, sizeof(meter_data_t));
    strlcpy(m->id_hex, id_hex, sizeof(m->id_hex));
    return m;
}

static void add_field(meter_data_t *m, const char *field,
                      float value, const char *unit) {
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

void wmbus_decoder_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "Dekoder wMbus gotowy (etap 1: odbior + logi)");
}

void wmbus_decoder_on_frame(const wmbus_frame_t *frame) {
    if (!frame || frame->len < 10) return;

    // Hex calej ramki (jak ESPHome)
    char hex[3 * 64 + 8] = {0};
    size_t show = frame->len > 64 ? 64 : frame->len;
    for (size_t i = 0; i < show; i++)
        snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", frame->data[i]);

    char id[12]  = {0};
    char man[8]  = {0};
    parse_meter_id(frame->data, frame->len, id, sizeof(id));
    parse_manufacturer(frame->data, frame->len, man, sizeof(man));

    uint8_t medium  = frame->len > 9 ? frame->data[9] : 0;
    uint8_t version = frame->len > 8 ? frame->data[8] : 0;

    // LOG jak w ESPHome: RSSI, hex, dlugosc, ID, producent
    ESP_LOGI(TAG, "RSSI:%ddBm L:%d ID:%s MAN:%s VER:0x%02X MEDIUM:%s(0x%02X)",
             frame->rssi, (int)frame->len, id, man, version,
             medium_name(medium), medium);
    ESP_LOGI(TAG, "  T: %s", hex);

    if (strlen(id) == 0) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_data_t *m = find_or_create(id);
    if (m) {
        m->rssi      = frame->rssi;
        m->last_seen = (uint32_t)(esp_timer_get_time() / 1000);
        m->valid     = true;
        strlcpy(m->type, medium_name(medium), sizeof(m->type));

        // Nadpisz nazwa/typ z konfiguracji jesli licznik znany
        sih_config_t cfg = nvs_config_get();
        bool known = false;
        for (int i = 0; i < cfg.meter_count; i++) {
            if (strcasecmp(cfg.meters[i].id_hex, id) == 0) {
                strlcpy(m->type, cfg.meters[i].type, sizeof(m->type));
                strlcpy(m->name, cfg.meters[i].name, sizeof(m->name));
                known = true;
                break;
            }
        }
        if (!known && strlen(m->name) == 0)
            snprintf(m->name, sizeof(m->name), "%s-%s", man, id);

        add_field(m, "rssi_dbm", (float)frame->rssi, "dBm");
        add_field(m, "frame_len", (float)frame->len, "B");
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
