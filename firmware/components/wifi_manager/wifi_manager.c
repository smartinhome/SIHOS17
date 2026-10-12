#include "wifi_manager.h"
#include "nvs_config.h"
#include "mqtt_pub.h"
#include "esphome_api.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"
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
// Fallback dla runtime-loss STA: po pierwszym udanym GOT_IP flaga jest true.
// Jesli STA padnie na dluzej i s_runtime_retry przekroczy prog, robimy
// esp_restart() - wtedy wifi_manager_init sprobuje znowu STA (15 s timeout)
// i przy braku sieci sam skoczy do trybu AP. Bez tego moduł po runtime-loss
// STA zawisl w niekonczacych sie retry, znikajac z sieci na zawsze.
static bool               s_got_ip_once = false;
static int                s_runtime_retry = 0;
#define MAX_RETRY 5
#define RUNTIME_MAX_RETRY 60    // ~5-10 minut ciaglych probowan po utracie STA
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// ---------------------------------------------------------------------------
// beta394: staly adres IP w sieci domowej.
// Osobny blob "netcfg" w przestrzeni konfiguracji - reset fabryczny
// (nvs_erase_all) kasuje go razem z nia, a uklad sih_config_t zostaje bez zmian.
//
// Zly adres (np. spoza podsieci) odcialby modul od sieci na dobre, dlatego nowy
// adres jest na probe: po zapisaniu modul startuje z flaga "pending" i ma
// NETCFG_TRIAL_S sekund, zeby ktos otworzyl panel pod nowym adresem (panel sam
// to potwierdza). Bez potwierdzenia modul wraca do DHCP i restartuje sie.
// ---------------------------------------------------------------------------
#define NETCFG_KEY      "netcfg"
#define NETCFG_VER      1
#define NETCFG_TRIAL_S  300

typedef struct {
    uint8_t  ver, on, pending, rsv;
    uint32_t ip, mask, gw, dns;        // kolejnosc sieciowa, jak esp_ip4_addr_t.addr
} netcfg_t;

static netcfg_t           s_net;
static esp_timer_handle_t s_trial_timer = NULL;
static int64_t            s_trial_end_us = 0;

static void netcfg_load(void) {
    memset(&s_net, 0, sizeof(s_net));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    netcfg_t n;
    size_t sz = sizeof(n);
    if (nvs_get_blob(h, NETCFG_KEY, &n, &sz) == ESP_OK && sz == sizeof(n) && n.ver == NETCFG_VER)
        s_net = n;
    nvs_close(h);
}

static bool netcfg_store(const netcfg_t *n) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, NETCFG_KEY, n, sizeof(*n)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) s_net = *n;
    return ok;
}

static void ip_str(uint32_t a, char *buf, size_t len) {
    esp_ip4_addr_t x = { .addr = a };
    snprintf(buf, len, IPSTR, IP2STR(&x));
}

// NULL = poprawne, inaczej komunikat dla panelu.
static const char *netcfg_check(uint32_t ip, uint32_t mask, uint32_t gw) {
    if (!ip || !mask || !gw) return "Uzupełnij adres IP, maskę i bramę.";
    uint32_t m = __builtin_bswap32(mask);          // maska w kolejnosci hosta
    uint32_t inv = ~m;
    if (m == 0xFFFFFFFFu || (inv & (inv + 1)) != 0) return "Nieprawidłowa maska podsieci.";
    uint8_t first = (uint8_t)(ip & 0xFF);          // pierwszy oktet adresu
    if (first == 0 || first == 127 || first >= 224) return "Nieprawidłowy adres IP.";
    if ((ip & mask) != (gw & mask)) return "Brama musi być w tej samej podsieci co adres IP.";
    uint32_t hip = ip & ~mask, hgw = gw & ~mask;
    if (hip == 0 || hip == ~mask) return "To adres sieci albo rozgłoszeniowy - wybierz inny adres IP.";
    if (hgw == 0 || hgw == ~mask) return "Nieprawidłowy adres bramy.";
    if (ip == gw) return "Adres IP nie może być taki sam jak adres bramy.";
    return NULL;
}

// Przed esp_wifi_start: z zatrzymanym klientem DHCP esp_netif sam zglasza
// IP_EVENT_STA_GOT_IP przy kazdym polaczeniu z siecia (esp_netif_action_connected),
// takze po zerwaniu i powrocie Wi-Fi - reszta firmware dziala jak przy DHCP.
static void netcfg_apply(void) {
    if (!s_net.on || !s_sta_netif) return;
    esp_err_t e = esp_netif_dhcpc_stop(s_sta_netif);
    if (e != ESP_OK && e != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "Staly IP: nie mozna zatrzymac DHCP (%d) - zostaje DHCP", e);
        return;
    }
    esp_netif_ip_info_t ii = { 0 };
    ii.ip.addr = s_net.ip;
    ii.netmask.addr = s_net.mask;
    ii.gw.addr = s_net.gw;
    if (esp_netif_set_ip_info(s_sta_netif, &ii) != ESP_OK) {
        ESP_LOGE(TAG, "Staly IP: nie mozna ustawic adresu - wracam do DHCP");
        esp_netif_dhcpc_start(s_sta_netif);
        return;
    }
    // set_ip_info czysci serwery DNS, wiec ustawiamy je dopiero teraz. Bez DNS
    // nie zadziala zegar (pool.ntp.org), OTA z GitHuba ani broker po nazwie.
    esp_netif_dns_info_t d = { 0 };
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4.addr = s_net.dns ? s_net.dns : s_net.gw;
    esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d);
    if (s_net.dns && s_net.dns != s_net.gw) {
        d.ip.u_addr.ip4.addr = s_net.gw;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &d);
    }
    ESP_LOGI(TAG, "Staly adres IP: " IPSTR "%s", IP2STR(&ii.ip),
             s_net.pending ? " (na probe - czeka na otwarcie panelu)" : "");
}

static void trial_expired(void *arg) {
    (void)arg;
    if (!s_net.pending) return;
    ESP_LOGW(TAG, "Nikt nie otworzyl panelu pod nowym adresem w %d s - wracam do DHCP",
             NETCFG_TRIAL_S);
    netcfg_t n = s_net;
    n.on = 0;
    n.pending = 0;
    netcfg_store(&n);
    esp_restart();
}

static void netcfg_trial_start(void) {
    if (!s_net.on || !s_net.pending) return;
    const esp_timer_create_args_t a = { .callback = trial_expired, .name = "ip_trial" };
    if (esp_timer_create(&a, &s_trial_timer) != ESP_OK) return;
    esp_timer_start_once(s_trial_timer, (uint64_t)NETCFG_TRIAL_S * 1000000ULL);
    s_trial_end_us = esp_timer_get_time() + (int64_t)NETCFG_TRIAL_S * 1000000LL;
}

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
        if (s_got_ip_once) {
            // Runtime-loss: bylismy juz raz podlaczeni, wiec probujemy w kolko.
            // Po RUNTIME_MAX_RETRY nieudanych probach robimy restart, zeby
            // wifi_manager_init mogl zrobic czysty start-up (i ewentualnie
            // przejsc do trybu AP po 15 s czekania na WIFI_CONNECTED_BIT).
            esp_wifi_connect();
            s_runtime_retry++;
            s_state = WIFI_STATE_CONNECTING;
            if (s_runtime_retry <= RUNTIME_MAX_RETRY) {
                ESP_LOGW(TAG, "Runtime retry %d/%d - probuje odzyskac STA",
                         s_runtime_retry, RUNTIME_MAX_RETRY);
            } else {
                ESP_LOGE(TAG, "Runtime retry przekroczony - restart modulu "
                              "(przy braku STA wifi_manager_init przejdzie do AP)");
                esp_restart();   // deferred, bezpieczne z event_handler
            }
        } else if (s_retry < MAX_RETRY) {
            // Boot-time retry: krotszy limit, po nim fallback do AP przez WIFI_FAIL_BIT.
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
        s_runtime_retry = 0;
        s_got_ip_once = true;
        s_state = WIFI_STATE_CONNECTED;
        mqtt_pub_start();   // broker dostepny dopiero z adresem IP
        esphome_api_start();
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
    // Interfejs STA jest wlaczony WYLACZNIE po to, by dzialalo skanowanie sieci.
    // Bez jawnie pustej konfiguracji sterownik co ~2 s probuje szukac sieci do
    // polaczenia i zasypuje log ostrzezeniem "Haven't to connect to a suitable
    // AP now!". Kazda taka proba zabiera czas antenie wspoldzielonej z AP -
    // przy duzym ruchu radiowym gubily sie przez to zadania HTTP i panel
    // potrafil pokazac pusta liste licznikow.
    wifi_config_t sta_idle = {0};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_idle));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_state = WIFI_STATE_AP_MODE;
    mqtt_pub_stop();   // w trybie AP nie ma dostepu do brokera
    esphome_api_stop();
    esp_wifi_disconnect();   // STA ma byc bezczynny, tylko do skanowania
    // beta364: sterownik WiFi (tag "wifi", mala litera - nie nasz "WIFI") mimo
    // pustej konfiguracji STA i tak co chwile zglasza "Haven't to connect to a
    // suitable AP now!". Nie ma SSID, wiec nie ma proby polaczenia, przestrojenia
    // kanalu ani przerwy w pracy AP - to sam komunikat, nie zdarzenie. Kilkanascie
    // linii na sekunde wypychalo jednak z 8 KB bufora wszystko inne, wiec zakladka
    // Logi w trybie AP byla bezuzyteczna. Bledy sterownika nadal widac.
    esp_log_level_set("wifi", ESP_LOG_ERROR);
    ESP_LOGI(TAG, "Tryb AP: %s", cfg->ap_ssid);
}

void wifi_manager_init(void) {
    s_wifi_eg = xEventGroupCreate();
    const sih_config_t *cfg = nvs_config_ptr();
    netcfg_load();
    netcfg_trial_start();     // beta394: takze w trybie AP - inaczej zly adres zostalby na zawsze

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &event_handler, NULL));

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (strlen(cfg->wifi_ssid) == 0) {
        ESP_LOGW(TAG, "Brak SSID — start w trybie AP");
        start_ap(cfg);
        return;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, "sih-wmbus");
    netcfg_apply();           // beta394
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid,     cfg->wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, cfg->wifi_pass,  sizeof(sta_cfg.sta.password));

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
        start_ap(cfg);
    }
}

int wifi_manager_ap_clients(void) { return s_ap_clients; }

wifi_state_t wifi_manager_get_state(void) { return s_state; }

int wifi_manager_get_rssi(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) s_rssi = ap.rssi;
    return s_rssi;
}

int wifi_manager_get_channel(void) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.primary;
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

// ---------------------------------------------------------------------------
// beta394: staly adres IP - API dla panelu
// ---------------------------------------------------------------------------
int wifi_manager_netcfg_json(char *buf, int cap) {
    char ip[16], mask[16], gw[16], dns[16];
    char cip[16] = "0.0.0.0", cmask[16] = "0.0.0.0", cgw[16] = "0.0.0.0", cdns[16] = "0.0.0.0";
    ip_str(s_net.ip, ip, sizeof(ip));
    ip_str(s_net.mask, mask, sizeof(mask));
    ip_str(s_net.gw, gw, sizeof(gw));
    ip_str(s_net.dns, dns, sizeof(dns));
    if (s_sta_netif && s_state == WIFI_STATE_CONNECTED) {
        esp_netif_ip_info_t ii;
        if (esp_netif_get_ip_info(s_sta_netif, &ii) == ESP_OK) {
            ip_str(ii.ip.addr, cip, sizeof(cip));
            ip_str(ii.netmask.addr, cmask, sizeof(cmask));
            ip_str(ii.gw.addr, cgw, sizeof(cgw));
        }
        esp_netif_dns_info_t d;
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d) == ESP_OK)
            ip_str(d.ip.u_addr.ip4.addr, cdns, sizeof(cdns));
    }
    int left = 0;
    if (s_net.pending && s_trial_end_us) {
        int64_t l = (s_trial_end_us - esp_timer_get_time()) / 1000000LL;
        left = l > 0 ? (int)l : 0;
    }
    return snprintf(buf, cap,
        "{\"static\":%s,\"pending\":%s,\"pending_s\":%d,"
        "\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\",\"dns\":\"%s\","
        "\"cur_ip\":\"%s\",\"cur_mask\":\"%s\",\"cur_gw\":\"%s\",\"cur_dns\":\"%s\","
        "\"ap\":%s}",
        s_net.on ? "true" : "false", s_net.pending ? "true" : "false", left,
        s_net.on ? ip : "", s_net.on ? mask : "", s_net.on ? gw : "",
        (s_net.on && s_net.dns) ? dns : "",
        cip, cmask, cgw, cdns,
        s_state == WIFI_STATE_AP_MODE ? "true" : "false");
}

const char *wifi_manager_netcfg_save(bool on, const char *ip, const char *mask,
                                     const char *gw, const char *dns) {
    netcfg_t n;
    memset(&n, 0, sizeof(n));
    n.ver = NETCFG_VER;
    if (on) {
        esp_ip4_addr_t a = { 0 }, m = { 0 }, g = { 0 }, d = { 0 };
        if (!ip || esp_netif_str_to_ip4(ip, &a) != ESP_OK)     return "Nieprawidłowy adres IP.";
        if (!mask || esp_netif_str_to_ip4(mask, &m) != ESP_OK) return "Nieprawidłowa maska podsieci.";
        if (!gw || esp_netif_str_to_ip4(gw, &g) != ESP_OK)     return "Nieprawidłowy adres bramy.";
        if (dns && dns[0] && esp_netif_str_to_ip4(dns, &d) != ESP_OK) return "Nieprawidłowy adres DNS.";
        const char *err = netcfg_check(a.addr, m.addr, g.addr);
        if (err) return err;
        n.on = 1;
        n.pending = 1;
        n.ip = a.addr; n.mask = m.addr; n.gw = g.addr; n.dns = d.addr;
    }
    if (!netcfg_store(&n)) return "Nie udało się zapisać ustawień.";
    ESP_LOGI(TAG, "Zapisano adresacje: %s", on ? "staly adres IP (na probe)" : "DHCP");
    return NULL;
}

void wifi_manager_netcfg_confirm(void) {
    if (!s_net.pending) return;
    netcfg_t n = s_net;
    n.pending = 0;
    if (!netcfg_store(&n)) return;
    if (s_trial_timer) esp_timer_stop(s_trial_timer);
    s_trial_end_us = 0;
    ESP_LOGI(TAG, "Staly adres IP potwierdzony - panel otwarty pod nowym adresem");
}
