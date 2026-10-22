#include "auth.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "nvs.h"
#include "nvs_config.h"   // NVS_NAMESPACE
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "AUTH";

#define AUTH_KEY      "auth"       // ustawienia: uzytkownik, sol, skrot hasla
#define AUTH_SES_KEY  "authses"    // zapamietane sesje ("zapamietaj mnie")
#define AUTH_VER      1
#define AUTH_ITER     10000        // PBKDF2: ok. 0,5 s na ESP32-C6 przy logowaniu
#define USER_MAX      32
#define PASS_MIN      8
#define PASS_MAX      64
#define MAX_SES       8
#define SES_LONG_S    (30u * 86400u)  // "zapamietaj mnie"
#define SES_SHORT_S   (12u * 3600u)   // bez zapamietania
#define FAIL_MAX      5               // nieudane proby przed blokada
#define LOCK_BASE_S   60              // pierwsza blokada; kazda kolejna 2x dluzsza
#define LOCK_MAX_LVL  4               // najwyzej 60 s << 4 = 16 min
#define MAX_IPS       8
#define CLOCK_OK      1700000000u     // ponizej: zegar jeszcze nieustawiony
#define COOKIE_NAME   "sih_s"

typedef struct {
    uint8_t  ver;
    uint8_t  enabled;
    uint8_t  pad[2];
    uint32_t iter;
    char     user[USER_MAX + 1];
    uint8_t  salt[16];
    uint8_t  hash[32];
} auth_cfg_t;

typedef struct {
    uint8_t  th[16];    // poczatek SHA-256 klucza sesji - samego klucza nie trzymamy
    uint32_t since;     // poczatek sesji (0 = zegar nieznany)
    uint32_t exp;       // koniec waznosci (0 = do restartu, zegar nieznany)
    uint8_t  used;
    uint8_t  remember;
    uint8_t  pad[2];
} auth_ses_t;

typedef struct {
    uint32_t ip;        // adres w kolejnosci sieciowej
    uint8_t  fails;
    uint8_t  lvl;
    int64_t  until_us;  // blokada do (esp_timer)
    int64_t  last_us;
} ip_slot_t;

static auth_cfg_t s_cfg;
static auth_ses_t s_ses[MAX_SES];
static ip_slot_t  s_ips[MAX_IPS];
static uint32_t   s_fail_ts;
static uint32_t   s_fail_ip;
static SemaphoreHandle_t s_mtx;
// Set-Cookie: httpd trzyma wskaznik do wartosci naglowka az do wyslania
// odpowiedzi, wiec bufor nie moze lezec na stosie handlera.
static char s_cookie_hdr[128];

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static uint32_t now_s(void) { return (uint32_t)time(NULL); }
static bool clock_ok(void)  { return now_s() >= CLOCK_OK; }

// ---------------------------------------------------------------- pomocnicze

static bool ct_eq(const void *a, const void *b, size_t n) {
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d == 0;
}

static void bin2hex(const uint8_t *b, int n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = hx[b[i] >> 4]; out[2 * i + 1] = hx[b[i] & 15]; }
    out[2 * n] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex2bin(const char *s, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int hi = hexval(s[2 * i]), lo = (hi < 0) ? -1 : hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool valid_user(const char *u) {
    size_t n = u ? strlen(u) : 0;
    if (n < 1 || n > USER_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = u[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                  || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

static void ip_str(uint32_t ip, char out[16]) {
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

// Adres klienta (IPv4; przy gniezdzie IPv6 - adres IPv4 zmapowany).
static uint32_t client_ip(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    memset(&ss, 0, sizeof(ss));
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&ss, &len) != 0) return 0;
    uint32_t ip = 0;
    if (ss.ss_family == AF_INET) {
        ip = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
    }
#if LWIP_IPV6
    else if (ss.ss_family == AF_INET6) {
        memcpy(&ip, &((struct sockaddr_in6 *)&ss)->sin6_addr.s6_addr[12], 4);
    }
#endif
    return ip;
}

// PBKDF2-HMAC-SHA256, jeden blok 32 B (RFC 8018). Wlasna petla na HMAC z mbedTLS,
// zeby nie zalezec od opcji MBEDTLS_PKCS5_C w konfiguracji ESP-IDF.
static void pbkdf2_sha256(const char *pass, const uint8_t *salt, size_t salt_len,
                          uint32_t iter, uint8_t out[32]) {
    uint8_t u[32], t[32];
    const uint8_t blk[4] = { 0, 0, 0, 1 };
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) != 0) {
        memset(out, 0, 32);
        mbedtls_md_free(&ctx);
        return;
    }
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)pass, strlen(pass));
    mbedtls_md_hmac_update(&ctx, salt, salt_len);
    mbedtls_md_hmac_update(&ctx, blk, 4);
    mbedtls_md_hmac_finish(&ctx, u);
    memcpy(t, u, 32);
    for (uint32_t i = 1; i < iter; i++) {
        mbedtls_md_hmac_reset(&ctx);
        mbedtls_md_hmac_update(&ctx, u, 32);
        mbedtls_md_hmac_finish(&ctx, u);
        for (int j = 0; j < 32; j++) t[j] ^= u[j];
        if ((i & 1023) == 0) vTaskDelay(1);   // oddaj procesor innym taskom
    }
    memcpy(out, t, 32);
    memset(u, 0, sizeof(u));
    memset(t, 0, sizeof(t));
    mbedtls_md_free(&ctx);
}

// ---------------------------------------------------------------- NVS

static void cfg_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Nie moge zapisac ustawien logowania");
        return;
    }
    if (nvs_set_blob(h, AUTH_KEY, &s_cfg, sizeof(s_cfg)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

// Zapisujemy tylko sesje "zapamietaj mnie" ze znanym koncem waznosci.
// Pozostale koncza sie razem z restartem modulu.
static void ses_save(void) {
    auth_ses_t cp[MAX_SES];
    memset(cp, 0, sizeof(cp));
    for (int i = 0; i < MAX_SES; i++)
        if (s_ses[i].used && s_ses[i].remember && s_ses[i].exp) cp[i] = s_ses[i];
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_blob(h, AUTH_SES_KEY, cp, sizeof(cp)) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

void auth_init(void) {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    memset(&s_cfg, 0, sizeof(s_cfg));
    memset(s_ses, 0, sizeof(s_ses));
    memset(s_ips, 0, sizeof(s_ips));
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_cfg);
        if (nvs_get_blob(h, AUTH_KEY, &s_cfg, &len) != ESP_OK || len != sizeof(s_cfg)
            || s_cfg.ver != AUTH_VER)
            memset(&s_cfg, 0, sizeof(s_cfg));
        len = sizeof(s_ses);
        if (nvs_get_blob(h, AUTH_SES_KEY, s_ses, &len) != ESP_OK || len != sizeof(s_ses))
            memset(s_ses, 0, sizeof(s_ses));
        nvs_close(h);
    }
    s_cfg.user[USER_MAX] = 0;
    if (!s_cfg.enabled) memset(s_ses, 0, sizeof(s_ses));
    ESP_LOGI(TAG, "Logowanie do panelu: %s", s_cfg.enabled ? "wlaczone" : "wylaczone");
}

bool auth_enabled(void) { return s_cfg.enabled != 0; }

// ---------------------------------------------------------------- sesje

static bool cookie_token(httpd_req_t *req, uint8_t tok[16]) {
    size_t n = httpd_req_get_hdr_value_len(req, "Cookie");
    if (n == 0 || n > 1024) return false;
    char *buf = malloc(n + 1);
    if (!buf) return false;
    bool ok = false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", buf, n + 1) == ESP_OK) {
        const size_t kl = strlen(COOKIE_NAME "=");
        for (const char *p = buf; (p = strstr(p, COOKIE_NAME "=")) != NULL; p++) {
            if (p != buf && p[-1] != ' ' && p[-1] != ';') continue;   // np. "xsih_s="
            const char *v = p + kl;
            if (strlen(v) >= 32 && (v[32] == 0 || v[32] == ';' || v[32] == ' '))
                ok = hex2bin(v, tok, 16);
            break;
        }
    }
    free(buf);
    return ok;
}

// Indeks waznej sesji z ciasteczka zapytania albo -1. Wolac pod LOCK().
static int ses_find(httpd_req_t *req) {
    uint8_t tok[16], th[32];
    if (!cookie_token(req, tok)) return -1;
    mbedtls_sha256(tok, sizeof(tok), th, 0);
    uint32_t now = now_s();
    for (int i = 0; i < MAX_SES; i++) {
        if (!s_ses[i].used || !ct_eq(s_ses[i].th, th, 16)) continue;
        // Bez ustawionego zegara (zaraz po starcie) waznosci nie da sie sprawdzic.
        if (s_ses[i].exp && now >= CLOCK_OK && now >= s_ses[i].exp) {
            s_ses[i].used = 0;
            return -1;
        }
        return i;
    }
    return -1;
}

// Nowa sesja; ustawia naglowek Set-Cookie w s_cookie_hdr. Wolac pod LOCK().
static int ses_new(bool remember) {
    int slot = -1;
    for (int i = 0; i < MAX_SES; i++) if (!s_ses[i].used) { slot = i; break; }
    if (slot < 0) {                       // komplet - zastap najstarsza
        slot = 0;
        for (int i = 1; i < MAX_SES; i++) if (s_ses[i].since < s_ses[slot].since) slot = i;
    }
    uint8_t tok[16], th[32];
    char hex[33];
    esp_fill_random(tok, sizeof(tok));
    mbedtls_sha256(tok, sizeof(tok), th, 0);
    bin2hex(tok, sizeof(tok), hex);
    auth_ses_t *s = &s_ses[slot];
    memset(s, 0, sizeof(*s));
    memcpy(s->th, th, sizeof(s->th));
    s->used = 1;
    s->remember = remember ? 1 : 0;
    if (clock_ok()) {
        s->since = now_s();
        s->exp = s->since + (remember ? SES_LONG_S : SES_SHORT_S);
    }
    if (remember)
        snprintf(s_cookie_hdr, sizeof(s_cookie_hdr),
                 COOKIE_NAME "=%s; Path=/; Max-Age=%u; HttpOnly; SameSite=Strict", hex, SES_LONG_S);
    else
        snprintf(s_cookie_hdr, sizeof(s_cookie_hdr),
                 COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict", hex);
    memset(tok, 0, sizeof(tok));
    memset(hex, 0, sizeof(hex));
    return slot;
}

static void cookie_clear(httpd_req_t *req) {
    snprintf(s_cookie_hdr, sizeof(s_cookie_hdr),
             COOKIE_NAME "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    httpd_resp_set_hdr(req, "Set-Cookie", s_cookie_hdr);
}

// ---------------------------------------------------------------- blokada

static ip_slot_t *ip_slot(uint32_t ip, bool create) {
    ip_slot_t *old = &s_ips[0];
    for (int i = 0; i < MAX_IPS; i++) {
        if (s_ips[i].ip == ip && (s_ips[i].fails || s_ips[i].lvl || s_ips[i].until_us)) return &s_ips[i];
        if (s_ips[i].last_us < old->last_us) old = &s_ips[i];
    }
    if (!create) return NULL;
    memset(old, 0, sizeof(*old));        // wolny albo najdawniej uzyty
    old->ip = ip;
    return old;
}

static int lock_left_s(uint32_t ip) {
    ip_slot_t *s = ip_slot(ip, false);
    int64_t now = esp_timer_get_time();
    if (!s || s->until_us <= now) return 0;
    return (int)((s->until_us - now) / 1000000) + 1;
}

// Zapisuje nieudana probe; zwraca czas blokady w s (0 = jeszcze bez blokady),
// a w *left liczbe pozostalych prob.
static int fail_note(uint32_t ip, int *left) {
    ip_slot_t *s = ip_slot(ip, true);
    int64_t now = esp_timer_get_time();
    s->last_us = now;
    s->fails++;
    s_fail_ts = now_s();
    s_fail_ip = ip;
    if (s->fails < FAIL_MAX) { *left = FAIL_MAX - s->fails; return 0; }
    int secs = LOCK_BASE_S << s->lvl;
    s->until_us = now + (int64_t)secs * 1000000;
    if (s->lvl < LOCK_MAX_LVL) s->lvl++;
    s->fails = 0;
    *left = 0;
    return secs;
}

static void fail_clear(uint32_t ip) {
    ip_slot_t *s = ip_slot(ip, false);
    if (s) memset(s, 0, sizeof(*s));
}

// ---------------------------------------------------------------- HTTP

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json) {
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

// Stan logowania. Nazwe uzytkownika i szczegoly widzi tylko zalogowany.
static void status_json(int si, char *out, size_t cap) {
    if (!s_cfg.enabled) { snprintf(out, cap, "{\"enabled\":false}"); return; }
    if (si < 0)         { snprintf(out, cap, "{\"enabled\":true,\"logged\":false}"); return; }
    int n = 0;
    for (int i = 0; i < MAX_SES; i++) if (s_ses[i].used) n++;
    char fip[16] = "";
    if (s_fail_ip) ip_str(s_fail_ip, fip);
    snprintf(out, cap,
             "{\"enabled\":true,\"logged\":true,\"user\":\"%s\",\"since\":%u,\"exp\":%u,"
             "\"remember\":%s,\"sessions\":%d,\"fail_ts\":%u,\"fail_ip\":\"%s\"}",
             s_cfg.user, (unsigned)s_ses[si].since, (unsigned)s_ses[si].exp,
             s_ses[si].remember ? "true" : "false", n, (unsigned)s_fail_ts, fip);
}

esp_err_t auth_gate(httpd_req_t *req) {
    esp_err_t (*h)(httpd_req_t *) = (esp_err_t (*)(httpd_req_t *))req->user_ctx;
    LOCK();
    bool ok = !s_cfg.enabled || ses_find(req) >= 0;
    UNLOCK();
    if (ok) return h(req);
    send_json(req, "401 Unauthorized", "{\"error\":\"auth\"}");
    // Duza tresc (np. plik firmware) - zamknij polaczenie zamiast ja wczytywac.
    return (req->content_len > 2048) ? ESP_FAIL : ESP_OK;
}

// Tekst pola JSON albo NULL (bez zaleznosci od wersji cJSON).
static const char *jstr(cJSON *js, const char *key) {
    cJSON *it = cJSON_GetObjectItem(js, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

static void wipe_str(cJSON *it) {
    if (cJSON_IsString(it) && it->valuestring) memset(it->valuestring, 0, strlen(it->valuestring));
}

static esp_err_t do_login(httpd_req_t *req, cJSON *js, char *out, size_t cap) {
    if (!s_cfg.enabled) { status_json(-1, out, cap); return send_json(req, NULL, out); }
    uint32_t ip = client_ip(req);
    LOCK();
    int wait = lock_left_s(ip);
    auth_cfg_t c = s_cfg;
    UNLOCK();
    if (wait > 0) {
        snprintf(out, cap, "{\"error\":\"lock\",\"wait\":%d}", wait);
        return send_json(req, "429 Too Many Requests", out);
    }
    const char *u = jstr(js, "user");
    const char *p = jstr(js, "pass");
    bool remember = cJSON_IsTrue(cJSON_GetObjectItem(js, "remember"));
    uint8_t h[32];
    // Skrot liczymy zawsze, takze dla zlej nazwy - czas odpowiedzi nie zdradza,
    // czy uzytkownik istnieje.
    pbkdf2_sha256((p && strlen(p) <= PASS_MAX) ? p : "", c.salt, sizeof(c.salt), c.iter ? c.iter : AUTH_ITER, h);
    bool good = u && p && strlen(u) == strlen(c.user) && ct_eq(u, c.user, strlen(c.user))
                && ct_eq(h, c.hash, sizeof(h));
    memset(h, 0, sizeof(h));
    memset(&c, 0, sizeof(c));
    char ips[16];
    ip_str(ip, ips);
    LOCK();
    if (!good) {
        int left = 0;
        int secs = fail_note(ip, &left);
        UNLOCK();
        ESP_LOGW(TAG, "Nieudane logowanie z %s", ips);
        if (secs) {
            snprintf(out, cap, "{\"error\":\"lock\",\"wait\":%d}", secs);
            return send_json(req, "429 Too Many Requests", out);
        }
        snprintf(out, cap, "{\"error\":\"bad\",\"left\":%d}", left);
        return send_json(req, "401 Unauthorized", out);
    }
    fail_clear(ip);
    int si = ses_new(remember);
    ses_save();
    status_json(si, out, cap);
    UNLOCK();
    ESP_LOGI(TAG, "Zalogowano z %s", ips);
    httpd_resp_set_hdr(req, "Set-Cookie", s_cookie_hdr);
    return send_json(req, NULL, out);
}

static esp_err_t do_config(httpd_req_t *req, cJSON *js, char *out, size_t cap) {
    LOCK();
    bool was = s_cfg.enabled != 0;
    int si = was ? ses_find(req) : -1;
    UNLOCK();
    if (was && si < 0) return send_json(req, "401 Unauthorized", "{\"error\":\"auth\"}");

    if (!cJSON_IsTrue(cJSON_GetObjectItem(js, "enabled"))) {
        LOCK();
        s_cfg.enabled = 0;
        memset(s_ses, 0, sizeof(s_ses));
        cfg_save();
        ses_save();
        UNLOCK();
        ESP_LOGW(TAG, "Logowanie do panelu wylaczone");
        cookie_clear(req);
        return send_json(req, NULL, "{\"enabled\":false}");
    }

    const char *u = jstr(js, "user");
    const char *p = jstr(js, "pass");
    if (p && !*p) p = NULL;
    if (!valid_user(u)) return send_json(req, "400 Bad Request", "{\"error\":\"user\"}");
    if (p && (strlen(p) < PASS_MIN || strlen(p) > PASS_MAX))
        return send_json(req, "400 Bad Request", "{\"error\":\"pass\"}");
    // Wlaczenie (takze ponowne) zawsze z nowym haslem - nie wracamy po cichu
    // do hasla, ktorego uzytkownik mogl juz nie pamietac.
    if (!was && !p) return send_json(req, "400 Bad Request", "{\"error\":\"pass\"}");

    uint8_t salt[16], hash[32];
    if (p) {
        esp_fill_random(salt, sizeof(salt));
        pbkdf2_sha256(p, salt, sizeof(salt), AUTH_ITER, hash);
    }
    LOCK();
    s_cfg.ver = AUTH_VER;
    strlcpy(s_cfg.user, u, sizeof(s_cfg.user));
    if (p) {
        memcpy(s_cfg.salt, salt, sizeof(salt));
        memcpy(s_cfg.hash, hash, sizeof(hash));
        s_cfg.iter = AUTH_ITER;
        // Nowe haslo wylogowuje wszystkie inne urzadzenia.
        memset(s_ses, 0, sizeof(s_ses));
    }
    s_cfg.enabled = 1;
    cfg_save();
    bool fresh = false;
    if (p || si < 0) {
        si = ses_new(true);              // ten, kto ustawil haslo, zostaje zalogowany
        fresh = true;
    }
    ses_save();
    status_json(si, out, cap);
    UNLOCK();
    memset(hash, 0, sizeof(hash));
    ESP_LOGW(TAG, "Logowanie do panelu wlaczone (uzytkownik %s%s)", u, p ? ", nowe haslo" : "");
    if (fresh) httpd_resp_set_hdr(req, "Set-Cookie", s_cookie_hdr);
    return send_json(req, NULL, out);
}

esp_err_t auth_handle(httpd_req_t *req) {
    static char out[384];   // handlery httpd dzialaja na jednym tasku
    if (req->method == HTTP_GET) {
        LOCK();
        int si = s_cfg.enabled ? ses_find(req) : -1;
        status_json(si, out, sizeof(out));
        UNLOCK();
        return send_json(req, NULL, out);
    }
    if (req->content_len == 0 || req->content_len > 512)
        return send_json(req, "400 Bad Request", "{\"error\":\"body\"}");
    char body[513];
    int got = 0;
    while (got < (int)req->content_len) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) return send_json(req, "400 Bad Request", "{\"error\":\"body\"}");
        got += r;
    }
    body[got] = 0;
    cJSON *js = cJSON_Parse(body);
    memset(body, 0, sizeof(body));
    if (!js) return send_json(req, "400 Bad Request", "{\"error\":\"body\"}");
    const char *a = jstr(js, "action");
    esp_err_t r;
    if (a && !strcmp(a, "login")) {
        r = do_login(req, js, out, sizeof(out));
    } else if (a && !strcmp(a, "logout")) {
        LOCK();
        int si = s_cfg.enabled ? ses_find(req) : -1;
        if (si >= 0) { s_ses[si].used = 0; ses_save(); }
        UNLOCK();
        cookie_clear(req);
        r = send_json(req, NULL, "{\"status\":\"ok\"}");
    } else if (a && !strcmp(a, "logout_all")) {
        LOCK();
        bool ok = !s_cfg.enabled || ses_find(req) >= 0;
        if (ok) { memset(s_ses, 0, sizeof(s_ses)); ses_save(); }
        UNLOCK();
        if (ok) { cookie_clear(req); r = send_json(req, NULL, "{\"status\":\"ok\"}"); }
        else r = send_json(req, "401 Unauthorized", "{\"error\":\"auth\"}");
    } else if (a && !strcmp(a, "config")) {
        r = do_config(req, js, out, sizeof(out));
    } else {
        r = send_json(req, "400 Bad Request", "{\"error\":\"action\"}");
    }
    wipe_str(cJSON_GetObjectItem(js, "pass"));
    cJSON_Delete(js);
    return r;
}

// Zapis do NVS w osobnym, krotkim tasku: task przycisku ma tylko 3 KB stosu.
static void save_task(void *arg) {
    (void)arg;
    LOCK();
    cfg_save();
    ses_save();
    UNLOCK();
    vTaskDelete(NULL);
}

void auth_button_reset(void) {
    if (!s_mtx) return;
    LOCK();
    bool was = s_cfg.enabled != 0;
    s_cfg.enabled = 0;
    memset(s_ses, 0, sizeof(s_ses));
    memset(s_ips, 0, sizeof(s_ips));
    UNLOCK();
    if (was && xTaskCreate(save_task, "auth_save", 4096, NULL, 3, NULL) != pdPASS)
        ESP_LOGW(TAG, "Brak pamieci na zapis - logowanie wylaczone do restartu");
    ESP_LOGW(TAG, "Przycisk przewijania stron trzymany 10 s: logowanie do panelu %s",
             was ? "wylaczone" : "bylo juz wylaczone");
}
