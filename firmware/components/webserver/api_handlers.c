#include "api_handlers.h"
#include "wmbus_decoder.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "ota_manager.h"
#include "history.h"
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
    static char buf[8192];
    history_get_json(id, res, buf, sizeof(buf));
    resp_json(req, buf);
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
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"version\":\"%s\",\"uptime_ms\":%" PRId64 ","
        "\"wifi_state\":\"%s\",\"wifi_rssi\":%d,"
        "\"ip\":\"%s\",\"meter_count\":%d,\"partition\":\"%s\"}",
        ota_get_running_version(),
        esp_timer_get_time() / 1000,
        wifi_str[ws],
        wifi_manager_get_rssi(),
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
    resp_ok(req);
    ota_start_from_github();
    return ESP_OK;
}

static esp_err_t handle_ota_status(httpd_req_t *req) {
    ota_status_t st = ota_get_status();
    const char *states[] = {"idle","downloading","writing","success","failed"};
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

static esp_err_t handle_logs(httpd_req_t *req) {
    static char logbuf[16384];
    size_t n = log_buffer_dump(logbuf, sizeof(logbuf));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, logbuf, n);
    return ESP_OK;
}

static esp_err_t handle_logs_clear(httpd_req_t *req) {
    log_buffer_clear();
    resp_ok(req);
    return ESP_OK;
}

void api_register_handlers(httpd_handle_t server) {
    system_temp_init();   // czujnik temperatury ESP32-C6
    const httpd_uri_t handlers[] = {
        { .uri="/api/status",      .method=HTTP_GET,  .handler=handle_status,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/system",      .method=HTTP_GET,  .handler=handle_system,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history/list", .method=HTTP_GET,  .handler=handle_history_list, .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history",      .method=HTTP_GET,  .handler=handle_history,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history/track",.method=HTTP_POST, .handler=handle_history_track,.user_ctx=NULL, .is_websocket=false },
        { .uri="/api/history/tracked",.method=HTTP_GET, .handler=handle_history_tracked,.user_ctx=NULL, .is_websocket=false },
        { .uri="/api/meters",      .method=HTTP_GET,  .handler=handle_meters,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/frames",      .method=HTTP_GET,  .handler=handle_frames,      .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/config",      .method=HTTP_GET,  .handler=handle_config_get,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/config/meter",.method=HTTP_POST, .handler=handle_config_meter,.user_ctx=NULL, .is_websocket=false },
        { .uri="/api/config/wifi", .method=HTTP_POST, .handler=handle_config_wifi, .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/ota/url",     .method=HTTP_POST, .handler=handle_ota_url,     .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/ota/github",  .method=HTTP_POST, .handler=handle_ota_github,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/ota/upload",  .method=HTTP_POST, .handler=handle_ota_upload,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/ota/status",  .method=HTTP_GET,  .handler=handle_ota_status,  .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/restart",     .method=HTTP_POST, .handler=handle_restart,     .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/logs",        .method=HTTP_GET,  .handler=handle_logs,        .user_ctx=NULL, .is_websocket=false },
        { .uri="/api/logs/clear",  .method=HTTP_POST, .handler=handle_logs_clear,  .user_ctx=NULL, .is_websocket=false },
    };
    for (int i = 0; i < (int)(sizeof(handlers)/sizeof(handlers[0])); i++)
        httpd_register_uri_handler(server, &handlers[i]);
    ESP_LOGI(TAG, "%d endpointow API zarejestrowanych", (int)(sizeof(handlers)/sizeof(handlers[0])));
}
