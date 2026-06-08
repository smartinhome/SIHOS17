#include "api_handlers.h"
#include "wmbus_decoder.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "ota_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "API";

// ── Helpers ────────────────────────────────────────────────

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
    while ((ret = httpd_req_recv(req, buf + total,
                                 max - total - 1)) > 0)
        total += ret;
    buf[total] = 0;
    return total;
}

// ── GET /api/status ────────────────────────────────────────
static esp_err_t handle_status(httpd_req_t *req) {
    char ip[16];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_state_t ws = wifi_manager_get_state();
    const char *wifi_str[] = {"disconnected","connecting","connected","ap_mode"};

    char buf[512];
    snprintf(buf, sizeof(buf),
        "{"
        "\"version\":\"%s\","
        "\"uptime_ms\":%lld,"
        "\"wifi_state\":\"%s\","
        "\"wifi_rssi\":%d,"
        "\"ip\":\"%s\","
        "\"meter_count\":%d"
        "}",
        ota_get_running_version(),
        esp_timer_get_time() / 1000,
        wifi_str[ws],
        wifi_manager_get_rssi(),
        ip,
        wmbus_decoder_get_count()
    );
    resp_json(req, buf);
    return ESP_OK;
}

// ── GET /api/meters ────────────────────────────────────────
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
            "{"
            "\"id\":\"%s\","
            "\"type\":\"%s\","
            "\"name\":\"%s\","
            "\"rssi\":%d,"
            "\"last_seen\":%u,"
            "\"fields\":[",
            m->id_hex, m->type, m->name,
            m->rssi, m->last_seen
        );

        for (int j = 0; j < m->field_count; j++) {
            n += snprintf(mbuf + n, sizeof(mbuf) - n,
                "%s{\"field\":\"%s\",\"value\":%.3f,\"unit\":\"%s\"}",
                j > 0 ? "," : "",
                m->fields[j].field,
                m->fields[j].value,
                m->fields[j].unit
            );
        }
        n += snprintf(mbuf + n, sizeof(mbuf) - n, "]}");
        if (i > 0) httpd_resp_sendstr_chunk(req, ",");
        httpd_resp_sendstr_chunk(req, mbuf);
    }

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// ── GET /api/config ────────────────────────────────────────
static esp_err_t handle_config_get(httpd_req_t *req) {
    sih_config_t cfg = nvs_config_get();
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{"
        "\"wifi_ssid\":\"%s\","
        "\"ap_ssid\":\"%s\","
        "\"freq_mhz\":%.3f,"
        "\"meters\":[",
        cfg.wifi_ssid, cfg.ap_ssid, cfg.freq_mhz
    );
    for (int i = 0; i < cfg.meter_count; i++) {
        n += snprintf(buf + n, sizeof(buf) - n,
            "%s{\"id\":\"%s\",\"type\":\"%s\","
            "\"name\":\"%s\",\"enabled\":%s}",
            i > 0 ? "," : "",
            cfg.meters[i].id_hex,
            cfg.meters[i].type,
            cfg.meters[i].name,
            cfg.meters[i].enabled ? "true" : "false"
        );
    }
    snprintf(buf + n, sizeof(buf) - n, "]}");
    resp_json(req, buf);
    return ESP_OK;
}

// ── POST /api/config/wifi ──────────────────────────────────
static esp_err_t handle_config_wifi(httpd_req_t *req) {
    char body[256];
    read_body(req, body, sizeof(body));
    // Prosta ekstrakcja SSID/hasła z JSON
    // W produkcji użyć cJSON
    char ssid[64] = {0}, pass[64] = {0};
    // Szukaj "ssid":"..." i "password":"..."
    char *p;
    if ((p = strstr(body, "\"ssid\":\""))) {
        sscanf(p, "\"ssid\":\"%63[^\"]\"", ssid);
    }
    if ((p = strstr(body, "\"password\":\""))) {
        sscanf(p, "\"password\":\"%63[^\"]\"", pass);
    }
    if (strlen(ssid) == 0) {
        resp_err(req, "brak ssid");
        return ESP_OK;
    }
    resp_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_manager_reconnect(ssid, pass);
    return ESP_OK;
}

// ── POST /api/ota/url ──────────────────────────────────────
static esp_err_t handle_ota_url(httpd_req_t *req) {
    char body[512];
    read_body(req, body, sizeof(body));
    char url[480] = {0};
    char *p;
    if ((p = strstr(body, "\"url\":\""))) {
        sscanf(p, "\"url\":\"%479[^\"]\"", url);
    }
    if (strlen(url) == 0) {
        resp_err(req, "brak url");
        return ESP_OK;
    }
    resp_ok(req);
    ota_start_from_url(url);
    return ESP_OK;
}

// ── POST /api/ota/upload ───────────────────────────────────
// Multipart upload pliku .bin
static esp_err_t handle_ota_upload(httpd_req_t *req) {
    size_t len = req->content_len;
    if (len == 0 || len > 2 * 1024 * 1024) {
        resp_err(req, "nieprawidłowy rozmiar");
        return ESP_OK;
    }
    uint8_t *buf = malloc(len);
    if (!buf) {
        resp_err(req, "brak pamięci");
        return ESP_OK;
    }
    int received = 0;
    int ret;
    while (received < (int)len) {
        ret = httpd_req_recv(req, (char *)buf + received, len - received);
        if (ret <= 0) break;
        received += ret;
    }
    if (received != (int)len) {
        free(buf);
        resp_err(req, "błąd odbioru");
        return ESP_OK;
    }
    resp_ok(req);
    ota_start_from_buffer(buf, len);  // buf zwolni ota task
    return ESP_OK;
}

// ── GET /api/ota/status ────────────────────────────────────
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

// ── POST /api/restart ──────────────────────────────────────
static esp_err_t handle_restart(httpd_req_t *req) {
    resp_ok(req);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ── Rejestracja wszystkich handlerów ──────────────────────
void api_register_handlers(httpd_handle_t server) {
    const httpd_uri_t handlers[] = {
        { "/api/status",     HTTP_GET,  handle_status,      NULL },
        { "/api/meters",     HTTP_GET,  handle_meters,      NULL },
        { "/api/config",     HTTP_GET,  handle_config_get,  NULL },
        { "/api/config/wifi",HTTP_POST, handle_config_wifi, NULL },
        { "/api/ota/url",    HTTP_POST, handle_ota_url,     NULL },
        { "/api/ota/upload", HTTP_POST, handle_ota_upload,  NULL },
        { "/api/ota/status", HTTP_GET,  handle_ota_status,  NULL },
        { "/api/restart",    HTTP_POST, handle_restart,     NULL },
    };
    for (int i = 0; i < sizeof(handlers)/sizeof(handlers[0]); i++)
        httpd_register_uri_handler(server, &handlers[i]);

    ESP_LOGI(TAG, "%d endpointów API zarejestrowanych",
             (int)(sizeof(handlers)/sizeof(handlers[0])));
}
