#include "webserver.h"
#include "api_handlers.h"
#include "esp_http_server.h"
#include "esp_log.h"

static const char *TAG = "WEBSERVER";
static httpd_handle_t s_server = NULL;

static const char *ROOT_HTML =
"<!DOCTYPE html><html lang=\"pl\"><head><meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>SIH wMbus Reader</title>"
"<style>"
":root{--bg:#0b0e14;--panel:#141925;--panel2:#1b2230;--line:#28324a;--txt:#e6edf7;--mut:#7886a0;--accent:#3d8bff;--accent2:#00d3a7;--warn:#f0b429;--err:#ff5c5c;--ok:#22c55e;--r:14px;--sidebar:230px}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:var(--bg);color:var(--txt);font-family:'Segoe UI',system-ui,sans-serif;min-height:100vh;display:flex}"
".sidebar{width:var(--sidebar);background:var(--panel);border-right:1px solid var(--line);display:flex;flex-direction:column;position:fixed;top:0;bottom:0;left:0;z-index:50;transition:transform .25s}"
".brand{padding:22px 20px;border-bottom:1px solid var(--line);display:flex;align-items:center;gap:12px}"
".brand-logo{width:38px;height:38px;border-radius:10px;background:linear-gradient(135deg,var(--accent),var(--accent2));display:flex;align-items:center;justify-content:center;flex-shrink:0}"
".brand-logo svg{width:22px;height:22px}"
".brand-txt b{font-size:.98rem;font-weight:700;letter-spacing:.3px;display:block}"
".brand-txt span{font-size:.7rem;color:var(--mut)}"
".nav{flex:1;padding:14px 12px;display:flex;flex-direction:column;gap:4px}"
".nav-item{display:flex;align-items:center;gap:13px;padding:11px 14px;border-radius:10px;color:var(--mut);cursor:pointer;font-size:.9rem;font-weight:500;border:none;background:none;width:100%;text-align:left;transition:all .15s}"
".nav-item svg{width:19px;height:19px;flex-shrink:0}"
".nav-item:hover{background:var(--panel2);color:var(--txt)}"
".nav-item.active{background:linear-gradient(135deg,rgba(61,139,255,.16),rgba(0,211,167,.10));color:var(--accent);font-weight:600}"
".sidebar-foot{padding:14px 20px;border-top:1px solid var(--line);font-size:.7rem;color:var(--mut)}"
".sidebar-foot b{color:var(--accent2)}"
".main{flex:1;margin-left:var(--sidebar);min-height:100vh}"
".topbar{height:62px;border-bottom:1px solid var(--line);display:flex;align-items:center;padding:0 26px;gap:16px;position:sticky;top:0;background:rgba(11,14,20,.85);backdrop-filter:blur(12px);z-index:40}"
".hamburger{display:none;background:none;border:none;color:var(--txt);cursor:pointer}"
".page-title{font-size:1.05rem;font-weight:600}"
".topbar-status{margin-left:auto;display:flex;align-items:center;gap:10px;font-size:.78rem;color:var(--mut)}"
".chip{display:inline-flex;align-items:center;gap:6px;padding:5px 12px;border-radius:20px;font-size:.74rem;font-weight:600;background:var(--panel2);border:1px solid var(--line)}"
".chip .dot{width:7px;height:7px;border-radius:50%}"
".chip.ok{color:var(--ok)}.chip.ok .dot{background:var(--ok);box-shadow:0 0 8px var(--ok)}"
".chip.warn{color:var(--warn)}.chip.warn .dot{background:var(--warn)}"
".chip.err{color:var(--err)}.chip.err .dot{background:var(--err)}"
".content{padding:26px}"
".page{display:none}.page.active{display:block;animation:fade .3s}"
"@keyframes fade{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}"
".section{margin-bottom:28px}"
".section-head{display:flex;align-items:center;gap:10px;margin-bottom:14px}"
".section-head .ic{width:30px;height:30px;border-radius:8px;background:var(--panel2);display:flex;align-items:center;justify-content:center;color:var(--accent)}"
".section-head .ic svg{width:17px;height:17px}"
".section-head h3{font-size:.82rem;font-weight:700;text-transform:uppercase;letter-spacing:1.2px;color:var(--mut)}"
".cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:14px}"
".card{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);padding:18px 20px}"
".card .lbl{font-size:.72rem;color:var(--mut);text-transform:uppercase;letter-spacing:.6px;margin-bottom:8px}"
".card .v{font-size:1.7rem;font-weight:700;line-height:1.1}"
".card .v small{font-size:.85rem;color:var(--mut);font-weight:400}"
".card .sub{font-size:.75rem;color:var(--mut);margin-top:5px}"
".card.accent{background:linear-gradient(135deg,rgba(61,139,255,.12),rgba(0,211,167,.06));border-color:rgba(61,139,255,.3)}"
".meter{background:var(--panel);border:1px solid var(--line);border-radius:var(--r);padding:18px;margin-bottom:14px}"
".meter-top{display:flex;justify-content:space-between;align-items:flex-start;margin-bottom:14px}"
".meter-name{font-size:1.02rem;font-weight:600}"
".meter-id{font-family:'Consolas',monospace;font-size:.74rem;color:var(--mut);margin-top:2px}"
".tag{padding:4px 11px;border-radius:20px;font-size:.7rem;font-weight:700;text-transform:uppercase;letter-spacing:.5px}"
".tag.elec{background:rgba(61,139,255,.15);color:#6aa8ff}"
".tag.water{background:rgba(0,211,167,.15);color:#2ee0bb}"
".tag.gas{background:rgba(240,180,41,.15);color:#f5c451}"
".tag.unk{background:var(--panel2);color:var(--mut)}"
".meter-fields{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:10px}"
".mf{background:var(--panel2);border-radius:9px;padding:10px 12px}"
".mf .k{font-size:.68rem;color:var(--mut);margin-bottom:3px}"
".mf .val{font-size:1.05rem;font-weight:600}"
".signal{margin-top:12px;height:5px;background:var(--panel2);border-radius:3px;overflow:hidden}"
".signal i{display:block;height:100%;background:linear-gradient(90deg,var(--err),var(--warn),var(--ok))}"
".empty{text-align:center;padding:48px;color:var(--mut)}"
"label{display:block;font-size:.8rem;color:var(--mut);margin:14px 0 6px}"
"input,select{width:100%;padding:11px 14px;background:var(--panel2);border:1px solid var(--line);border-radius:9px;color:var(--txt);font-size:.9rem;outline:none;transition:border .15s}"
"input:focus,select:focus{border-color:var(--accent)}"
".btn{padding:11px 22px;border:none;border-radius:9px;font-size:.88rem;font-weight:600;cursor:pointer;margin-top:16px;transition:all .15s}"
".btn-primary{background:var(--accent);color:#fff}.btn-primary:hover{background:#2f7bef}"
".btn-github{background:#238636;color:#fff;width:100%}.btn-github:hover{background:#2ea043}"
".btn-ghost{background:var(--panel2);color:var(--txt);border:1px solid var(--line)}"
".msg{margin-top:14px;padding:11px 15px;border-radius:9px;font-size:.85rem;display:none}"
".msg.ok{background:rgba(34,197,94,.12);color:var(--ok);display:block}"
".msg.err{background:rgba(255,92,92,.12);color:var(--err);display:block}"
".pbar{background:var(--panel2);border-radius:8px;height:10px;margin:12px 0;overflow:hidden;display:none}"
".pbar i{display:block;height:100%;background:linear-gradient(90deg,var(--accent),var(--accent2));width:0;transition:width .3s}"
"pre.logs{background:#070a0f;border:1px solid var(--line);border-radius:10px;padding:14px;font-size:.72rem;line-height:1.55;color:#9fb0cc;overflow:auto;max-height:64vh;white-space:pre-wrap;word-break:break-all;font-family:'Consolas',monospace;margin:0}"
".row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}"
".overlay{display:none}"
"@media(max-width:820px){.sidebar{transform:translateX(-100%)}.sidebar.open{transform:none}.main{margin-left:0}.hamburger{display:block}.overlay.show{display:block;position:fixed;inset:0;background:rgba(0,0,0,.5);z-index:45}}"
"</style></head><body>"
"<aside class=\"sidebar\" id=\"sidebar\">"
"<div class=\"brand\"><div class=\"brand-logo\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#fff\" stroke-width=\"2\"><path d=\"M2 12h3l2-7 4 14 3-9 2 2h6\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/></svg></div><div class=\"brand-txt\"><b>SIH wMbus</b><span>Reader 868</span></div></div>"
"<nav class=\"nav\" id=\"nav\">"
"<button class=\"nav-item active\" data-page=\"dashboard\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><rect x=\"3\" y=\"3\" width=\"7\" height=\"9\" rx=\"1\"/><rect x=\"14\" y=\"3\" width=\"7\" height=\"5\" rx=\"1\"/><rect x=\"14\" y=\"12\" width=\"7\" height=\"9\" rx=\"1\"/><rect x=\"3\" y=\"16\" width=\"7\" height=\"5\" rx=\"1\"/></svg>Dashboard</button>"
"<button class=\"nav-item\" data-page=\"meters\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M12 12l4-3\" stroke-linecap=\"round\"/></svg>Liczniki</button>"
"<button class=\"nav-item\" data-page=\"wifi\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><path d=\"M5 12.5a10 10 0 0114 0M8.5 16a5 5 0 017 0\" stroke-linecap=\"round\"/><circle cx=\"12\" cy=\"19.5\" r=\"1\" fill=\"currentColor\"/></svg>WiFi</button>"
"<button class=\"nav-item\" data-page=\"update\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><path d=\"M12 3v12m0 0l-4-4m4 4l4-4\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/><path d=\"M4 17v2a2 2 0 002 2h12a2 2 0 002-2v-2\" stroke-linecap=\"round\"/></svg>Aktualizacja</button>"
"<button class=\"nav-item\" data-page=\"logs\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><path d=\"M4 6h16M4 12h16M4 18h10\" stroke-linecap=\"round\"/></svg>Logi</button>"
"</nav>"
"<div class=\"sidebar-foot\">v1.0.0 &middot; <b>smartinhome.pl</b></div>"
"</aside>"
"<div class=\"overlay\" id=\"overlay\" onclick=\"toggleSidebar()\"></div>"
"<div class=\"main\">"
"<div class=\"topbar\"><button class=\"hamburger\" onclick=\"toggleSidebar()\"><svg width=\"24\" height=\"24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><path d=\"M3 6h18M3 12h18M3 18h18\" stroke-linecap=\"round\"/></svg></button><span class=\"page-title\" id=\"ptitle\">Dashboard</span><div class=\"topbar-status\"><span class=\"chip ok\" id=\"wifiChip\"><span class=\"dot\"></span><span id=\"wifiChipTxt\">--</span></span></div></div>"
"<div class=\"content\">"
"<div class=\"page active\" id=\"page-dashboard\">"
"<div class=\"section\"><div class=\"section-head\"><div class=\"ic\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><rect x=\"4\" y=\"4\" width=\"16\" height=\"16\" rx=\"2\"/><path d=\"M9 9h6v6H9z\"/></svg></div><h3>Urzadzenie</h3></div>"
"<div class=\"cards\"><div class=\"card accent\"><div class=\"lbl\">Firmware</div><div class=\"v\" id=\"d-ver\" style=\"font-size:1.3rem\">--</div><div class=\"sub\">ESP32-C6</div></div><div class=\"card\"><div class=\"lbl\">Uptime</div><div class=\"v\" id=\"d-uptime\" style=\"font-size:1.3rem\">--</div><div class=\"sub\">od restartu</div></div><div class=\"card\"><div class=\"lbl\">Liczniki</div><div class=\"v\" id=\"d-meters\">--</div><div class=\"sub\">wykryte w eterze</div></div></div></div>"
"<div class=\"section\"><div class=\"section-head\"><div class=\"ic\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><path d=\"M5 12.5a10 10 0 0114 0\" stroke-linecap=\"round\"/><circle cx=\"12\" cy=\"19.5\" r=\"1\" fill=\"currentColor\"/></svg></div><h3>Siec</h3></div>"
"<div class=\"cards\"><div class=\"card\"><div class=\"lbl\">Adres IP</div><div class=\"v\" id=\"d-ip\" style=\"font-size:1.2rem\">--</div><div class=\"sub\">DHCP</div></div><div class=\"card\"><div class=\"lbl\">WiFi RSSI</div><div class=\"v\" id=\"d-rssi\">--<small> dBm</small></div><div class=\"sub\" id=\"d-wifistate\">--</div></div><div class=\"card\"><div class=\"lbl\">Hostname</div><div class=\"v\" style=\"font-size:1.2rem\">sih-wmbus</div><div class=\"sub\">.local</div></div></div></div>"
"<div class=\"section\"><div class=\"section-head\"><div class=\"ic\"><svg viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\"><circle cx=\"12\" cy=\"12\" r=\"9\"/><path d=\"M12 12l4-3\" stroke-linecap=\"round\"/></svg></div><h3>Liczniki - podglad</h3></div>"
"<div id=\"dash-meters\"><div class=\"empty\">Oczekiwanie na ramki wMbus...</div></div></div>"
"</div>"
"<div class=\"page\" id=\"page-meters\"><div id=\"meters-list\"><div class=\"empty\">Brak wykrytych licznikow</div></div></div>"
"<div class=\"page\" id=\"page-wifi\"><div class=\"card\" style=\"max-width:460px\"><div class=\"section-head\" style=\"margin-bottom:4px\"><h3>Polaczenie WiFi</h3></div><label>Nazwa sieci (SSID)</label><input id=\"ssid\" placeholder=\"Nazwa sieci\"><label>Haslo</label><input type=\"password\" id=\"pass\" placeholder=\"Haslo WiFi\"><button class=\"btn btn-primary\" onclick=\"saveWifi()\">Zapisz i polacz</button><div id=\"wifi-msg\" class=\"msg\"></div></div></div>"
"<div class=\"page\" id=\"page-update\"><div class=\"cards\" style=\"grid-template-columns:repeat(auto-fit,minmax(260px,1fr))\"><div class=\"card accent\"><div class=\"lbl\">Aktualna wersja</div><div class=\"v\" id=\"u-ver\" style=\"font-size:1.5rem\">--</div><div class=\"sub\">ESP32-C6 - SIH wMbus Reader</div></div></div>"
"<div class=\"card\" style=\"margin-top:14px\"><div class=\"lbl\">Aktualizacja z GitHub</div><p style=\"font-size:.85rem;color:var(--mut);margin:8px 0\">Pobiera najnowszy firmware z GitHub Releases i wgrywa bezprzewodowo.</p><button class=\"btn btn-github\" onclick=\"otaGithub()\">Aktualizuj z GitHub</button><div id=\"ota-msg\" class=\"msg\"></div><div class=\"pbar\" id=\"ota-pbar\"><i id=\"ota-bar\"></i></div></div></div>"
"<div class=\"page\" id=\"page-logs\"><div class=\"card\"><div class=\"row\" style=\"justify-content:space-between;margin-bottom:12px\"><div class=\"lbl\" style=\"margin:0\">Logi systemowe</div><div class=\"row\"><button class=\"btn btn-ghost\" style=\"margin:0;padding:7px 14px;font-size:.8rem\" onclick=\"toggleAutoLogs()\"><span id=\"log-auto\">Auto: ON</span></button><button class=\"btn btn-ghost\" style=\"margin:0;padding:7px 14px;font-size:.8rem\" onclick=\"clearLogs()\">Wyczysc</button></div></div><pre class=\"logs\" id=\"log-output\">Ladowanie...</pre></div></div>"
"</div></div>"
"<script>"
"let otaPoller=null,logTimer=null,logAuto=true;"
"const titles={dashboard:'Dashboard',meters:'Liczniki',wifi:'WiFi',update:'Aktualizacja',logs:'Logi'};"
"function rssiW(r){return Math.max(0,Math.min(100,((r+100)/60)*100))}"
"function tagClass(t){if(t.includes('elect')||t=='amiplus')return'elec';if(t.includes('water')||t=='izar'||t=='apator162')return'water';if(t=='unismart'||t.includes('gas'))return'gas';return'unk'}"
"function tagLabel(t){return({amiplus:'Prad',izar:'Woda',apator162:'Woda',unismart:'Gaz',electricity:'Prad',water:'Woda',gas:'Gaz'})[t]||t}"
"function fmtUp(ms){let s=ms/1000|0,d=s/86400|0;s%=86400;let h=s/3600|0;s%=3600;let m=s/60|0;return d>0?d+'d '+h+'h '+m+'m':h>0?h+'h '+m+'m':m+'m'}"
"function meterCard(m){const f=m.fields.map(x=>'<div class=\"mf\"><div class=\"k\">'+x.field.replace(/_/g,' ')+'</div><div class=\"val\">'+x.value.toFixed(3)+' <small style=\"color:var(--mut)\">'+x.unit+'</small></div></div>').join('');return '<div class=\"meter\"><div class=\"meter-top\"><div><div class=\"meter-name\">'+(m.name||m.id)+'</div><div class=\"meter-id\">ID: '+m.id+'</div></div><span class=\"tag '+tagClass(m.type)+'\">'+tagLabel(m.type)+'</span></div><div class=\"meter-fields\">'+(f||'<div style=\"color:var(--mut);font-size:.85rem\">Brak pol</div>')+'</div><div class=\"signal\"><i style=\"width:'+rssiW(m.rssi)+'%\"></i></div><div style=\"font-size:.72rem;color:var(--mut);margin-top:6px\">Sygnal '+m.rssi+' dBm</div></div>'}"
"function showPage(name){document.querySelectorAll('.page').forEach(p=>p.classList.remove('active'));document.querySelectorAll('.nav-item').forEach(t=>t.classList.remove('active'));if(!document.getElementById('page-'+name))name='dashboard';document.getElementById('page-'+name).classList.add('active');const nv=document.querySelector('.nav-item[data-page=\"'+name+'\"]');if(nv)nv.classList.add('active');document.getElementById('ptitle').textContent=titles[name]||name;if(location.hash!=='#'+name)location.hash=name;if(name==='logs')startLogs();else if(logTimer){clearInterval(logTimer);logTimer=null}if(name==='update')fetchStatus();if(window.innerWidth<=820)closeSidebar()}"
"function toggleSidebar(){document.getElementById('sidebar').classList.toggle('open');document.getElementById('overlay').classList.toggle('show')}"
"function closeSidebar(){document.getElementById('sidebar').classList.remove('open');document.getElementById('overlay').classList.remove('show')}"
"async function fetchStatus(){try{const d=await(await fetch('/api/status')).json();const c=document.getElementById('wifiChip'),t=document.getElementById('wifiChipTxt');document.getElementById('d-ver').textContent=d.version||'--';document.getElementById('u-ver').textContent=d.version||'--';document.getElementById('d-uptime').textContent=fmtUp(d.uptime_ms);document.getElementById('d-meters').textContent=d.meter_count||0;document.getElementById('d-ip').textContent=d.ip||'--';document.getElementById('d-rssi').innerHTML=(d.wifi_rssi||'--')+'<small> dBm</small>';if(d.wifi_state==='connected'){c.className='chip ok';t.textContent=d.ip;document.getElementById('d-wifistate').textContent='polaczono'}else if(d.wifi_state==='ap_mode'){c.className='chip warn';t.textContent='Tryb AP';document.getElementById('d-wifistate').textContent='AP'}else{c.className='chip err';t.textContent='Brak WiFi';document.getElementById('d-wifistate').textContent='--'}}catch(e){}}"

"function hexToBytes(h){const b=new Uint8Array(h.length/2);for(let i=0;i<b.length;i++)b[i]=parseInt(h.substr(i*2,2),16);return b}"
"function u32be(d,o){return((d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3])>>>0}"
"function convKey(h){const b=hexToBytes(h);return(u32be(b,0)^u32be(b,4))>>>0}"
"function manuf(b){const m=b[2]|(b[3]<<8);return String.fromCharCode(((m>>10)&0x1F)+64,((m>>5)&0x1F)+64,(m&0x1F)+64)}"
"function meterId(b){const h=x=>x.toString(16).padStart(2,'0');return h(b[7])+h(b[6])+h(b[5])+h(b[4])}"
"const IZAR_KEYS=['39BC8A10E66D83F8','51728910E66D83F8'];"
"function izarDecode(frame){for(const kh of IZAR_KEYS){let k=convKey(kh);k=(k^u32be(frame,2))>>>0;k=(k^u32be(frame,6))>>>0;k=(k^u32be(frame,12))>>>0;const size=frame.length-17;if(size<=0)continue;const dec=new Uint8Array(size);let kk=k;for(let i=0;i<size;i++){for(let j=0;j<8;j++){const bit=(((kk>>>1)&1)^((kk>>>2)&1)^((kk>>>11)&1)^((kk>>>31)&1))&1;kk=((kk<<1)|bit)>>>0}dec[i]=frame[i+17]^(kk&0xFF)}if(dec[0]===0x4B){const t=(dec[1]|(dec[2]<<8)|(dec[3]<<16)|(dec[4]<<24))>>>0;return{total_m3:t/1000}}}return null}"
"function bcd(b,p,n){let s='';for(let i=n-1;i>=0;i--)s+=((b[p+i]>>4)&15).toString()+(b[p+i]&15).toString();return parseInt(s,10)}"
"function rdVal(b,p,dif){const c=dif&15;switch(c){case 0:return{val:0,len:0};case 1:return{val:(b[p]<<24>>24),len:1};case 2:{let v=b[p]|(b[p+1]<<8);return{val:(v<<16>>16),len:2}}case 3:{let v=b[p]|(b[p+1]<<8)|(b[p+2]<<16);if(v&0x800000)v-=0x1000000;return{val:v,len:3}}case 4:return{val:(b[p]|(b[p+1]<<8)|(b[p+2]<<16)|(b[p+3]<<24))|0,len:4};case 6:{let v=0;for(let i=0;i<6;i++)v+=b[p+i]*Math.pow(2,8*i);return{val:v,len:6}}case 7:{let v=0;for(let i=0;i<8;i++)v+=b[p+i]*Math.pow(2,8*i);return{val:v,len:8}}case 9:return{val:bcd(b,p,1),len:1};case 10:return{val:bcd(b,p,2),len:2};case 11:return{val:bcd(b,p,3),len:3};case 12:return{val:bcd(b,p,4),len:4};case 14:return{val:bcd(b,p,6),len:6};case 13:{const l=b[p];return{val:0,len:l+1,isStr:true}}default:return{val:0,len:0}}}"
"function dVIF(vif){const v=vif&0x7F;if(v<=7)return{q:'Energy',u:'kWh',s:Math.pow(10,v-3)/1000};if(v>=8&&v<=15)return{q:'Energy',u:'kWh',s:Math.pow(10,v-3)/3600000};if(v>=16&&v<=23)return{q:'Volume',u:'m3',s:Math.pow(10,(v&7)-6)};if(v>=40&&v<=47)return{q:'Power',u:'kW',s:Math.pow(10,(v&7)-3)/1000};if(v>=56&&v<=63)return{q:'Flow',u:'m3h',s:Math.pow(10,(v&7)-6)};if(v===0x6C)return{q:'Date',u:'',s:1};if(v===0x6D)return{q:'DateTime',u:'',s:1};return{q:'Unknown',u:'',s:1}}"
"function dVIFE_FD(vife){const v=vife&0x7F;if(v>=0x40&&v<=0x4F)return{q:'Voltage',u:'V',s:Math.pow(10,(v&15)-9)};if(v>=0x50&&v<=0x5F)return{q:'Current',u:'A',s:Math.pow(10,(v&15)-12)};return{q:'Unknown',u:'',s:1}}"
"function parseDifVif(payload){const recs=[];let pos=0;while(pos<payload.length){let dif=payload[pos++];if(dif===0x2F)continue;if(dif===0x0F||dif===0x1F)break;let tariff=0,ext=dif&0x80;while(ext&&pos<payload.length){const de=payload[pos++];tariff|=((de>>4)&15);ext=de&0x80}if(pos>=payload.length)break;let vif=payload[pos++];let vi;if(vif===0xFD){const vife=payload[pos++];vi=dVIFE_FD(vife);let e=vife&0x80;while(e&&pos<payload.length){e=payload[pos]&0x80;pos++}}else if(vif&0x80){vi=dVIF(vif);let e=1;while(e&&pos<payload.length){e=payload[pos]&0x80;pos++}}else{vi=dVIF(vif)}const r=rdVal(payload,pos,dif);pos+=r.len;if(!r.isStr&&vi.q!=='Unknown'&&vi.q!=='Date'&&vi.q!=='DateTime')recs.push({tariff,quantity:vi.q,unit:vi.u,value:(typeof r.val==='number')?r.val*vi.s:r.val})}return recs}"
"function amiplusDecode(b){let ci=-1;for(let i=10;i<Math.min(b.length,16);i++){if(b[i]===0x7A){ci=i;break}}if(ci<0)return[];const payload=b.slice(ci+5);const recs=parseDifVif(payload);const out=[];let ve=0,vp=0,vv=0;for(const r of recs){if(r.quantity==='Energy'&&!r.tariff&&ve<1){out.push({field:'energia_kwh',value:r.value,unit:'kWh'});ve++}else if(r.quantity==='Power'&&vp<1){out.push({field:'moc_kw',value:r.value,unit:'kW'});vp++}else if(r.quantity==='Voltage'&&vv<3){out.push({field:'napiecie_l'+(vv+1)+'_v',value:r.value,unit:'V'});vv++}}return out}"
"function decodeFrame(hex){const b=hexToBytes(hex);if(b.length<10)return null;const o={id:meterId(b),manufacturer:manuf(b),version:b[8],medium:b[9],rssi:0,fields:[]};if(o.manufacturer==='SAP'&&o.medium===0x01){o.type='izar';const r=izarDecode(b);if(r)o.fields.push({field:'total_m3',value:r.total_m3,unit:'m3'})}else if(o.manufacturer==='APA'){o.type='apator162'}else if(o.medium===0x02){o.type='amiplus';const fl=amiplusDecode(b);for(const x of fl)o.fields.push(x)}else if(o.medium===0x03){o.type='gas'}else{o.type='unknown'}return o}"
"async function fetchFrames(){try{const fr=await(await fetch('/api/frames')).json();const byId={};for(const f of fr){const d=decodeFrame(f.hex);if(!d)continue;if(!byId[d.id]||f.ts>byId[d.id]._ts){d.rssi=f.rssi;d._ts=f.ts;byId[d.id]=d}}return Object.values(byId)}catch(e){return[]}}"
"async function fetchMeters(){const m=await fetchFrames();const dm=document.getElementById('dash-meters'),ml=document.getElementById('meters-list');if(!m.length){dm.innerHTML='<div class=\"empty\">Oczekiwanie na ramki wMbus...</div>';ml.innerHTML='<div class=\"empty\">Brak wykrytych licznikow</div>';return}const html=m.map(meterCard).join('');dm.innerHTML=html;ml.innerHTML=html}"
"function fetchAll(){fetchStatus();fetchMeters()}"
"async function saveWifi(){const s=document.getElementById('ssid').value.trim(),p=document.getElementById('pass').value,msg=document.getElementById('wifi-msg');if(!s){msg.className='msg err';msg.textContent='Podaj nazwe sieci!';return}msg.className='msg ok';msg.textContent='Zapisywanie...';try{await fetch('/api/config/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,password:p})})}catch(e){}msg.textContent='Zapisano - modul restartuje sie i laczy z '+s}"
"async function otaGithub(){const msg=document.getElementById('ota-msg'),pb=document.getElementById('ota-pbar');if(!confirm('Pobrac i wgrac najnowszy firmware z GitHub?'))return;msg.className='msg ok';msg.textContent='Modul laczy sie z GitHub...';pb.style.display='block';try{await fetch('/api/ota/github',{method:'POST'});if(otaPoller)clearInterval(otaPoller);otaPoller=setInterval(pollOta,1500)}catch(e){msg.className='msg err';msg.textContent='Blad: '+e}}"
"async function pollOta(){try{const d=await(await fetch('/api/ota/status')).json();document.getElementById('ota-bar').style.width=d.progress+'%';const L={idle:'',downloading:'Pobieranie...',writing:'Zapisywanie '+d.progress+'%',success:'OK - restart...',failed:'Blad: '+d.error};const msg=document.getElementById('ota-msg');if(d.state!=='idle')msg.textContent=L[d.state]||d.state;if(d.state==='failed')msg.className='msg err';if(d.state==='success'||d.state==='failed'){clearInterval(otaPoller);otaPoller=null}}catch(e){}}"
"function stripAnsi(s){return s.replace(/\\x1b\\[[0-9;]*m/g,'')}"
"async function fetchLogs(){try{let t=await(await fetch('/api/logs')).text();t=stripAnsi(t);const el=document.getElementById('log-output');if(!el)return;const b=el.scrollHeight-el.scrollTop-el.clientHeight<40;el.textContent=t||'(brak logow)';if(b)el.scrollTop=el.scrollHeight}catch(e){}}"
"function startLogs(){fetchLogs();if(logTimer)clearInterval(logTimer);if(logAuto)logTimer=setInterval(fetchLogs,2000)}"
"function toggleAutoLogs(){logAuto=!logAuto;document.getElementById('log-auto').textContent='Auto: '+(logAuto?'ON':'OFF');if(logAuto)startLogs();else if(logTimer){clearInterval(logTimer);logTimer=null}}"
"async function clearLogs(){try{await fetch('/api/logs/clear',{method:'POST'});document.getElementById('log-output').textContent='(wyczyszczono)'}catch(e){}}"
"document.getElementById('nav').addEventListener('click',e=>{const b=e.target.closest('.nav-item');if(b)showPage(b.dataset.page)});"
"window.addEventListener('hashchange',()=>showPage((location.hash||'#dashboard').slice(1)));"
"showPage((location.hash||'#dashboard').slice(1));fetchAll();setInterval(fetchAll,5000);"
"</script></body></html>"
"";

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
