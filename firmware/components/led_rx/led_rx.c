#include "led_rx.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stddef.h>

#define LED_GPIO        19
#define LED_INVERTED    1          // True: niski stan = swieci
#define LED_TIMER       LEDC_TIMER_0
#define LED_CHANNEL     LEDC_CHANNEL_0
#define LED_MODE        LEDC_LOW_SPEED_MODE
#define LED_RES         LEDC_TIMER_8_BIT   // duty 0-255
#define LED_FREQ        5000
#define BLINK_MS_DEF    60
#define BLINK_MS_MIN    20
#define BLINK_MS_MAX    5000

static const char *TAG = "LED_RX";
static bool     s_enabled = true;
static uint8_t  s_brightness = 50;   // 0-100 %
static uint16_t s_blink_ms = BLINK_MS_DEF;

static uint16_t clamp_blink_ms(uint16_t ms) {
    if (ms < BLINK_MS_MIN) return BLINK_MS_DEF;
    if (ms > BLINK_MS_MAX) return BLINK_MS_MAX;
    return ms;
}
static bool     s_ready = false;
static esp_timer_handle_t s_off_timer = NULL;

// Ustaw surowy duty (0-255) uwzgledniajac inwersje.
static void led_set_duty(uint8_t duty) {
    // 8-bit PWM: pelne wypelnienie = 256 (nie 255). Przy inverted, duty=0
    // (zgaszona) musi dac stale wysoki stan -> d=256, inaczej zostaje ~0.4%
    // niskiego stanu i dioda lekko swieci.
    uint32_t d;
    if (LED_INVERTED) {
        d = 256 - (uint32_t)duty;   // duty=0 -> 256 (pelne high = zgaszona)
    } else {
        d = duty;
    }
    ledc_set_duty(LED_MODE, LED_CHANNEL, d);
    ledc_update_duty(LED_MODE, LED_CHANNEL);
}

// Zgas diode (wywolywane przez timer po blysku).
static void led_off_cb(void *arg) {
    led_set_duty(0);
}

void led_rx_init(bool enabled, uint8_t brightness, uint16_t blink_ms) {
    s_enabled = enabled;
    s_brightness = brightness > 100 ? 100 : brightness;
    s_blink_ms = clamp_blink_ms(blink_ms);

    ledc_timer_config_t tcfg = {
        .speed_mode      = LED_MODE,
        .timer_num       = LED_TIMER,
        .duty_resolution = LED_RES,
        .freq_hz         = LED_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    if (ledc_timer_config(&tcfg) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config blad");
        return;
    }
    ledc_channel_config_t ccfg = {
        .gpio_num   = LED_GPIO,
        .speed_mode = LED_MODE,
        .channel    = LED_CHANNEL,
        .timer_sel  = LED_TIMER,
        .duty       = LED_INVERTED ? 256 : 0,  // zgaszona (pelne high)
        .hpoint     = 0
    };
    if (ledc_channel_config(&ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config blad");
        return;
    }

    esp_timer_create_args_t targs = {
        .callback = led_off_cb,
        .name = "led_off"
    };
    esp_timer_create(&targs, &s_off_timer);

    s_ready = true;
    led_set_duty(0);  // zgaszona na starcie
    ESP_LOGI(TAG, "Dioda RX GPIO%d, %s, jasnosc %d%%, blysk %d ms",
             LED_GPIO, s_enabled ? "wlaczona" : "wylaczona", s_brightness, s_blink_ms);
}

void led_rx_blink(void) {
    if (!s_ready || !s_enabled) return;
    // jasnosc % -> duty 0-255
    uint8_t duty = (uint16_t)s_brightness * 255 / 100;
    led_set_duty(duty);
    // zaplanuj zgaszenie po s_blink_ms (restart timera jesli juz biegnie)
    if (s_off_timer) {
        esp_timer_stop(s_off_timer);
        esp_timer_start_once(s_off_timer, (uint64_t)s_blink_ms * 1000);
    }
}

void led_rx_set(bool enabled, uint8_t brightness, uint16_t blink_ms) {
    s_enabled = enabled;
    s_brightness = brightness > 100 ? 100 : brightness;
    s_blink_ms = clamp_blink_ms(blink_ms);
    if (!s_ready) return;
    if (!s_enabled) led_set_duty(0);  // zgas natychmiast gdy wylaczona
}
