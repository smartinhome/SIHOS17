#include "app_main.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "webserver.h"
#include "cc1101.h"
#include "wmbus_decoder.h"
#include "history.h"
#include "ota_manager.h"
#include "log_buffer.h"

#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "MAIN";

void app_main(void) {
    log_buffer_init();
    ESP_LOGI(TAG, "SIH wMbus Reader v%s — start", FW_VERSION_STR);

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Netif + event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Konfiguracja z NVS
    nvs_config_init();

    // WiFi (STA + fallback AP)
    wifi_manager_init();

    // Radio CC1101
    cc1101_config_t radio_cfg = {
        .spi_host = CC1101_SPI_HOST,
        .pin_clk  = CC1101_PIN_CLK,
        .pin_mosi = CC1101_PIN_MOSI,
        .pin_miso = CC1101_PIN_MISO,
        .pin_cs   = CC1101_PIN_CS,
        .pin_gdo0 = CC1101_PIN_GDO0,
        .pin_gdo2 = CC1101_PIN_GDO2,
        .freq_mhz = WMBUS_FREQ_MHZ,
    };
    cc1101_init(&radio_cfg);

    // Dekoder wMbus
    history_init();   // montuje SPIFFS, wczytuje historie
    wmbus_decoder_init();

    // OTA manager
    ota_manager_init();

    // Serwer HTTP (REST API + Web UI)
    webserver_init();

    // Uruchom odbiór ramek
    cc1101_start_receive(wmbus_decoder_on_frame);

    ESP_LOGI(TAG, "System gotowy");
}
