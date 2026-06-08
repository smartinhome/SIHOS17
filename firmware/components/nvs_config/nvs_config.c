#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_CFG";
static sih_config_t g_cfg = {0};

// Domyślna konfiguracja
static void set_defaults(sih_config_t *c) {
    strlcpy(c->ap_ssid, "SIH-wMbus", sizeof(c->ap_ssid));
    strlcpy(c->ap_pass, "smartinhome", sizeof(c->ap_pass));
    c->freq_mhz    = 868.950f;
    c->meter_count = 0;
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
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Błąd odczytu konfiguracji, reset do domyślnej");
        set_defaults(&g_cfg);
    } else {
        ESP_LOGI(TAG, "Konfiguracja wczytana, %d liczników", g_cfg.meter_count);
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
