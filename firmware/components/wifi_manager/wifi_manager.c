#include "wifi_manager.h"
#include "nvs_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_eg;
static wifi_state_t       s_state = WIFI_STATE_DISCONNECTED;
static int                s_rssi  = 0;
static char               s_ip[16] = "0.0.0.0";
static esp_netif_t       *s_sta_netif = NULL;
static esp_netif_t       *s_ap_netif  = NULL;
static int                s_retry = 0;
#define MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        s_state = WIFI_STATE_CONNECTING;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < MAX_RETRY) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "Ponowne łączenie... (%d/%d)", s_retry, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
            s_state = WIFI_STATE_DISCONNECTED;
            ESP_LOGE(TAG, "Nie można połączyć — przełączam na AP");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_state = WIFI_STATE_CONNECTED;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Połączono: %s", s_ip);
    }
}

static void start_ap(const sih_config_t *cfg) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid,     cfg->ap_ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, cfg->ap_pass,  sizeof(ap_cfg.ap.password));
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_state = WIFI_STATE_AP_MODE;
    ESP_LOGI(TAG, "Tryb AP: %s", cfg->ap_ssid);
}

void wifi_manager_init(void) {
    s_wifi_eg = xEventGroupCreate();
    sih_config_t cfg = nvs_config_get();

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (strlen(cfg.wifi_ssid) == 0) {
        ESP_LOGW(TAG, "Brak SSID — start w trybie AP");
        start_ap(&cfg);
        return;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, cfg.wifi_pass,  sizeof(sta_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "STA failed — fallback AP");
        esp_wifi_stop();
        start_ap(&cfg);
    }
}

wifi_state_t wifi_manager_get_state(void) { return s_state; }

int wifi_manager_get_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
    return s_rssi;
}

void wifi_manager_get_ip(char *buf, size_t len) {
    strlcpy(buf, s_ip, len);
}

void wifi_manager_reconnect(const char *ssid, const char *pass) {
    sih_config_t cfg = nvs_config_get();
    strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
    nvs_config_save(&cfg);
    esp_restart();
}
