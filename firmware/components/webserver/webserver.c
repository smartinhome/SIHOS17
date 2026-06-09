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
"body{font-family:system-ui,sans-serif;background:#0f1117;color:#e2e8f0;min-height:100vh}"
"nav{background:#1a1d27;border-bottom:1px solid #2e3352;padding:0 20px;"
"display:flex;align-items:center;height:56px;gap:8px}"
".logo{color:#4f8ef7;font-weight:700;font-size:1rem;margin-right:16px}"
".tab{padding:6px 14px;border-radius:8px;cursor:pointer;font-size:.85rem;"
"color:#64748b;border:none;background:transparent}"
".tab.active,.tab:hover{background:#22263a;color:#e2e8f0}"
".tab.active{color:#4f8ef7}"
".badge{margin-left:auto;padding:4px 10px;border-radius:20px;font-size:.75rem;font-weight:600}"
".badge.connected{background:#16301f;color:#22c55e}"
".badge.ap{background:#2d2408;color:#f59e0b}"
".badge.off{background:#2d0f0f;color:#ef4444}"
"main{padding:24px 20px;max-width:960px;margin:0 auto}"
".page{display:none}.page.active{display:block}"
"h2{font-size:1rem;font-weight:600;margin-bottom:16px}"
".card{background:#1a1d27;border:1px solid #2e3352;border-radius:12px;padding:20px;margin-bottom:16px}"
".card-title{font-size:.72rem;font-weight:600;color:#64748b;text-transform:uppercase;"
"letter-spacing:1px;margin-bottom:10px}"
".val{font-size:1.8rem;font-weight:700}"
".unit{font-size:.9rem;color:#64748b;font-weight:400}"
".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:12px}"
".meter{background:#1a1d27;border:1px solid #2e3352;border-radius:12px;padding:16px;margin-bottom:12px}"
".meter-hdr{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
".meter-name{font-weight:600}"
".meter-id{font-size:.72rem;color:#64748b;font-family:monospace}"
".type{padding:2px 8px;border-radius:20px;font-size:.7rem;font-weight:600}"
".elec{background:#1a2a4a;color:#60a5fa}.water{background:#0f2a2a;color:#34d399}"
".gas{background:#2a1f0f;color:#fbbf24}.unk{background:#22263a;color:#64748b}"
".fields{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:8px}"
".field{background:#22263a;border-radius:8px;padding:8px 10px}"
".fname{font-size:.68rem;color:#64748b;margin-bottom:3px}"
".fval{font-size:1rem;font-weight:600}"
"label{display:block;font-size:.82rem;color:#94a3b8;margin-bottom:5px;margin-top:12px}"
"input,select{width:100%;padding:10px 12px;background:#22263a;border:1px solid #2e3352;"
"border-radius:8px;color:#e2e8f0;font-size:.9rem;outline:none}"
"input:focus{border-color:#4f8ef7}"
".btn{padding:10px 20px;border:none;border-radius:8px;font-size:.875rem;"
"font-weight:600;cursor:pointer;margin-top:12px}"
".btn-primary{background:#4f8ef7;color:#fff}.btn-primary:hover{background:#3b7de8}"
".btn-danger{background:#ef4444;color:#fff}"
".btn-github{background:#238636;color:#fff;width:100%;display:block;text-align:center}"
".btn-github:hover{background:#2ea043}"
".msg{margin-top:12px;padding:10px 14px;border-radius:8px;font-size:.85rem;display:none}"
".msg.ok{background:#16301f;color:#22c55e;display:block}"
".msg.err{background:#2d0f0f;color:#ef4444;display:block}"
".progress-wrap{background:#22263a;border-radius:8px;height:10px;margin:10px 0;overflow:hidden;display:none}"
".progress-bar{height:100%;background:#4f8ef7;border-radius:8px;width:0%;transition:width .3s}"
"</style></head><body>"
"<nav>"
"<span class='logo'>SIH wMbus</span>"
"<button class='tab active' onclick='showPage(\"dashboard\",this)'>Dashboard</button>"
"<button class='tab' onclick='showPage(\"wifi\",this)'>WiFi</button>"
"<button class='tab' onclick='showPage(\"update\",this);fetchStatus()'>Aktualizacja</button>"
"<span id='wifi-badge' class='badge ap'>AP</span>"
"</nav>"
"<main>"

"<div id='page-dashboard' class='page active'>"
"<h2>Odczyty licznikow</h2>"
"<div class='grid' id='stat-grid'>"
"<div class='card'><div class='card-title'>Liczniki</div>"
"<div class='val' id='s-meters'>-</div></div>"
"<div class='card'><div class='card-title'>WiFi RSSI</div>"
"<div class='val' id='s-rssi'>-<span class='unit'> dBm</span></div></div>"
"<div class='card'><div class='card-title'>Uptime</div>"
"<div class='val' id='s-uptime' style='font-size:1.2rem'>-</div></div>"
"</div>"
"<div id='meters-list'><div style='color:#64748b;text-align:center;padding:40px'>"
"Oczekiwanie na ramki wMbus...</div></div>"
"</div>"

"<div id='page-wifi' class='page'>"
"<div class='card'>"
"<div class='card-title'>Polaczenie WiFi</div>"
"<label>Nazwa sieci (SSID)</label>"
"<input type='text' id='ssid' placeholder='Nazwa sieci'>"
"<label>Haslo</label>"
"<input type='password' id='pass' placeholder='Haslo WiFi'>"
"<button class='btn btn-primary' onclick='saveWifi()'>Zapisz i polacz</button>"
"<div id='wifi-msg' class='msg'></div>"
"</div>"
"</div>"

"<div id='page-update' class='page'>"
"<div class='card'>"
"<div class='card-title'>Aktualna wersja</div>"
"<div style='font-size:1.4rem;font-weight:700;margin:8px 0' id='fw-ver'>-</div>"
"<div style='color:#64748b;font-size:.85rem'>ESP32-C6 · SIH wMbus Reader</div>"
"</div>"
"<div class='card'>"
"<div class='card-title'>Aktualizacja z GitHub</div>"
"<p style='font-size:.85rem;color:#94a3b8;margin-top:8px'>"
"Pobiera najnowszy firmware z GitHub Releases i wgrywa automatycznie.</p>"
"<button class='btn btn-github' onclick='otaGithub()'>Aktualizuj z GitHub (latest)</button>"
"<div id='ota-msg' class='msg'></div>"
"<div class='progress-wrap' id='ota-progress'>"
"<div class='progress-bar' id='ota-bar'></div></div>"
"</div>"
"</div>"

"</main>"
"<script>"
"const GITHUB_BIN='https://github.com/smartinhome/SIHOS17/releases/latest/download/sih-wmbus-reader.bin';"
"let otaPoller=null;"

"function showPage(name,btn){"
"document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));"
"document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));"
"document.getElementById('page-'+name).classList.add('active');"
"btn.classList.add('active');"
"if(name==='dashboard')fetchAll();"
"if(name==='update')fetchStatus();"
"}"

"async function fetchStatus(){"
"try{const r=await fetch('/api/status');const d=await r.json();"
"const b=document.getElementById('wifi-badge');"
"document.getElementById('s-meters').textContent=d.meter_count||0;"
"document.getElementById('s-rssi').innerHTML=d.wifi_rssi+'<span class=\"unit\"> dBm</span>';"
"document.getElementById('s-uptime').textContent=fmtUptime(d.uptime_ms);"
"document.getElementById('fw-ver').textContent=d.version||'-';"
"if(d.wifi_state==='connected'){b.className='badge connected';b.textContent=d.ip;}"
"else if(d.wifi_state==='ap_mode'){b.className='badge ap';b.textContent='Tryb AP';}"
"else{b.className='badge off';b.textContent='Brak WiFi';}"
"}catch(e){}}"

"async function fetchMeters(){"
"try{const r=await fetch('/api/meters');const meters=await r.json();"
"const el=document.getElementById('meters-list');"
"if(!meters.length){el.innerHTML='<div style=\"color:#64748b;text-align:center;padding:40px\">Oczekiwanie na ramki wMbus...</div>';return;}"
"el.innerHTML=meters.map(m=>{"
"const tc=m.type.includes('elect')||m.type==='amiplus'?'elec':"
"m.type.includes('water')||m.type==='izar'||m.type==='apator162'?'water':"
"m.type==='unismart'||m.type.includes('gas')?'gas':'unk';"
"const tl={'amiplus':'Elektrycznosc','izar':'Woda','apator162':'Woda','unismart':'Gaz'}[m.type]||m.type;"
"const fields=m.fields.map(f=>"
"`<div class='field'><div class='fname'>${f.field.replace(/_/g,' ')}</div>"
"<div class='fval'>${f.value.toFixed(3)} <small style='color:#64748b'>${f.unit}</small></div></div>`"
").join('');"
"return `<div class='meter'><div class='meter-hdr'>"
"<div><div class='meter-name'>${m.name||m.id}</div>"
"<div class='meter-id'>${m.id}</div></div>"
"<span class='type ${tc}'>${tl}</span></div>"
"<div class='fields'>${fields||'<div style=\"color:#64748b;font-size:.85rem\">Brak pol</div>'}</div>"
"<div style='font-size:.72rem;color:#64748b;margin-top:8px'>RSSI: ${m.rssi} dBm</div>"
"</div>`;"
"}).join('');"
"}catch(e){}}"

"function fetchAll(){fetchStatus();fetchMeters();}"

"async function saveWifi(){"
"const ssid=document.getElementById('ssid').value.trim();"
"const pass=document.getElementById('pass').value;"
"const msg=document.getElementById('wifi-msg');"
"if(!ssid){msg.className='msg err';msg.textContent='Podaj nazwe sieci!';return;}"
"msg.className='msg ok';msg.textContent='Wysylanie...';"
"try{await fetch('/api/config/wifi',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({ssid,password:pass})});}"
"catch(e){}"
"msg.textContent='Zapisano — urzadzenie restartuje sie i laczy z '+ssid;}"

"async function otaGithub(){"
"const msg=document.getElementById('ota-msg');"
"const prog=document.getElementById('ota-progress');"
"if(!confirm('Pobrac i wgrac najnowszy firmware z GitHub?'))return;"
"msg.className='msg ok';msg.textContent='Wysylanie zadania OTA...';"
"prog.style.display='block';"
"try{"
"await fetch('/api/ota/url',{method:'POST',"
"headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({url:GITHUB_BIN})});"
"if(otaPoller)clearInterval(otaPoller);"
"otaPoller=setInterval(pollOta,1500);"
"}catch(e){msg.className='msg err';msg.textContent='Blad: '+e;}}"

"async function pollOta(){"
"try{const r=await fetch('/api/ota/status');const d=await r.json();"
"const msg=document.getElementById('ota-msg');"
"const bar=document.getElementById('ota-bar');"
"bar.style.width=d.progress+'%';"
"const labels={idle:'',downloading:'Pobieranie...',writing:'Zapisywanie...',"
"success:'Aktualizacja OK — restart...',failed:'Blad: '+d.error};"
"if(d.state!=='idle')msg.textContent=labels[d.state]||d.state;"
"if(d.state==='success'||d.state==='failed'){"
"clearInterval(otaPoller);otaPoller=null;}}"
"catch(e){}}"

"fetchAll();"
"setInterval(fetchAll,5000);"
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
