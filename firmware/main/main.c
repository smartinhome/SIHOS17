#include "app_main.h"
#include "wifi_manager.h"
#include "nvs_config.h"
#include "webserver.h"
#include "cc1101.h"
#include "wmbus_decoder.h"
#include "history.h"
#include "led_rx.h"
#include "led_status.h"
#include "ota_manager.h"
#include "display_eink.h"
#include "log_buffer.h"
#include "esp_heap_caps.h"

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

    // OTA: potwierdz sprawnosc firmware NATYCHMIAST po nvs, zanim cokolwiek
    // innego (wifi/radio/led/dekoder) zdazy ewentualnie crashnac. Bez tego
    // bootloader z wlaczonym rollbackiem cofnie obraz przy nastepnym restarcie.
    ota_manager_init();

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

    // Wyswietlacz e-ink (wspoldzieli magistrale SPI2 z CC1101 - dodaj jako 2. urzadzenie)
    display_eink_config_t eink_cfg = {
        .spi_host = CC1101_SPI_HOST,
        .pin_cs   = EINK_PIN_CS,
        .pin_dc   = EINK_PIN_DC,
        .pin_busy = EINK_PIN_BUSY,
        .pin_rst  = EINK_PIN_RST,
    };
    if (display_eink_init(&eink_cfg)) {
        display_eink_show_splash();  // logo SIH + QR (ekran startowy)
    }

    // Dekoder wMbus
    history_init();   // montuje SPIFFS, wczytuje historie
    {
        sih_config_t lc = nvs_config_get();
        led_rx_init(lc.led_enabled, lc.led_brightness, lc.led_blink_ms);
        led_status_init(lc.led_status_enabled, lc.led_status_brightness);
    }
    wmbus_decoder_init();

    // Serwer HTTP (REST API + Web UI)
    webserver_init();

    // Uruchom odbiór ramek
    cc1101_start_receive(wmbus_decoder_on_frame);

    // Taski wyswietlacza: przycisk BOOT (przelaczanie stron) + auto-odswiezanie.
    // Strony pokazuja sledzone liczniki z danymi z historii.
    display_eink_start_tasks(BOOT_BUTTON_PIN);

    // Diagnostyka pamieci: twarde liczby zamiast procentow (procent zajetosci
    // sterty myli, bo .bss nie jest w niej liczony - por. optymalizacja slotow).
    ESP_LOGI(TAG, "RAM: wolne %u B, min. wolne od startu %u B, calkowite %u B",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_DEFAULT));
    ESP_LOGI(TAG, "System gotowy");
}
