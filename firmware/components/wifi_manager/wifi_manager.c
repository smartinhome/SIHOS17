#include "wifi_manager.h"
#include "nvs_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WIFI";
static EventGroupHandle_t s_wifi_eg;
static wifi_state_t       s_state = WIFI_STATE_DISCONNECTED;
static int                s_rssi  = 0;
static int                s_last_disc_reason = 0;  // powod ostatniego rozlaczenia STA
static char               s_ip[16] = "0.0.0.0";
static esp_netif_t       *s_sta_netif = NULL;
// W trybie AP interfejs STA jest wlaczony (APSTA) TYLKO po to, by dzialalo
// skanowanie sieci. Nie moze wtedy probowac sie laczyc - kazda proba to
// przerwa w pracy AP i rozlaczenie telefonu/laptopa.
static bool               s_sta_autoconnect = false;
// Ilu klientow (telefon/laptop) jest podlaczonych do naszego AP.
// Uzywane przez diode RGB: niebieski = ktos podlaczony, czerwony = nikt.
static volatile int       s_ap_clients = 0;
static esp_netif_t       *s_ap_netif  = NULL;
static int                s_retry = 0;
#define MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        ESP_LOGI(TAG, "AP: klient podlaczony (razem %d)", s_ap_clients);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
        ESP_LOGI(TAG, "AP: klient odlaczony (zostalo %d)", s_ap_clients);
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!s_sta_autoconnect) return;   // APSTA w trybie AP - tylko do skanowania
        esp_wifi_connect();
        s_state = WIFI_STATE_CONNECTING;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)data;
        if (dis) { ESP_LOGW(TAG, "Rozlaczono z WiFi, powod = %d", dis->reason); s_last_disc_reason = dis->reason; }
        if (!s_sta_autoconnect) return;   // tryb AP - nie ponawiaj polaczen STA
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
        s_last_disc_reason = 0;  // polaczono - wyczysc powod bledu
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Połączono: %s", s_ip);
        // Synchronizacja czasu (SNTP) - dla timestampow HH:MM:SS w logach
        if (esp_sntp_enabled() == false) {
            setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);  // strefa Polska
            tzset();
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP: synchronizacja czasu uruchomiona");
        }
    }
}

static void start_ap(const sih_config_t *cfg) {
    s_sta_autoconnect = false;   // w AP interfejs STA sluzy wylacznie do skanowania
    s_ap_clients = 0;
    if (!s_ap_netif)  s_ap_netif  = esp_netif_create_default_wifi_ap();
    if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid,     cfg->ap_ssid, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, cfg->ap_pass,  sizeof(ap_cfg.ap.password));
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    // APSTA (nie czysty AP): skanowanie wymaga interfejsu STA. Dzieki temu
    // /api/wifi/scan nie musi przelaczac trybu, co rozlaczalo klientow AP.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
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
    esp_netif_set_hostname(s_sta_netif, "sih-wmbus");
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, cfg.wifi_pass,  sizeof(sta_cfg.sta.password));

    s_sta_autoconnect = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Wylacz oszczedzanie energii WiFi — modul zasilany z sieci, a power save
    // na ESP32-C6 bywa przyczyna losowych rozlaczen z niektorymi routerami.
    esp_wifi_set_ps(WIFI_PS_NONE);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "STA failed — fallback AP");
        esp_wifi_stop();
        start_ap(&cfg);
    }
}

int wifi_manager_ap_clients(void) { return s_ap_clients; }

wifi_state_t wifi_manager_get_state(void) { return s_state; }

int wifi_manager_get_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
    return s_rssi;
}

void wifi_manager_get_ip(char *buf, size_t len) {
    // W trybie AP s_ip zostaje "0.0.0.0" (jest ustawiany dopiero po uzyskaniu
    // adresu w sieci domowej), wiec adres bramki czytamy wprost z interfejsu AP.
    // Nie wpisujemy 192.168.4.1 na sztywno - gdyby konfiguracja AP kiedys sie
    // zmienila, adres na ekranie nadal bylby prawdziwy.
    if (s_state == WIFI_STATE_AP_MODE && s_ap_netif) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK && ip.ip.addr) {
            snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
            return;
        }
    }
    strlcpy(buf, s_ip, len);
}

void wifi_manager_reconnect(const char *ssid, const char *pass) {
    sih_config_t cfg = nvs_config_get();
    strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_pass, pass, sizeof(cfg.wifi_pass));
    nvs_config_save(&cfg);
    esp_restart();
}

// ---- Skanowanie sieci WiFi ----
// Dziala w STA i AP. W trybie AP modul pracuje w APSTA od startu (patrz start_ap),
// wiec skan NIE przelacza trybu - przelaczanie rozlaczalo klientow AP i odpowiedz
// HTTP nie docierala do przegladarki.
int wifi_manager_scan(char *json_buf, int cap) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);

    // Awaryjnie: gdyby modul byl w czystym AP (starsza konfiguracja), wlacz APSTA
    // i JUZ w nim zostan - powrot do czystego AP zrywal polaczenie z klientem.
    if (mode == WIFI_MODE_AP) {
        if (!s_sta_netif) s_sta_netif = esp_netif_create_default_wifi_sta();
        s_sta_autoconnect = false;
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
            ESP_LOGW(TAG, "scan: nie udalo sie wlaczyc APSTA");
            snprintf(json_buf, cap, "[]");
            return -1;
        }
    }

    // Krotki czas na kanal: skan blokuje radio, a w trybie AP kazda milisekunda
    // to przerwa w obsludze podlaczonego telefonu/laptopa.
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL, .bssid = NULL, .channel = 0, .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 40, .max = 90 } },
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* blokujaco */);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start blad: %d", err);
        snprintf(json_buf, cap, "[]");
        return -1;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) ap_count = 20; // ogranicz
    if (ap_count == 0) { snprintf(json_buf, cap, "[]"); return 0; }
    wifi_ap_record_t *recs = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!recs) {
        snprintf(json_buf, cap, "[]");
        return -1;
    }
    uint16_t got = ap_count;
    esp_wifi_scan_get_ap_records(&got, recs);

    // Zbuduj JSON, pomijajac duplikaty SSID (najsilniejszy wygrywa) i puste SSID
    int n = snprintf(json_buf, cap, "[");
    int written = 0;
    for (int i = 0; i < got; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (strlen(ssid) == 0) continue;
        // duplikat?
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp((const char *)recs[j].ssid, ssid) == 0) { dup = true; break; }
        }
        if (dup) continue;
        // escapowanie cudzyslowu w SSID
        char esc[65]; int e = 0;
        for (int k = 0; ssid[k] && e < 63; k++) {
            if (ssid[k] == '"' || ssid[k] == '\\') esc[e++] = '\\';
            esc[e++] = ssid[k];
        }
        esc[e] = 0;
        n += snprintf(json_buf + n, cap - n, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                      written ? "," : "", esc, recs[i].rssi, recs[i].authmode);
        written++;
        if (n > cap - 80) break;
    }
    snprintf(json_buf + n, cap - n, "]");
    free(recs);

    return written;
}

int wifi_manager_last_disc_reason(void) {
    return s_last_disc_reason;
}
