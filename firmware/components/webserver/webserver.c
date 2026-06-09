#include "webserver.h"
#include "api_handlers.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "WEBSERVER";
static httpd_handle_t s_server = NULL;

static const char *ROOT_HTML =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>SIH wMbus Reader</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;"
"min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
".box{background:#1a1d27;border:1px solid #2e3352;border-radius:12px;"
"padding:32px;width:100%;max-width:420px}"
"h1{color:#4f8ef7;font-size:1.3rem;margin-bottom:6px}"
".sub{color:#64748b;font-size:.85rem;margin-bottom:24px}"
".section{margin-bottom:24px}"
"h2{font-size:.75rem;font-weight:600;color:#64748b;text-transform:uppercase;"
"letter-spacing:1px;margin-bottom:12px}"
"label{display:block;font-size:.82rem;color:#94a3b8;margin-bottom:5px}"
"input{width:100%;padding:10px 12px;background:#22263a;border:1px solid #2e3352;"
"border-radius:8px;color:#e2e8f0;font-size:.9rem;outline:none;margin-bottom:12px}"
"input:focus{border-color:#4f8ef7}"
".btn{width:100%;padding:11px;border:none;border-radius:8px;font-size:.9rem;"
"font-weight:600;cursor:pointer;transition:background .15s}"
".btn-primary{background:#4f8ef7;color:#fff}"
".btn-primary:hover{background:#3b7de8}"
".status{margin-top:14px;padding:10px 14px;border-radius:8px;font-size:.85rem;display:none}"
".ok{background:#16301f;color:#22c55e;display:block}"
".err{background:#2d0f0f;color:#ef4444;display:block}"
".divider{border:none;border-top:1px solid #2e3352;margin:20px 0}"
".links{display:flex;gap:12px;flex-wrap:wrap}"
".links a{color:#4f8ef7;font-size:.85rem;text-decoration:none}"
".links a:hover{text-decoration:underline}"
".badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:.75rem;"
"font-weight:600;background:#1a2a4a;color:#60a5fa;margin-bottom:16px}"
"</style></head><body><div class='box'>"
"<h1>SIH wMbus Reader</h1>"
"<div class='sub'>smartinhome.pl</div>"
"<div id='badge' class='badge'>Ładowanie...</div>"

"<div class='section'>"
"<h2>Konfiguracja WiFi</h2>"
"<label>Nazwa sieci (SSID)</label>"
"<input type='text' id='ssid' placeholder='Nazwa sieci WiFi'>"
"<label>Hasło</label>"
"<input type='password' id='pass' placeholder='Hasło WiFi'>"
"<button class='btn btn-primary' onclick='saveWifi()'>Połącz z siecią</button>"
"<div id='msg' class='status'></div>"
"</div>"

"<hr class='divider'>"
"<div class='section'>"
"<h2>Diagnostyka</h2>"
"<div class='links'>"
"<a href='/api/status'>Status JSON</a>"
"<a href='/api/meters'>Liczniki JSON</a>"
"<a href='/api/config'>Konfiguracja JSON</a>"
"</div>"
"</div>"

"</div>"
"<script>"
"async function loadStatus(){"
"try{"
"const r=await fetch('/api/status');"
"const d=await r.json();"
"const b=document.getElementById('badge');"
"if(d.wifi_state==='connected'){"
"b.textContent='WiFi: '+d.ip;"
"b.style.background='#16301f';b.style.color='#22c55e';"
"}else if(d.wifi_state==='ap_mode'){"
"b.textContent='Tryb AP — brak WiFi';"
"b.style.background='#2d2408';b.style.color='#f59e0b';"
"}else{"
"b.textContent='Łączenie...';"
"}"
"}catch(e){document.getElementById('badge').textContent='Offline';}"
"}"

"async function saveWifi(){"
"const ssid=document.getElementById('ssid').value.trim();"
"const pass=document.getElementById('pass').value;"
"const msg=document.getElementById('msg');"
"if(!ssid){msg.className='status err';msg.textContent='Podaj nazwę sieci!';return;}"
"msg.className='status ok';msg.textContent='Łączenie — urządzenie uruchomi się ponownie...';"
"try{"
"await fetch('/api/config/wifi',{"
"method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid:ssid,password:pass})"
"});"
"}catch(e){}"
"msg.textContent='Wysłano — urządzenie restartuje się i łączy z siecią '+ssid;"
"}"

"loadStatus();"
"setInterval(loadStatus,5000);"
"</script></body></html>";

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, ROOT_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

void webserver_init(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &cfg));

    httpd_uri_t root = {
        .uri          = "/",
        .method       = HTTP_GET,
        .handler      = root_handler,
        .user_ctx     = NULL,
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
