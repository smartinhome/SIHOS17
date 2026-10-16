#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>   // snprintf w migracji przypiec

static const char *TAG = "NVS_CFG";
static sih_config_t g_cfg = {0};

// Lista pol przypietych do dashboardu - osobny blob NVS "dashflds"
// (implementacja na koncu pliku). Tablica stoi tutaj, bo czysci ja takze
// nvs_config_reset(), zdefiniowany wyzej niz reszta obslugi.
static char g_dash_fields[MAX_DASH_FIELDS][DASH_FIELD_LEN];
static void dash_fields_load(void);
// Zegar na e-ink - pojedyncza flaga, wlasny klucz NVS (patrz koniec pliku).
static bool g_eink_clock = false;
static void eink_clock_load(void);

// Domyślna konfiguracja
static void set_defaults(sih_config_t *c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->ap_ssid, "SIH-wMbus", sizeof(c->ap_ssid));
    strlcpy(c->ap_pass, "smartinhome", sizeof(c->ap_pass));
    c->freq_mhz    = 868.950f;
    c->meter_count = 0;
    c->led_enabled = true;     // dioda RX domyslnie wlaczona
    c->led_brightness = 50;    // 50% - wskaznik odbioru (pomaranczowy)
    c->led_status_enabled = true;    // dioda RGB statusu domyslnie wlaczona
    c->led_status_brightness = 7;    // 7% - RGB statusu swieci ciagle, wiec
                                     // niska jasnosc wystarcza i nie razi
    c->led_only_pinned = false;      // domyslnie mrugaj dla KAZDEJ ramki
    c->led_blink_ms = 140;           // czas swiecenia po ramce - 140 ms jest
                                     // wyraznie widoczne dla oka
    c->logs_enabled = false;         // zakladka Logi domyslnie ukryta
    c->mqtt_enabled = false;         // MQTT domyslnie wylaczony
    c->mqtt_port = 1883;
    snprintf(c->mqtt_prefix, sizeof(c->mqtt_prefix), "sihos17");
    c->mqtt_ha_discovery = true;     // encje HA tworza sie same
}

void nvs_config_init(void) {
    dash_fields_load();   // osobny blob, niezalezny od powodzenia odczytu "config"
    eink_clock_load();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Brak konfiguracji w NVS, używam domyślnej");
        set_defaults(&g_cfg);
        return;
    }
    size_t sz = sizeof(sih_config_t);
    err = nvs_get_blob(h, "config", &g_cfg, &sz);
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        // Rozmiar struktury zmienil sie miedzy wersjami firmware.
        // Wczytaj tyle ile zapisano, reszte (nowe pola) zostaw wyzerowana,
        // zeby NIE stracic licznikow i kluczy po aktualizacji OTA.
        set_defaults(&g_cfg);
        size_t stored = 0;
        if (nvs_get_blob(h, "config", NULL, &stored) == ESP_OK && stored > 0 &&
            stored <= sizeof(sih_config_t)) {
            err = nvs_get_blob(h, "config", &g_cfg, &stored);
        }
    }
    nvs_close(h);
    if (err == ESP_OK && !g_cfg.pins_migrated) {
        // Jednorazowo: przenies przypiecia i nazwy ze starych, 8-elementowych
        // tablic do nowych (MAX_PINS). Stare zostaja w strukturze nietkniete,
        // zeby nie ruszac ukladu blobu NVS.
        for (int i = 0; i < MAX_METERS && i < MAX_PINS; i++) {
            if (g_cfg.dashboard_ids[i][0])
                snprintf(g_cfg.pins[i], sizeof(g_cfg.pins[0]), "%s", g_cfg.dashboard_ids[i]);
            if (g_cfg.names[i].id_hex[0]) {
                snprintf(g_cfg.names[i].id_hex, sizeof(g_cfg.names[0].id_hex), "%s",
                         g_cfg.meter_names[i].id_hex);
                snprintf(g_cfg.names[i].name, sizeof(g_cfg.names[0].name), "%s",
                         g_cfg.meter_names[i].name);
            }
        }
        g_cfg.pins_migrated = true;
        nvs_config_save(&g_cfg);
        ESP_LOGI(TAG, "Przypiecia i nazwy przeniesione do wiekszych tablic (limit %d)", MAX_PINS);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Błąd odczytu konfiguracji, reset do domyślnej");
        set_defaults(&g_cfg);
    } else {
        ESP_LOGI(TAG, "Konfiguracja wczytana, %d liczników", g_cfg.meter_count);
        // Normalizacja pol dodanych w nowszych wersjach (stary blob -> zera).
        if (g_cfg.led_blink_ms < 20 || g_cfg.led_blink_ms > 5000)
            g_cfg.led_blink_ms = 60;
        // Wyczysc smieciowe wpisy dashboard_ids (np. po migracji starej struktury):
        // poprawne ID = hex dlugosci 6-10. Reszta -> wyzeruj.
        bool cleaned = false;
        for (int i = 0; i < MAX_METERS; i++) {
            char *d = g_cfg.dashboard_ids[i];
            size_t n = strlen(d);
            bool ok = (n >= 6 && n <= 10);
            if (ok) {
                for (size_t k = 0; k < n; k++) {
                    char ch = d[k];
                    bool hex = (ch>='0'&&ch<='9')||(ch>='a'&&ch<='f')||(ch>='A'&&ch<='F');
                    if (!hex) { ok = false; break; }
                }
            }
            if (!ok && d[0] != 0) { d[0] = 0; cleaned = true; }
            if (d[0]) ESP_LOGI(TAG, "  dashboard[%d] = '%s'", i, d);
        }
        if (cleaned) {
            ESP_LOGW(TAG, "Wyczyszczono smieciowe wpisy dashboard, zapisuje");
            nvs_config_save(&g_cfg);
        }
    }
}

sih_config_t nvs_config_get(void) {
    return g_cfg;
}

const sih_config_t *nvs_config_ptr(void) { return &g_cfg; }

void nvs_config_save(const sih_config_t *cfg) {
    memcpy(&g_cfg, cfg, sizeof(sih_config_t));
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    ESP_ERROR_CHECK(nvs_set_blob(h, "config", cfg, sizeof(sih_config_t)));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Konfiguracja zapisana");
}

void nvs_config_reset(void) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h));
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
    set_defaults(&g_cfg);
    // nvs_erase_all wyczyscil takze blob "dashflds" i flage zegara - zeruj kopie w RAM.
    memset(g_dash_fields, 0, sizeof(g_dash_fields));
    g_eink_clock = false;
    ESP_LOGW(TAG, "Konfiguracja zresetowana do domyślnej");
}

// Zwraca wlasna nazwe licznika po ID (case-insensitive) lub "" gdy brak.
const char *nvs_config_meter_name(const char *id_hex) {
    if (!id_hex || !id_hex[0]) return "";
    for (int i = 0; i < MAX_PINS; i++) {
        if (g_cfg.names[i].id_hex[0] &&
            strcasecmp(g_cfg.names[i].id_hex, id_hex) == 0) {
            return g_cfg.names[i].name;
        }
    }
    return "";
}

// Ustawia wlasna nazwe licznika po ID. Pusta nazwa kasuje wpis.
void nvs_config_set_meter_name(const char *id_hex, const char *name) {
    if (!id_hex || !id_hex[0]) return;
    int idx = -1, free_idx = -1;
    for (int i = 0; i < MAX_PINS; i++) {
        if (g_cfg.names[i].id_hex[0]) {
            if (strcasecmp(g_cfg.names[i].id_hex, id_hex) == 0) { idx = i; break; }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    bool empty = (!name || !name[0]);
    if (idx >= 0) {
        if (empty) {
            // Kasuj wpis (pusta nazwa = powrot do ID).
            memset(&g_cfg.names[idx], 0, sizeof(g_cfg.names[idx]));
        } else {
            strlcpy(g_cfg.names[idx].name, name, sizeof(g_cfg.names[idx].name));
        }
    } else if (!empty && free_idx >= 0) {
        strlcpy(g_cfg.names[free_idx].id_hex, id_hex, sizeof(g_cfg.names[free_idx].id_hex));
        strlcpy(g_cfg.names[free_idx].name, name, sizeof(g_cfg.names[free_idx].name));
    } else if (!empty) {
        ESP_LOGW(TAG, "Brak miejsca na nazwe licznika %s", id_hex);
        return;
    }
    nvs_config_save(&g_cfg);
}

bool nvs_config_is_pinned(const char *id_hex) {
    if (!id_hex || !id_hex[0]) return false;
    for (int i = 0; i < MAX_PINS; i++) {
        if (g_cfg.pins[i][0] &&
            strcasecmp(g_cfg.pins[i], id_hex) == 0) return true;
    }
    return false;
}

bool nvs_config_led_only_pinned(void) {
    return g_cfg.led_only_pinned;
}

// ---------------------------------------------------------------------------
// Pola przypiete do dashboardu ("id:pole"). Osobny blob NVS "dashflds" w tym
// samym namespace - nie dotyka blobu "config", wiec zmiana tej listy nigdy nie
// moze uszkodzic zapisanych licznikow, kluczy AES ani przypiec.
// ---------------------------------------------------------------------------
#define DASH_FIELDS_KEY "dashflds"

static void dash_fields_load(void) {
    memset(g_dash_fields, 0, sizeof(g_dash_fields));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(g_dash_fields);
    esp_err_t err = nvs_get_blob(h, DASH_FIELDS_KEY, g_dash_fields, &sz);
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        // Lista zmienila rozmiar miedzy wersjami - wczytaj tyle, ile sie zmiesci.
        memset(g_dash_fields, 0, sizeof(g_dash_fields));
        size_t stored = 0;
        if (nvs_get_blob(h, DASH_FIELDS_KEY, NULL, &stored) == ESP_OK &&
            stored > 0 && stored <= sizeof(g_dash_fields)) {
            nvs_get_blob(h, DASH_FIELDS_KEY, g_dash_fields, &stored);
        }
    }
    nvs_close(h);
    // Zabezpieczenie przed blobem bez terminatora (uszkodzony wpis).
    for (int i = 0; i < MAX_DASH_FIELDS; i++)
        g_dash_fields[i][DASH_FIELD_LEN - 1] = 0;
}

static void dash_fields_store(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Nie moge otworzyc NVS na zapis pol dashboardu");
        return;
    }
    if (nvs_set_blob(h, DASH_FIELDS_KEY, g_dash_fields, sizeof(g_dash_fields)) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

bool nvs_config_dash_field_is_set(const char *key) {
    if (!key || !key[0]) return false;
    for (int i = 0; i < MAX_DASH_FIELDS; i++) {
        if (g_dash_fields[i][0] && strcasecmp(g_dash_fields[i], key) == 0) return true;
    }
    return false;
}

bool nvs_config_dash_fields_set(const char *key, bool on) {
    if (!key || !key[0]) return false;
    int idx = -1, free_idx = -1;
    for (int i = 0; i < MAX_DASH_FIELDS; i++) {
        if (!g_dash_fields[i][0]) { if (free_idx < 0) free_idx = i; }
        else if (strcasecmp(g_dash_fields[i], key) == 0) { idx = i; }
    }
    if (on) {
        if (idx >= 0) return true;              // juz jest, nic do roboty
        if (free_idx < 0) return false;         // brak miejsca
        strlcpy(g_dash_fields[free_idx], key, DASH_FIELD_LEN);
    } else {
        if (idx < 0) return true;               // i tak go nie ma
        memset(g_dash_fields[idx], 0, DASH_FIELD_LEN);
    }
    dash_fields_store();
    return true;
}

int nvs_config_dash_fields_json(char *buf, int cap) {
    if (!buf || cap < 3) return 0;
    int n = 0;
    n += snprintf(buf + n, cap - n, "[");
    bool first = true;
    for (int i = 0; i < MAX_DASH_FIELDS && n < cap - 2; i++) {
        if (!g_dash_fields[i][0]) continue;
        int w = snprintf(buf + n, cap - n, "%s\"%s\"", first ? "" : ",", g_dash_fields[i]);
        if (w < 0 || w >= cap - n) break;       // nie miesci sie - urwij czysto
        n += w;
        first = false;
    }
    n += snprintf(buf + n, cap - n, "]");
    return n;
}

// ---------------------------------------------------------------------------
// Zegar na wyswietlaczu e-ink: jedna flaga, wlasny klucz NVS "einkclk".
// Tak jak lista pol dashboardu - poza blobem "config", zeby aktualizacja OTA
// nie miala jak przesunac zadnego istniejacego pola konfiguracji.
// ---------------------------------------------------------------------------
#define EINK_CLOCK_KEY "einkclk"

static void eink_clock_load(void) {
    g_eink_clock = false;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, EINK_CLOCK_KEY, &v) == ESP_OK) g_eink_clock = (v != 0);
    nvs_close(h);
}

bool nvs_config_eink_clock(void) {
    return g_eink_clock;
}

void nvs_config_set_eink_clock(bool on) {
    g_eink_clock = on;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Nie moge zapisac flagi zegara e-ink");
        return;
    }
    if (nvs_set_u8(h, EINK_CLOCK_KEY, on ? 1 : 0) == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Zegar na e-ink: %s", on ? "wlaczony" : "wylaczony");
}
