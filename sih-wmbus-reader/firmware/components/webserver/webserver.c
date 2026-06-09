#include "webserver.h"
#include "api_handlers.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "WEBSERVER";
static httpd_handle_t s_server = NULL;

// Serwuj index.html z LittleFS (lub embedded fallback)
static esp_err_t root_handler(httpd_req_t *req) {
    // W produkcji: wczytaj z LittleFS /www/index.html
    // Na razie — minimalna strona redirect
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    const char *html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='0;url=/ui/'>"
        "</head><body>Ładowanie...</body></html>";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void webserver_init(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 24;
    cfg.stack_size        = 8192;
    cfg.lru_purge_enable  = true;
    cfg.uri_match_fn      = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    // Root
    httpd_uri_t root = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(s_server, &root);

    // API handlers
    api_register_handlers(s_server);

    ESP_LOGI(TAG, "Serwer HTTP uruchomiony na porcie 80");
}

void webserver_stop(void) {
    if (s_server) httpd_stop(s_server);
    s_server = NULL;
}
