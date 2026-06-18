#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_CFG";
static sih_config_t g_cfg = {0};

// Domyślna konfiguracja
static void set_defaults(sih_config_t *c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->ap_ssid, "SIH-wMbus", sizeof(c->ap_ssid));
    strlcpy(c->ap_pass, "smartinhome", sizeof(c->ap_pass));
    c->freq_mhz    = 868.950f;
    c->meter_count = 0;
    c->led_enabled = true;     // dioda RX domyslnie wlaczona
    c->led_brightness = 50;    // 50% jasnosci
}

void nvs_config_init(void) {
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
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Błąd odczytu konfiguracji, reset do domyślnej");
        set_defaults(&g_cfg);
    } else {
        ESP_LOGI(TAG, "Konfiguracja wczytana, %d liczników", g_cfg.meter_count);
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
    ESP_LOGW(TAG, "Konfiguracja zresetowana do domyślnej");
}
