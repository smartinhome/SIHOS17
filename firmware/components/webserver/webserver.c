#include "webserver.h"
#include "api_handlers.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "WEBSERVER";
static httpd_handle_t s_server = NULL;

// Panel WWW (webui/index.html) jest WBUDOWANY w firmware jako spakowany gzipem
// blob (~36 KB, vs ~126 KB rozpakowanego). Wczesniej caly HTML/JS siedzial tu
// jako gigantyczny string literal ROOT_HTML - kazda zmiana panelu wymagala
// ekranowania cudzyslowow w kodzie C (\") i lamania linii na "\n", edytor nie
// podswietlal skladni HTML, nie bylo browser preview.
//
// Teraz: zrodlem jest zwykly plik webui/index.html (edytowalny w VS Code),
// gzip webui/index.html.gz jest w repo (regenerowany skryptem po edycji),
// CMake (patrz webserver/CMakeLists.txt) kopiuje ten .gz do build/ przez
// configure_file(COPYONLY), a EMBED_FILES tworzy z niego symbole binarne:
//   _binary_index_html_gz_start / _binary_index_html_gz_end
// Serwer HTTP wysyla surowe bajty gz z naglowkiem Content-Encoding: gzip -
// przegladarka sama rozpakowuje (natywne wsparcie od HTTP/1.1).
//
// Regeneracja index.html.gz po edycji webui/index.html:
//   python -c "import gzip; open('webui/index.html.gz','wb').write(gzip.compress(open('webui/index.html','rb').read(), 9))"
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    // Cache-Control: no-cache pozwala przegladarce trzymac plik w cache,
    // ale zawsze pyta serwer If-None-Match przed uzyciem. Przy zmianie
    // firmware panel odswiezy sie sam bez Ctrl+F5.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    const size_t len = index_html_gz_end - index_html_gz_start;
    return httpd_resp_send(req, (const char *)index_html_gz_start, len);
}

static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void webserver_init(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 48;   // 39 w uzyciu po dodaniu MQTT - zapas na kolejne
    // 12 kB: handlery robia lokalna KOPIE konfiguracji (ponad 3 kB po
    // rozszerzeniu przypiec do 32), bo modyfikuja ja przed zapisem.
    cfg.stack_size       = 12288;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;   // limit gniazd httpd; reszta z puli LWIP wolna dla OTA
    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));
    httpd_uri_t root = {
        .uri          = "/",
        .method       = HTTP_GET,
        .handler      = root_handler,
        .user_ctx     = NULL,
        .is_websocket = false,
    };
    httpd_register_uri_handler(s_server, &root);
    httpd_uri_t favicon = {
        .uri = "/favicon.ico", .method = HTTP_GET,
        .handler = favicon_handler, .user_ctx = NULL, .is_websocket = false,
    };
    httpd_register_uri_handler(s_server, &favicon);
    api_register_handlers(s_server);
    ESP_LOGI(TAG, "Serwer HTTP uruchomiony na porcie 80 (panel wbudowany, %u B gz)",
             (unsigned)(index_html_gz_end - index_html_gz_start));
}


