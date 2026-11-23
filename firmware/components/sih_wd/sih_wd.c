#include "sih_wd.h"
#include <stdio.h>
#include <stdint.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "STRAZNIK";

#define WD_PERIOD_MS   10000
#define WD_WORKER_MS   90000    // zadanie ramek zglasza sie co <= 5 s
#define WD_HTTP_MS     180000   // kopia zapasowa ~2,5 MB po WiFi to kilkadziesiat s
#define WD_MAGIC       0x53574431u   // "SWD1"

static volatile TickType_t  s_worker_alive = 0;
static volatile bool        s_worker_seen = false;
static const char *volatile s_stage = NULL;
static volatile TickType_t  s_http_since = 0;
static const char *volatile s_http_uri = NULL;

// Przetrwa restart programowy (nie odciecie zasilania).
RTC_NOINIT_ATTR static uint32_t s_magic;
RTC_NOINIT_ATTR static uint32_t s_secs;
RTC_NOINIT_ATTR static char     s_what[24];
RTC_NOINIT_ATTR static char     s_detail[56];

void sih_wd_worker_alive(void) {
    s_worker_alive = xTaskGetTickCount();
    s_worker_seen = true;
}

void sih_wd_worker_stage(const char *stage) { s_stage = stage; }

void sih_wd_http_enter(const char *uri) {
    s_http_since = xTaskGetTickCount();
    s_http_uri = uri;
}

void sih_wd_http_leave(void) { s_http_uri = NULL; }

static const char *reset_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "wlaczenie zasilania";
        case ESP_RST_EXT:       return "pin resetu";
        case ESP_RST_SW:        return "restart programowy";
        case ESP_RST_PANIC:     return "blad krytyczny (panic)";
        case ESP_RST_INT_WDT:   return "watchdog przerwan";
        case ESP_RST_TASK_WDT:  return "watchdog zadan";
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "wybudzenie";
        case ESP_RST_BROWNOUT:  return "spadek napiecia (brownout)";
        default:                return "inny";
    }
}

static void trip(const char *what, const char *detail, uint32_t secs) {
    snprintf(s_what, sizeof(s_what), "%s", what);
    snprintf(s_detail, sizeof(s_detail), "%s", detail ? detail : "?");
    s_secs = secs;
    s_magic = WD_MAGIC;
    ESP_LOGE(TAG, "%s stoi od %u s (%s) - restart modulu", s_what, (unsigned)secs, s_detail);
    vTaskDelay(pdMS_TO_TICKS(200));   // log zdazy wyjsc
    esp_restart();
}

static void wd_task(void *arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WD_PERIOD_MS));
        TickType_t now = xTaskGetTickCount();
        if (s_worker_seen) {
            uint32_t ms = (uint32_t)(now - s_worker_alive) * portTICK_PERIOD_MS;
            if (ms > WD_WORKER_MS) {
                const char *st = s_stage;
                trip("Obrobka ramek", st ? st : "czeka na ramke", ms / 1000);
            }
        }
        const char *uri = s_http_uri;
        if (uri) {
            uint32_t ms = (uint32_t)(now - s_http_since) * portTICK_PERIOD_MS;
            if (ms > WD_HTTP_MS) trip("Zapytanie WWW", uri, ms / 1000);
        }
    }
}

void sih_wd_init(void) {
    ESP_LOGI(TAG, "Powod restartu: %s", reset_name(esp_reset_reason()));
    if (s_magic == WD_MAGIC) {
        s_what[sizeof(s_what) - 1] = 0;
        s_detail[sizeof(s_detail) - 1] = 0;
        ESP_LOGE(TAG, "Poprzedni restart: straznik - %s stalo %u s (%s)",
                 s_what, (unsigned)s_secs, s_detail);
    }
    s_magic = 0;
    // Priorytet 10: powyzej obrobki ramek (6), radia (7) i WWW - straznik
    // musi dostac CPU takze wtedy, gdy ktores z nich krazy w petli.
    xTaskCreate(wd_task, "sih_wd", 3072, NULL, 10, NULL);
}
