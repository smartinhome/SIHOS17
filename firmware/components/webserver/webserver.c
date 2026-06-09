#include "webserver.h"
#include "api_handlers.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "WEBSERVER";
static httpd_handle_t s_server = NULL;

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    const char *html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>SIH wMbus Reader</title>"
        "<style>body{font-family:sans-serif;background:#0f1117;color:#e2e8f0;"
        "display:flex;align-items:center;justify-content:center;height:100vh;margin:0;}"
        ".box{text-align:center;padding:40px;background:#1a1d27;border-radius:12px;}"
        "h1{color:#4f8ef7;}a{color:#4f8ef7;}</style></head>"
        "<body><div class='box'>"
        "<h1>SIH wMbus Reader</h1>"
        "<p>Firmware dziala poprawnie.</p>"
        "<p><a href='/api/status'>Status JSON</a> &nbsp;|&nbsp; "
        "<a href='/api/meters'>Liczniki JSON</a></p>"
        "</div></body></html>";
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void webserver_init(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    httpd_uri_t root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_handler,
        .user_ctx = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(s_server, &root);

    api_register_handlers(s_server);

    ESP_LOGI(TAG, "Serwer HTTP uruchomiony na porcie 80");
}

void webserver_stop(void) {
    if (s_server) httpd_stop(s_server);
    s_server = NULL;
}
