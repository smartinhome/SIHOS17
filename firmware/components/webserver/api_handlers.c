#include "api_handlers.h"
#include <time.h>
#include "wmbus_decoder.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "ota_manager.h"
#include "history.h"
#include "led_rx.h"
#include "led_status.h"
#include "log_buffer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "driver/temperature_sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>

static const char *TAG = "API";

static void resp_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void resp_ok(httpd_req_t *req) {
    resp_json(req, "{\"status\":\"ok\"}");
}

static void resp_err(httpd_req_t *req, const char *msg) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, "400 Bad Request");
    resp_json(req, buf);
}

static int read_body(httpd_req_t *req, char *buf, size_t max) {
    int total = 0, ret;
    while ((ret = httpd_req_recv(req, buf + total, max - total - 1)) > 0)
        total += ret;
    buf[total] = 0;
    return total;
}

// ---------- Historia ----------
static esp_err_t handle_history_list(httpd_req_t *req) {
    static char buf[1024];
    history_list_json(buf, sizeof(buf));
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_history(httpd_req_t *req) {
    char id[40] = {0};
    char res[8] = "hour";
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[40];
        if (httpd_query_key_value(query, "id", val, sizeof(val)) == ESP_OK)
            strlcpy(id, val, sizeof(id));
        if (httpd_query_key_value(query, "res", val, sizeof(val)) == ESP_OK)
            strlcpy(res, val, sizeof(res));
    }
    if (strlen(id) == 0) { resp_err(req, "brak id"); return ESP_OK; }
    char *buf = malloc(8192);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
    history_get_json(id, res, buf, 8192);
    resp_json(req, buf);
    free(buf);
    return ESP_OK;
}

// GET /api/history/day?id=...&date=YYYY-MM-DD -> godzinowe zuzycie z danego dnia
static esp_err_t handle_history_day(httpd_req_t *req) {
    char id[40] = {0};
    char date[16] = {0};
    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[40];
        if (httpd_query_key_value(query, "id", val, sizeof(val)) == ESP_OK)
            strlcpy(id, val, sizeof(id));
        if (httpd_query_key_value(query, "date", val, sizeof(val)) == ESP_OK)
            strlcpy(date, val, sizeof(date));
    }
    if (strlen(id) == 0) { resp_err(req, "brak id"); return ESP_OK; }
    // Parsuj YYYY-MM-DD na unix timestamp (poludnie lokalne, by floor_day trafil w dzien).
    struct tm tmd = {0};
    int y, mo, d;
    if (sscanf(date, "%d-%d-%d", &y, &mo, &d) != 3) { resp_err(req, "zla data"); return ESP_OK; }
    tmd.tm_year = y - 1900; tmd.tm_mon = mo - 1; tmd.tm_mday = d;
    tmd.tm_hour = 12; tmd.tm_min = 0; tmd.tm_sec = 0;
    uint32_t day_ts = (uint32_t)mktime(&tmd);
    char *buf = malloc(8192);
    if (!buf) { httpd_resp_send_500(req); return ESP_OK; }
    history_get_day_json(id, day_ts, buf, 8192);
    resp_json(req, buf);
    free(buf);
    return ESP_OK;
}

// POST /api/history/track  body: {"id":"...","tracked":true|false}
static esp_err_t handle_history_tracked(httpd_req_t *req) {
    static char buf[1024];
    history_tracked_json(buf, sizeof(buf));
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_history_track(httpd_req_t *req) {
    char body[128];
    read_body(req, body, sizeof(body));
    char id[40] = {0};
    char *p;
    if ((p = strstr(body, "\"id\":\""))) sscanf(p, "\"id\":\"%39[^\"]\"", id);
    if (strlen(id) == 0) { resp_err(req, "brak id"); return ESP_OK; }
    bool tracked = (strstr(body, "\"tracked\":true") != NULL);
    history_set_tracked(id, tracked);
    resp_ok(req);
    return ESP_OK;
}

// Globalny uchwyt czujnika temperatury (inicjalizowany raz)
static temperature_sensor_handle_t s_temp_sensor = NULL;
static bool s_temp_ready = false;

static void system_temp_init(void) {
    // Wycisz jednorazowy log "Range [-10C ~ 80C]" z komponentu ESP-IDF.
    esp_log_level_set("temperature_sensor", ESP_LOG_WARN);
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_temp_sensor) == ESP_OK &&
        temperature_sensor_enable(s_temp_sensor) == ESP_OK) {
        s_temp_ready = true;
    }
}

static const char *reset_reason_str(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "wlaczenie zasilania";
        case ESP_RST_SW:        return "restart programowy";
        case ESP_RST_PANIC:     return "panika (crash)";
        case ESP_RST_INT_WDT:   return "watchdog (przerwania)";
        case ESP_RST_TASK_WDT:  return "watchdog (task)";
        case ESP_RST_WDT:       return "watchdog (inny)";
        case ESP_RST_DEEPSLEEP: return "wybudzenie z deep sleep";
        case ESP_RST_BROWNOUT:  return "spadek napiecia (brownout)";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "nieznany";
    }
}

static esp_err_t handle_system(httpd_req_t *req) {
    // IP biezacego polaczenia (do informacji systemowych)
    char sys_ip[16];
    wifi_manager_get_ip(sys_ip, sizeof(sys_ip));

    // Chip info
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *model = "nieznany";
    if (chip.model == CHIP_ESP32C6) model = "ESP32-C6";
    else if (chip.model == CHIP_ESP32) model = "ESP32";
    else if (chip.model == CHIP_ESP32C3) model = "ESP32-C3";
    else if (chip.model == CHIP_ESP32S3) model = "ESP32-S3";

    // Pamiec RAM (heap)
    uint32_t heap_free = esp_get_free_heap_size();
    uint32_t heap_min  = esp_get_minimum_free_heap_size();
    uint32_t heap_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    uint32_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    int heap_used_pct = heap_total ? (int)(100 - (heap_free * 100 / heap_total)) : 0;

    // Flash
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    // Aplikacja - rozmiar partycji
    const esp_partition_t *run = esp_ota_get_running_partition();
    uint32_t app_part_size = run ? run->size : 0;

    // CPU - czestotliwosc z konfiguracji kompilacji (pewne, bez zaleznosci od wersji API)
    uint32_t cpu_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    // Temperatura wewnetrzna
    float temp_c = 0;
    bool temp_ok = false;
    if (s_temp_ready && temperature_sensor_get_celsius(s_temp_sensor, &temp_c) == ESP_OK) {
        temp_ok = true;
    }

    // MAC
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Taski + uptime
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    // App description
    const esp_app_desc_t *app = esp_app_get_description();

    char buf[900];
    snprintf(buf, sizeof(buf),
        "{"
        "\"chip_model\":\"%s\",\"chip_rev\":%d,\"cpu_cores\":%d,"
        "\"cpu_freq_mhz\":%u,"
        "\"features\":{\"wifi\":%s,\"bt\":%s,\"ieee802154\":%s},"
        "\"heap_free\":%u,\"heap_min\":%u,\"heap_total\":%u,"
        "\"heap_largest\":%u,\"heap_used_pct\":%d,"
        "\"flash_size\":%u,\"app_part_size\":%u,"
        "\"temp_c\":%.1f,\"temp_ok\":%s,"
        "\"mac\":\"%s\",\"task_count\":%u,\"uptime_s\":%lld,"
        "\"wifi_rssi\":%d,\"ip\":\"%s\",\"meter_count\":%d,"
        "\"reset_reason\":\"%s\","
        "\"idf_ver\":\"%s\",\"compile_time\":\"%s %s\",\"app_ver\":\"%s\""
        "}",
        model, chip.revision, chip.cores,
        (unsigned)cpu_freq_mhz,
        (chip.features & CHIP_FEATURE_WIFI_BGN) ? "true" : "false",
        (chip.features & CHIP_FEATURE_BT) ? "true" : "false",
        (chip.features & CHIP_FEATURE_IEEE802154) ? "true" : "false",
        (unsigned)heap_free, (unsigned)heap_min, (unsigned)heap_total,
        (unsigned)heap_largest, heap_used_pct,
        (unsigned)flash_size, (unsigned)app_part_size,
        temp_ok ? temp_c : 0.0f, temp_ok ? "true" : "false",
        mac_str, (unsigned)task_count, (long long)uptime_s,
        wifi_manager_get_rssi(), sys_ip, wmbus_decoder_get_count(),
        reset_reason_str(),
        app->idf_ver, app->date, app->time, app->version
    );
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_status(httpd_req_t *req) {
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_state_t ws = wifi_manager_get_state();
    const char *wifi_str[] = {"disconnected","connecting","connected","ap_mode"};
    sih_config_t scfg = nvs_config_get();
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"uptime_ms\":%" PRId64 ","
        "\"wifi_state\":\"%s\",\"wifi_rssi\":%d,"
        "\"ssid\":\"%s\","
        "\"wifi_disc_reason\":%d,"
        "\"ip\":\"%s\",\"meter_count\":%d,\"partition\":\"%s\"}",
        ota_get_running_version(),
        esp_timer_get_time() / 1000,
        wifi_str[ws],
        wifi_manager_get_rssi(),
        scfg.wifi_ssid,
        wifi_manager_last_disc_reason(),
        ip,
        wmbus_decoder_get_count(),
        ota_get_partition_label()
    );
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_meters(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "[");
    int count = wmbus_decoder_get_count();
    for (int i = 0; i < count; i++) {
        meter_data_t *m = wmbus_decoder_get_meter(i);
        if (!m) continue;
        char mbuf[512];
        int n = snprintf(mbuf, sizeof(mbuf),
            "{\"id\":\"%s\",\"type\":\"%s\",\"name\":\"%s\","
            "\"rssi\":%d,\"last_seen\":%" PRIu32 ",\"fields\":[",
            m->id_hex, m->type, m->name, m->rssi, m->last_seen
        );
        for (int j = 0; j < m->field_count; j++) {
            n += snprintf(mbuf + n, sizeof(mbuf) - n,
                "%s{\"field\":\"%s\",\"value\":%.3f,\"unit\":\"%s\"}",
                j > 0 ? "," : "",
                m->fields[j].field, m->fields[j].value, m->fields[j].unit
            );
        }
        snprintf(mbuf + n, sizeof(mbuf) - n, "]}");
        if (i > 0) httpd_resp_sendstr_chunk(req, ",");
        httpd_resp_sendstr_chunk(req, mbuf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Surowe ramki wMbus z bufora (najnowsze pierwsze) — Web UI dekoduje je lokalnie
static esp_err_t handle_frames(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr_chunk(req, "[");
    int count = wmbus_decoder_raw_count();
    for (int i = 0; i < count; i++) {
        const raw_frame_t *r = wmbus_decoder_raw_get(i);
        if (!r) continue;
        char buf[MAX_RAW_HEX + 96];
        snprintf(buf, sizeof(buf),
            "%s{\"hex\":\"%s\",\"rssi\":%d,\"lqi\":%u,\"ts\":%" PRIu32 ",\"ts_unix\":%" PRIu32 "}",
            i > 0 ? "," : "", r->hex, r->rssi, (unsigned)r->lqi, r->ts_ms, r->ts_unix);
        httpd_resp_sendstr_chunk(req, buf);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// Dodaj/zaktualizuj licznik wraz z kluczem AES (zapis do NVS)
// GET /api/led -> {"enabled":true,"brightness":50}
// POST /api/led body: {"enabled":true,"brightness":70}
// GET /api/dashboard -> {"pinned":["56989134","215f1155"]}
// POST /api/dashboard body: {"id":"56989134","pinned":true}
// Sprawdza czy ID licznika jest poprawne (hex, dlugosc 6-10).
// Chroni przed smieciami w NVS (np. "   ") po migracji starej struktury.
static bool valid_meter_id(const char *id) {
    size_t n = strlen(id);
    if (n < 6 || n > 10) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = id[i];
        bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
        if (!hex) return false;
    }
    return true;
}

static esp_err_t handle_dashboard(httpd_req_t *req) {
    sih_config_t cfg = nvs_config_get();
    if (req->method == HTTP_POST) {
        char body[96];
        read_body(req, body, sizeof(body));
        char id[12] = {0};
        char *p = strstr(body, "\"id\":\"");
        if (p) {
            p += 6;
            int n = 0;
            while (*p && *p != '"' && n < 11) id[n++] = *p++;
            id[n] = 0;
        }
        if (!valid_meter_id(id)) { resp_err(req, "zle id"); return ESP_OK; }
        bool want = (strstr(body, "\"pinned\":true") != NULL);
        ESP_LOGI(TAG, "DASH POST: id='%s' want=%d", id, want);
        // znajdz ID na liscie; slot pusty LUB ze smieciem (niepoprawne id) = wolny
        int idx = -1, freeIdx = -1;
        for (int i = 0; i < MAX_METERS; i++) {
            if (!valid_meter_id(cfg.dashboard_ids[i])) {
                cfg.dashboard_ids[i][0] = 0;       // wyczysc smiec
                if (freeIdx < 0) freeIdx = i;
            } else if (strcasecmp(cfg.dashboard_ids[i], id) == 0) {
                idx = i;
            }
        }
        ESP_LOGI(TAG, "DASH POST: idx=%d freeIdx=%d", idx, freeIdx);
        if (want && idx < 0 && freeIdx >= 0) {
            strlcpy(cfg.dashboard_ids[freeIdx], id, sizeof(cfg.dashboard_ids[freeIdx]));
            nvs_config_save(&cfg);
            ESP_LOGI(TAG, "DASH POST: ZAPISANO '%s' na pozycji %d", id, freeIdx);
        } else if (!want && idx >= 0) {
            cfg.dashboard_ids[idx][0] = 0;
            nvs_config_save(&cfg);
            ESP_LOGI(TAG, "DASH POST: USUNIETO z pozycji %d", idx);
        } else {
            ESP_LOGW(TAG, "DASH POST: NIC nie zapisano (want=%d idx=%d freeIdx=%d)", want, idx, freeIdx);
        }
        resp_ok(req);
        return ESP_OK;
    }
    char buf[256];
    int pos = snprintf(buf, sizeof(buf), "{\"pinned\":[");
    bool first = true;
    for (int i = 0; i < MAX_METERS; i++) {
        if (valid_meter_id(cfg.dashboard_ids[i])) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"%s\"",
                            first ? "" : ",", cfg.dashboard_ids[i]);
            first = false;
        }
    }
    snprintf(buf + pos, sizeof(buf) - pos, "]}");
    resp_json(req, buf);
    return ESP_OK;
}

// GET  /api/meter/names -> {"id1":"nazwa1","id2":"nazwa2",...}
// POST /api/meter/name  body {"id":"56989134","name":"Licznik glowny"}
//   pusta nazwa kasuje wpis (powrot do ID).
static esp_err_t handle_meter_name(httpd_req_t *req) {
    if (req->method == HTTP_POST) {
        char body[160];
        read_body(req, body, sizeof(body));
        char id[12] = {0};
        char name[32] = {0};
        char *p = strstr(body, "\"id\":\"");
        if (p) { p += 6; int n = 0; while (*p && *p != '"' && n < 11) id[n++] = *p++; id[n] = 0; }
        p = strstr(body, "\"name\":\"");
        if (p) { p += 8; int n = 0; while (*p && *p != '"' && n < 31) name[n++] = *p++; name[n] = 0; }
        if (!valid_meter_id(id)) { resp_err(req, "zle id"); return ESP_OK; }
        nvs_config_set_meter_name(id, name);
        ESP_LOGI(TAG, "NAZWA: id='%s' name='%s'", id, name);
        resp_ok(req);
        return ESP_OK;
    }
    // GET - zwroc mape ID -> nazwa (tylko wpisy z nazwa).
    sih_config_t cfg = nvs_config_get();
    char buf[MAX_METERS * 48 + 4];
    int pos = snprintf(buf, sizeof(buf), "{");
    bool first = true;
    for (int i = 0; i < MAX_METERS; i++) {
        if (cfg.meter_names[i].id_hex[0] && cfg.meter_names[i].name[0]) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s\"%s\":\"%s\"",
                            first ? "" : ",", cfg.meter_names[i].id_hex, cfg.meter_names[i].name);
            first = false;
        }
    }
    snprintf(buf + pos, sizeof(buf) - pos, "}");
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_led(httpd_req_t *req) {
    sih_config_t cfg = nvs_config_get();
    if (req->method == HTTP_POST) {
        char body[160];
        read_body(req, body, sizeof(body));
        cfg.led_enabled = (strstr(body, "\"enabled\":true") != NULL);
        cfg.led_only_pinned = (strstr(body, "\"only_pinned\":true") != NULL);
        char *p = strstr(body, "\"brightness\":");
        if (p) {
            int b = atoi(p + 13);
            if (b < 0) b = 0;
            if (b > 100) b = 100;
            cfg.led_brightness = (uint8_t)b;
        }
        p = strstr(body, "\"blink_ms\":");
        if (p) {
            int ms = atoi(p + 11);
            if (ms < 20) ms = 20;
            if (ms > 5000) ms = 5000;
            cfg.led_blink_ms = (uint16_t)ms;
        }
        nvs_config_save(&cfg);
        led_rx_set(cfg.led_enabled, cfg.led_brightness, cfg.led_blink_ms);
        resp_ok(req);
        return ESP_OK;
    }
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"enabled\":%s,\"brightness\":%d,\"only_pinned\":%s,\"blink_ms\":%d}",
             cfg.led_enabled ? "true" : "false", cfg.led_brightness,
             cfg.led_only_pinned ? "true" : "false", cfg.led_blink_ms);
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_led_status(httpd_req_t *req) {
    sih_config_t cfg = nvs_config_get();
    if (req->method == HTTP_POST) {
        char body[96];
        read_body(req, body, sizeof(body));
        cfg.led_status_enabled = (strstr(body, "\"enabled\":true") != NULL);
        char *p = strstr(body, "\"brightness\":");
        if (p) {
            int b = atoi(p + 13);
            if (b < 0) b = 0;
            if (b > 100) b = 100;
            cfg.led_status_brightness = (uint8_t)b;
        }
        nvs_config_save(&cfg);
        led_status_set(cfg.led_status_enabled, cfg.led_status_brightness);
        resp_ok(req);
        return ESP_OK;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"enabled\":%s,\"brightness\":%d}",
             cfg.led_status_enabled ? "true" : "false", cfg.led_status_brightness);
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_config_meter(httpd_req_t *req) {
    char body[512];
    read_body(req, body, sizeof(body));
    char id[12] = {0}, type[16] = {0}, key[33] = {0}, name[32] = {0};
    char *p;
    if ((p = strstr(body, "\"id\":\"")))   sscanf(p, "\"id\":\"%11[^\"]\"",   id);
    if ((p = strstr(body, "\"type\":\""))) sscanf(p, "\"type\":\"%15[^\"]\"", type);
    if ((p = strstr(body, "\"key\":\"")))  sscanf(p, "\"key\":\"%32[^\"]\"",  key);
    if ((p = strstr(body, "\"name\":\""))) sscanf(p, "\"name\":\"%31[^\"]\"", name);
    if (strlen(id) == 0) { resp_err(req, "brak id"); return ESP_OK; }

    sih_config_t cfg = nvs_config_get();
    int idx = -1;
    for (int i = 0; i < cfg.meter_count; i++)
        if (strcasecmp(cfg.meters[i].id_hex, id) == 0) { idx = i; break; }

    // Usuwanie licznika: body zawiera "del":1
    if (strstr(body, "\"del\"")) {
        if (idx >= 0) {
            for (int i = idx; i < cfg.meter_count - 1; i++)
                cfg.meters[i] = cfg.meters[i + 1];
            cfg.meter_count--;
            memset(&cfg.meters[cfg.meter_count], 0, sizeof(meter_config_t));
            nvs_config_save(&cfg);
        }
        resp_ok(req);
        return ESP_OK;
    }
    if (idx < 0) {
        if (cfg.meter_count >= MAX_METERS) { resp_err(req, "limit licznikow"); return ESP_OK; }
        idx = cfg.meter_count++;
        memset(&cfg.meters[idx], 0, sizeof(meter_config_t));
        strlcpy(cfg.meters[idx].id_hex, id, sizeof(cfg.meters[idx].id_hex));
    }
    if (strlen(type)) strlcpy(cfg.meters[idx].type, type, sizeof(cfg.meters[idx].type));
    strlcpy(cfg.meters[idx].key, key, sizeof(cfg.meters[idx].key)); // pusty = brak klucza
    if (strlen(name))
        strlcpy(cfg.meters[idx].name, name, sizeof(cfg.meters[idx].name));
    else if (strlen(cfg.meters[idx].name) == 0)
        strlcpy(cfg.meters[idx].name, id, sizeof(cfg.meters[idx].name));
    cfg.meters[idx].enabled = true;
    nvs_config_save(&cfg);
    resp_ok(req);
    return ESP_OK;
}

static esp_err_t handle_config_get(httpd_req_t *req) {
    sih_config_t cfg = nvs_config_get();
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{\"wifi_ssid\":\"%s\",\"ap_ssid\":\"%s\","
        "\"freq_mhz\":%.3f,\"meters\":[",
        cfg.wifi_ssid, cfg.ap_ssid, cfg.freq_mhz
    );
    for (int i = 0; i < cfg.meter_count; i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"id\":\"%s\",\"type\":\"%s\",\"name\":\"%s\",\"key\":\"%s\",\"enabled\":%s}",
            i > 0 ? "," : "",
            cfg.meters[i].id_hex, cfg.meters[i].type,
            cfg.meters[i].name, cfg.meters[i].key,
            cfg.meters[i].enabled ? "true" : "false"
        );
    }
    snprintf(buf + n, sizeof(buf) - n, "]}");
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_config_wifi(httpd_req_t *req) {
    char body[256];
    read_body(req, body, sizeof(body));
    char ssid[64] = {0}, pass[64] = {0};
    char *p;
    if ((p = strstr(body, "\"ssid\":\"")))
        sscanf(p, "\"ssid\":\"%63[^\"]\"", ssid);
    if ((p = strstr(body, "\"password\":\"")))
        sscanf(p, "\"password\":\"%63[^\"]\"", pass);
    if (strlen(ssid) == 0) { resp_err(req, "brak ssid"); return ESP_OK; }
    resp_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_reconnect(ssid, pass);
    return ESP_OK;
}

static esp_err_t handle_ota_url(httpd_req_t *req) {
    char body[512];
    read_body(req, body, sizeof(body));
    char url[480] = {0};
    char *p;
    if ((p = strstr(body, "\"url\":\"")))
        sscanf(p, "\"url\":\"%479[^\"]\"", url);
    if (strlen(url) == 0) { resp_err(req, "brak url"); return ESP_OK; }
    resp_ok(req);
    ota_start_from_url(url);
    return ESP_OK;
}

static esp_err_t handle_ota_upload(httpd_req_t *req) {
    size_t len = req->content_len;
    if (len == 0 || len > 2 * 1024 * 1024) {
        resp_err(req, "nieprawidlowy rozmiar");
        return ESP_OK;
    }
    uint8_t *buf = malloc(len);
    if (!buf) { resp_err(req, "brak pamieci"); return ESP_OK; }
    int received = 0, ret;
    while (received < (int)len) {
        ret = httpd_req_recv(req, (char *)buf + received, len - received);
        if (ret <= 0) break;
        received += ret;
    }
    if (received != (int)len) {
        free(buf);
        resp_err(req, "blad odbioru");
        return ESP_OK;
    }
    resp_ok(req);
    ota_start_from_buffer(buf, len);
    return ESP_OK;
}

static esp_err_t handle_ota_github(httpd_req_t *req) {
    // ?channel=beta -> kanal beta; domyslnie (lub channel=stable) -> oficjalny
    bool beta = false;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "channel", val, sizeof(val)) == ESP_OK)
            beta = (strcmp(val, "beta") == 0);
    }
    resp_ok(req);
    ota_start_from_github(beta);
    return ESP_OK;
}

static esp_err_t handle_ota_status(httpd_req_t *req) {
    ota_status_t st = ota_get_status();
    const char *states[] = {"idle","downloading","writing","success","failed","uptodate"};
    char buf[200];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"progress\":%d,\"error\":\"%s\"}",
        states[st.state], st.progress_pct, st.error
    );
    resp_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_restart(httpd_req_t *req) {
    resp_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handle_factory_reset(httpd_req_t *req) {
    resp_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    nvs_config_reset();   // kasuje cala konfiguracje (liczniki, klucze, wifi, dashboard)
    esp_restart();
    return ESP_OK;
}

// ===== Kopia zapasowa =====
// Format pliku .sihbak (binarny):
//   [4B "SBAK"][1B wersja=1]
//   [4B len config][config: sih_config_t]
//   powtarzane dla kazdego pliku historii:
//     [1B len nazwy][nazwa][4B len danych][dane]
//   [1B 0x00 = koniec listy plikow]
#define BACKUP_MAGIC "SBAK"
#define BACKUP_VER   2

// helper: dopisz do bufora z kontrola pojemnosci
static int buf_append(char *buf, int *pos, int cap, const void *data, int len) {
    if (*pos + len > cap) return 0;
    memcpy(buf + *pos, data, len);
    *pos += len;
    return 1;
}

static esp_err_t handle_backup_get(httpd_req_t *req) {
    const int cap = 60000;   // historia + config; SPIFFS jest maly
    char *buf = malloc(cap);   // alokacja tylko na czas tworzenia kopii
    if (!buf) { resp_err(req, "brak pamieci na kopie"); return ESP_OK; }
    int pos = 0;
    // naglowek
    buf_append(buf, &pos, cap, BACKUP_MAGIC, 4);
    uint8_t ver = BACKUP_VER;
    buf_append(buf, &pos, cap, &ver, 1);
    // tylko definicje licznikow (meters[] + meter_count) - bez WiFi, diod, AP,
    // czestotliwosci, przypiec do dashboard i nazw. Kopia = liczniki + historia.
    sih_config_t cfg = nvs_config_get();
    uint8_t mc = cfg.meter_count;
    if (mc > MAX_METERS) mc = MAX_METERS;
    buf_append(buf, &pos, cap, &mc, 1);
    uint32_t mlen = (uint32_t)mc * sizeof(meter_config_t);
    buf_append(buf, &pos, cap, &mlen, 4);
    buf_append(buf, &pos, cap, cfg.meters, mlen);
    // pliki historii z /spiffs (h_*.bin oraz tracked)
    DIR *dir = opendir("/spiffs");
    if (dir) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            const char *nm = de->d_name;
            // tylko pliki historii i listy sledzonych
            if (strncmp(nm, "h_", 2) != 0 && strcmp(nm, "tracked.txt") != 0) continue;
            char path[96];
            snprintf(path, sizeof(path), "/spiffs/%.80s", nm);
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long fsz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (fsz <= 0 || pos + (int)strlen(nm) + 5 + (int)fsz > cap) { fclose(f); continue; }
            uint8_t nlen = (uint8_t)strlen(nm);
            buf_append(buf, &pos, cap, &nlen, 1);
            buf_append(buf, &pos, cap, nm, nlen);
            uint32_t dlen = (uint32_t)fsz;
            buf_append(buf, &pos, cap, &dlen, 4);
            int rd = fread(buf + pos, 1, fsz, f);
            pos += rd;
            fclose(f);
        }
        closedir(dir);
    }
    uint8_t endmark = 0;
    buf_append(buf, &pos, cap, &endmark, 1);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"sih-backup.sihbak\"");
    httpd_resp_send(req, buf, pos);
    ESP_LOGI(TAG, "Backup wyeksportowany: %d B", pos);
    free(buf);
    return ESP_OK;
}

static esp_err_t handle_backup_post(httpd_req_t *req) {
    const int cap = 60000;
    char *buf = malloc(cap);   // alokacja tylko na czas przywracania kopii
    if (!buf) { resp_err(req, "brak pamieci na kopie"); return ESP_OK; }
    int total = 0, ret;
    while ((ret = httpd_req_recv(req, buf + total, cap - total)) > 0) {
        total += ret;
        if (total >= cap) break;
    }
    if (total < 9 || memcmp(buf, BACKUP_MAGIC, 4) != 0) {
        resp_err(req, "zly plik kopii");
        free(buf);
        return ESP_OK;
    }
    int p = 4;
    uint8_t ver = (uint8_t)buf[p]; p += 1;
    if (ver != BACKUP_VER) { resp_err(req, "zla wersja kopii"); free(buf); return ESP_OK; }
    // tylko definicje licznikow - wczytaj do ISTNIEJACEJ konfiguracji, nie ruszajac
    // WiFi, diod, AP, czestotliwosci, przypiec ani nazw.
    uint8_t mc = (uint8_t)buf[p]; p += 1;
    if (mc > MAX_METERS) { resp_err(req, "za duzo licznikow w kopii"); free(buf); return ESP_OK; }
    uint32_t mlen; memcpy(&mlen, buf + p, 4); p += 4;
    if (mlen != (uint32_t)mc * sizeof(meter_config_t) || p + (int)mlen > total) {
        resp_err(req, "niezgodne dane licznikow");
        free(buf);
        return ESP_OK;
    }
    sih_config_t cfg = nvs_config_get();   // zachowaj biezaca konfiguracje
    memset(cfg.meters, 0, sizeof(cfg.meters));
    memcpy(cfg.meters, buf + p, mlen);     // podmien tylko liczniki
    cfg.meter_count = mc;
    p += mlen;
    nvs_config_save(&cfg);
    // pliki historii
    int restored = 0;
    while (p < total) {
        uint8_t nlen = (uint8_t)buf[p]; p += 1;
        if (nlen == 0) break;   // koniec
        if (p + nlen + 4 > total) break;
        char nm[64] = {0};
        if (nlen >= sizeof(nm)) break;
        memcpy(nm, buf + p, nlen); p += nlen;
        uint32_t dlen; memcpy(&dlen, buf + p, 4); p += 4;
        if (p + (int)dlen > total) break;
        char path[96];
        snprintf(path, sizeof(path), "/spiffs/%.80s", nm);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(buf + p, 1, dlen, f);
            fclose(f);
            restored++;
        }
        p += dlen;
    }
    ESP_LOGI(TAG, "Backup przywrocony: %d licznikow + %d plikow historii", mc, restored);
    free(buf);
    resp_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();   // restart wczyta przywrocona konfiguracje i historie
    return ESP_OK;
}


static esp_err_t handle_logs(httpd_req_t *req) {
    // Bufor alokowany dynamicznie tylko na czas zadania (zamiast 16KB na stale w RAM).
    char *logbuf = malloc(16384);
    if (!logbuf) { httpd_resp_send_500(req); return ESP_OK; }
    size_t n = log_buffer_dump(logbuf, 16384);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, logbuf, n);
    free(logbuf);
    return ESP_OK;
}

static esp_err_t handle_logs_clear(httpd_req_t *req) {
    log_buffer_clear();
    resp_ok(req);
    return ESP_OK;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req) {
    static char buf[1536];
    int n = wifi_manager_scan(buf, sizeof(buf));
    if (n < 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

void api_register_handlers(httpd_handle_t server) {
    system_temp_init();   // czujnik temperatury ESP32-C6
    const httpd_uri_t handlers[] = {
        { .uri="/api/status",      .method=HTTP_GET,  .handler=handle_status,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/led",         .method=HTTP_GET,  .handler=handle_led,         .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/led",         .method=HTTP_POST, .handler=handle_led,         .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/led-status",  .method=HTTP_GET,  .handler=handle_led_status,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/led-status",  .method=HTTP_POST, .handler=handle_led_status,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/dashboard",   .method=HTTP_GET,  .handler=handle_dashboard,   .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/dashboard",   .method=HTTP_POST, .handler=handle_dashboard,   .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/meter/names", .method=HTTP_GET,  .handler=handle_meter_name,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/meter/name",  .method=HTTP_POST, .handler=handle_meter_name,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/system",      .method=HTTP_GET,  .handler=handle_system,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history/list", .method=HTTP_GET,  .handler=handle_history_list, .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history",      .method=HTTP_GET,  .handler=handle_history,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history/day",  .method=HTTP_GET,  .handler=handle_history_day,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history/track",.method=HTTP_POST, .handler=handle_history_track,.user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history/tracked",.method=HTTP_GET, .handler=handle_history_tracked,.user_ctx=NULL, .is_websocket=false },
        { .uri="/api/meters",      .method=HTTP_GET,  .handler=handle_meters,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/frames",      .method=HTTP_GET,  .handler=handle_frames,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/config",      .method=HTTP_GET,  .handler=handle_config_get,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/config/meter",.method=HTTP_POST, .handler=handle_config_meter,.user_ctx=NULL, .is_websocket=false },
        { .uri="/api/config/wifi", .method=HTTP_POST, .handler=handle_config_wifi, .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/wifi/scan",   .method=HTTP_GET,  .handler=handle_wifi_scan,   .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/ota/url",     .method=HTTP_POST, .handler=handle_ota_url,     .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/ota/github",  .method=HTTP_POST, .handler=handle_ota_github,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/ota/upload",  .method=HTTP_POST, .handler=handle_ota_upload,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/ota/status",  .method=HTTP_GET,  .handler=handle_ota_status,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/restart",     .method=HTTP_POST, .handler=handle_restart,     .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/factory-reset",.method=HTTP_POST,.handler=handle_factory_reset,.user_ctx=NULL, .is_websocket=false },
        { .uri="/api/backup",      .method=HTTP_GET,  .handler=handle_backup_get,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/backup",      .method=HTTP_POST, .handler=handle_backup_post, .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/logs",        .method=HTTP_GET,  .handler=handle_logs,        .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/logs/clear",  .method=HTTP_POST, .handler=handle_logs_clear,  .user_ctx=NULL, .is_websocket=false },
    };
    for (int i = 0; i < (int)(sizeof(handlers)/sizeof(handlers[0])); i++)
        httpd_register_uri_handler(server, &handlers[i]);
    ESP_LOGI(TAG, "%d endpointow API zarejestrowanych", (int)(sizeof(handlers)/sizeof(handlers[0])));
}
