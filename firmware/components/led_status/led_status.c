#include "led_status.h"
#include "wifi_manager.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

#define LED_GPIO        8
#define RMT_RESOLUTION  10000000   // 10 MHz -> 1 tick = 100 ns

static const char *TAG = "LED_ST";

static rmt_channel_handle_t  s_chan = NULL;
static rmt_encoder_handle_t  s_encoder = NULL;
static bool     s_enabled = true;
static uint8_t  s_brightness = 50;   // 0-100 %
static uint8_t  s_last_r = 0, s_last_g = 0, s_last_b = 0;
static bool     s_have_last = false;

// --- Enkoder WS2812: bajt -> symbole RMT (bit0/bit1 wg timingu WS2812) ---
// WS2812: T0H=0.4us T0L=0.85us, T1H=0.8us T1L=0.45us (tolerancja szeroka)
// przy 100ns/tick: T0H=4 T0L=8, T1H=8 T1L=4
static rmt_symbol_word_t s_bit0 = { .level0 = 1, .duration0 = 4,  .level1 = 0, .duration1 = 8 };
static rmt_symbol_word_t s_bit1 = { .level0 = 1, .duration0 = 8,  .level1 = 0, .duration1 = 4 };

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} ws2812_encoder_t;

static size_t ws2812_encode(rmt_encoder_t *encoder, rmt_channel_handle_t channel,
                            const void *primary_data, size_t data_size,
                            rmt_encode_state_t *ret_state) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_handle_t bytes = enc->bytes_encoder;
    rmt_encoder_handle_t copy = enc->copy_encoder;
    rmt_encode_state_t session = RMT_ENCODING_RESET;
    size_t encoded = 0;
    if (enc->state == 0) {
        encoded += bytes->encode(bytes, channel, primary_data, data_size, &session);
        if (session & RMT_ENCODING_COMPLETE) enc->state = 1;
        if (session & RMT_ENCODING_MEM_FULL) { *ret_state = RMT_ENCODING_MEM_FULL; return encoded; }
    }
    if (enc->state == 1) {
        encoded += copy->encode(copy, channel, &enc->reset_code, sizeof(enc->reset_code), &session);
        if (session & RMT_ENCODING_COMPLETE) { enc->state = RMT_ENCODING_RESET; *ret_state = RMT_ENCODING_COMPLETE; }
        if (session & RMT_ENCODING_MEM_FULL) { *ret_state = RMT_ENCODING_MEM_FULL; return encoded; }
    }
    return encoded;
}

static esp_err_t ws2812_del(rmt_encoder_t *encoder) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_del_encoder(enc->bytes_encoder);
    rmt_del_encoder(enc->copy_encoder);
    free(enc);
    return ESP_OK;
}

static esp_err_t ws2812_reset(rmt_encoder_t *encoder) {
    ws2812_encoder_t *enc = __containerof(encoder, ws2812_encoder_t, base);
    rmt_encoder_reset(enc->bytes_encoder);
    rmt_encoder_reset(enc->copy_encoder);
    enc->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

static esp_err_t make_encoder(rmt_encoder_handle_t *out) {
    ws2812_encoder_t *enc = calloc(1, sizeof(ws2812_encoder_t));
    if (!enc) return ESP_ERR_NO_MEM;
    enc->base.encode = ws2812_encode;
    enc->base.del = ws2812_del;
    enc->base.reset = ws2812_reset;
    rmt_bytes_encoder_config_t bcfg = {
        .bit0 = s_bit0, .bit1 = s_bit1,
        .flags = { .msb_first = 1 },
    };
    if (rmt_new_bytes_encoder(&bcfg, &enc->bytes_encoder) != ESP_OK) { free(enc); return ESP_FAIL; }
    rmt_copy_encoder_config_t ccfg = {};
    if (rmt_new_copy_encoder(&ccfg, &enc->copy_encoder) != ESP_OK) {
        rmt_del_encoder(enc->bytes_encoder); free(enc); return ESP_FAIL;
    }
    // reset code: >50us niskiego stanu
    enc->reset_code = (rmt_symbol_word_t){ .level0 = 0, .duration0 = 250, .level1 = 0, .duration1 = 250 };
    *out = &enc->base;
    return ESP_OK;
}

// Wyslij kolor (skalowany jasnoscia). WS2812 oczekuje kolejnosci GRB.
static void led_show(uint8_t r, uint8_t g, uint8_t b) {
    s_last_r = r; s_last_g = g; s_last_b = b; s_have_last = true;
    if (!s_chan || !s_encoder) return;
    uint8_t br = s_enabled ? s_brightness : 0;
    // WS2812 na tym module pracuje w kolejnosci RGB (jak w konfiguracji uzytkownika)
    uint8_t rgb[3] = {
        (uint8_t)((uint16_t)r * br / 100),
        (uint8_t)((uint16_t)g * br / 100),
        (uint8_t)((uint16_t)b * br / 100),
    };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_encoder, rgb, sizeof(rgb), &tx);
    rmt_tx_wait_all_done(s_chan, 50);
}

// Zadanie: co sekunde sprawdza stan WiFi i ustawia kolor.
static void led_status_task(void *arg) {
    wifi_state_t last = (wifi_state_t)(-1);
    while (1) {
        wifi_state_t st = wifi_manager_get_state();
        if (st != last || !s_have_last) {
            last = st;
            if (st == WIFI_STATE_CONNECTED) led_show(0, 255, 0);   // zielony
            else                            led_show(255, 0, 0);   // czerwony
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void led_status_init(bool enabled, uint8_t brightness) {
    s_enabled = enabled;
    s_brightness = brightness > 100 ? 100 : brightness;

    rmt_tx_channel_config_t chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&chan_cfg, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "Nie udalo sie utworzyc kanalu RMT");
        return;
    }
    if (make_encoder(&s_encoder) != ESP_OK) {
        ESP_LOGE(TAG, "Nie udalo sie utworzyc enkodera WS2812");
        return;
    }
    rmt_enable(s_chan);
    led_show(255, 0, 0);   // start: czerwony (brak polaczenia)
    xTaskCreate(led_status_task, "led_status", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "Dioda RGB status na GPIO%d: %s, jasnosc %d%%",
             LED_GPIO, s_enabled ? "wlaczona" : "wylaczona", s_brightness);
}

void led_status_set(bool enabled, uint8_t brightness) {
    s_enabled = enabled;
    s_brightness = brightness > 100 ? 100 : brightness;
    // odswiez biezacy kolor z nowa jasnoscia/stanem
    if (s_have_last) led_show(s_last_r, s_last_g, s_last_b);
}
