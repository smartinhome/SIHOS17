#include "esphome_api.h"
#include "nvs_config.h"
#include "history.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static const char *TAG = "ESPHOME";

// ---------------------------------------------------------------------------
// Zamrozona wersja protokolu: ESPHome 2025.6.3, API 1.10. Numery komunikatow
// i pol pochodza z esphome/components/api/api.proto tego wydania. Wersja 1.10
// obowiazywala od ESPHome 2024.12 do 2025.6, wiec rozumie ja kazdy HA z
// ostatnich lat - aktualizacje ESPHome nie sa potrzebne.
// ---------------------------------------------------------------------------
#define API_MAJOR          1
#define API_MINOR          10
#define ESPHOME_VER        "2025.6.3"

enum {
    MSG_HELLO_REQ = 1,  MSG_HELLO_RESP = 2,
    MSG_CONNECT_REQ = 3, MSG_CONNECT_RESP = 4,
    MSG_DISCONNECT_REQ = 5, MSG_DISCONNECT_RESP = 6,
    MSG_PING_REQ = 7,   MSG_PING_RESP = 8,
    MSG_DEVINFO_REQ = 9, MSG_DEVINFO_RESP = 10,
    MSG_LIST_REQ = 11,  MSG_LIST_SENSOR = 16, MSG_LIST_DONE = 19,
    MSG_SUB_STATES = 20, MSG_SENSOR_STATE = 25,
};

enum { STATE_CLASS_NONE = 0, STATE_CLASS_MEASUREMENT = 1, STATE_CLASS_TOTAL_INCREASING = 2 };

// ---------------------------------------------------------------------------
// Pamiec. Wszystko poza stosem alokowane raz przy starcie serwera i zwalniane
// przy zatrzymaniu - wylaczony serwer nie zajmuje sterty.
// ---------------------------------------------------------------------------
#define ESPH_MAX_ENT       32          // tyle, ile pol moze byc na dashboardzie
#define RX_CAP             512         // komunikaty od HA maja kilkadziesiat bajtow
#define TX_CAP             512         // najwiekszy nasz komunikat ok. 200 B
#define TX_HDR             8           // miejsce na naglowek ramki przed trescia
#define TASK_STACK         4096

#define MIN_STATE_MS       15000       // jak w MQTT: to samo pole nie czesciej
#define RELIST_DEBOUNCE_MS 10000       // zbierz kilka nowych pol w jedno odswiezenie
#define RELIST_GAP_MS      60000       // odswiezenie listy najwyzej raz na minute
#define PRUNE_EVERY_MS     15000
#define PING_IDLE_MS       60000
#define DEAD_IDLE_MS       150000
#define HELLO_TIMEOUT_MS   10000
#define CLOSING_MS         3000
#define CACHE_DELAY_MS     5000

#define EF_VALUE   0x01                // jest wartosc z ramki
#define EF_DIRTY   0x02                // wartosc czeka na wyslanie
#define EF_LISTED  0x04                // HA zna te encje w biezacej sesji

typedef struct {
    char     id[12];
    char     field[32];
    char     unit[8];
    float    value;
    uint32_t key;
    uint32_t sent_ms;
    uint8_t  flags;
} ent_t;

// Rekord w NVS: lista encji przetrwa restart. HA usuwa z rejestru encje,
// ktorych urzadzenie nie zglosi - razem z nazwami i pokojami nadanymi przez
// uzytkownika. Bez tej listy kazdy restart modulu kasowalby je, zanim
// liczniki zdaza nadac pierwsze ramki.
typedef struct {
    char id[12];
    char field[32];
    char unit[8];
} cache_rec_t;

#define CACHE_VER  1
// Klucze w przestrzeni konfiguracji - reset fabryczny (nvs_erase_all) kasuje je razem z nia.
#define NVS_NS     NVS_NAMESPACE
#define KEY_EN     "esph_en"
#define KEY_ENTS   "esph_ents"

static SemaphoreHandle_t s_lock = NULL;
static ent_t   *s_ent = NULL;
static int      s_count = 0;
static uint32_t s_skipped = 0;
static uint8_t *s_rx = NULL, *s_tx = NULL;

static TaskHandle_t       s_task = NULL;
static SemaphoreHandle_t  s_done = NULL;
static volatile bool      s_run = false;
static int8_t             s_enabled = -1;      // -1 = jeszcze nie wczytane z NVS

static bool     s_list_dirty = false;
static uint32_t s_list_dirty_ms = 0;
static uint32_t s_last_relist_ms = 0;
static bool     s_cache_dirty = false;
static uint32_t s_cache_dirty_ms = 0;
static uint32_t s_last_prune_ms = 0;

enum { C_NONE = 0, C_HELLO_WAIT, C_READY, C_CLOSING };
static int      s_lsock = -1, s_csock = -1;
static uint8_t  s_cst = C_NONE;
static bool     s_subscribed = false, s_ping_sent = false;
static uint32_t s_accept_ms = 0, s_last_rx_ms = 0, s_closing_ms = 0;
static uint16_t s_rx_len = 0;
static uint32_t s_skip = 0;
static uint32_t s_states_sent = 0;
static uint32_t s_stack_free = 0;
static char     s_peer[16] = "";
static char     s_client_info[40] = "";
static const char *s_last_reason = "";
static char     s_name[24] = "sihos17";
static char     s_mac[18] = "";

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------------------------------------------------------------------
// Kodowanie protobuf - tylko typy, ktorych uzywamy. Pola o wartosci domyslnej
// pomijamy, tak jak ESPHome (proto3).
// ---------------------------------------------------------------------------
typedef struct { uint8_t *p, *end; bool ovf; } pw_t;

static void pw_byte(pw_t *w, uint8_t b) {
    if (w->p < w->end) *w->p++ = b; else w->ovf = true;
}
static void pw_varint(pw_t *w, uint32_t v) {
    while (v >= 0x80) { pw_byte(w, (uint8_t)(v | 0x80)); v >>= 7; }
    pw_byte(w, (uint8_t)v);
}
static void pw_tag(pw_t *w, uint32_t field, uint32_t wt) { pw_varint(w, (field << 3) | wt); }
static void pw_u32(pw_t *w, uint32_t f, uint32_t v) { if (v) { pw_tag(w, f, 0); pw_varint(w, v); } }
static void pw_bool(pw_t *w, uint32_t f, bool v) { if (v) { pw_tag(w, f, 0); pw_byte(w, 1); } }
static void pw_str(pw_t *w, uint32_t f, const char *s) {
    if (!s || !s[0]) return;
    size_t n = strlen(s);
    pw_tag(w, f, 2); pw_varint(w, (uint32_t)n);
    for (size_t i = 0; i < n; i++) pw_byte(w, (uint8_t)s[i]);
}
static void pw_fixed32(pw_t *w, uint32_t f, uint32_t v) {
    if (!v) return;
    pw_tag(w, f, 5);
    for (int i = 0; i < 4; i++) pw_byte(w, (uint8_t)(v >> (8 * i)));
}
static void pw_float(pw_t *w, uint32_t f, float v) {
    uint32_t u; memcpy(&u, &v, 4);
    pw_fixed32(w, f, u);              // 0.0f ma same zera - pomijane jak w ESPHome
}

static pw_t tx_begin(void) {
    pw_t w = { s_tx + TX_HDR, s_tx + TX_CAP, false };
    return w;
}

// ---------------------------------------------------------------------------
// Polaczenie
// ---------------------------------------------------------------------------
static void client_close(const char *reason) {
    if (s_csock >= 0) {
        shutdown(s_csock, SHUT_RDWR);
        close(s_csock);
        ESP_LOGI(TAG, "Rozlaczono %s: %s", s_peer, reason);
    }
    s_csock = -1;
    s_cst = C_NONE;
    s_subscribed = false;
    s_ping_sent = false;
    s_rx_len = 0;
    s_skip = 0;
    s_last_reason = reason;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) s_ent[i].flags &= (uint8_t)~EF_LISTED;
    xSemaphoreGive(s_lock);
}

static bool send_all(const uint8_t *p, size_t n) {
    while (n > 0) {
        int r = send(s_csock, p, n, 0);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

// Ramka bez szyfrowania: 0x00, dlugosc tresci (varint), typ (varint), tresc.
static bool tx_send(uint16_t type, const pw_t *w) {
    if (s_csock < 0) return false;
    if (w->ovf) { ESP_LOGE(TAG, "Komunikat %u za duzy - pominiety", type); return true; }
    uint8_t *body = s_tx + TX_HDR;
    uint32_t len = (uint32_t)(w->p - body);
    uint8_t hdr[TX_HDR];
    pw_t h = { hdr, hdr + sizeof(hdr), false };
    pw_byte(&h, 0x00);
    pw_varint(&h, len);
    pw_varint(&h, type);
    size_t hl = (size_t)(h.p - hdr);
    memcpy(body - hl, hdr, hl);
    if (!send_all(body - hl, hl + len)) { client_close("blad wysylania"); return false; }
    return true;
}

static bool tx_empty(uint16_t type) { pw_t w = tx_begin(); return tx_send(type, &w); }

// ---------------------------------------------------------------------------
// Encje
// ---------------------------------------------------------------------------
// Ta sama regula co w mqtt_pub.c: pole przypiete do dashboardu lub sledzone
// w historii.
static bool wanted(const char *id, const char *field) {
    char key[48];                       // id (11) + ':' + pole (31) + NUL
    snprintf(key, sizeof(key), "%s:%s", id, field);
    return history_is_tracked(key) || nvs_config_dash_field_is_set(key);
}

// object_id jak w ESPHome: male litery, podkreslniki. Z niego klucz sesji.
static void make_object_id(const ent_t *e, char *out, size_t cap) {
    snprintf(out, cap, "%s_%s", e->id, e->field);
    for (char *c = out; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        else if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) *c = '_';
    }
}

static uint32_t fnv1(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h *= 16777619u; h ^= (uint8_t)*s; }
    return h ? h : 1;
}

static void ent_set_key(int idx) {
    char oid[48];
    make_object_id(&s_ent[idx], oid, sizeof(oid));
    uint32_t k = fnv1(oid);
    for (int tries = 0; tries < 8; tries++) {
        bool clash = false;
        for (int i = 0; i < s_count; i++)
            if (i != idx && s_ent[i].key == k) { clash = true; break; }
        if (!clash) break;
        k++;
    }
    s_ent[idx].key = k;
}

static void mark_list_dirty(void) {
    if (!s_list_dirty) { s_list_dirty = true; s_list_dirty_ms = now_ms(); }
}
static void mark_cache_dirty(void) {
    if (!s_cache_dirty) { s_cache_dirty = true; s_cache_dirty_ms = now_ms(); }
}

// Jednostka w zapisie, ktory HA akceptuje dla danej klasy (jak w mqtt_pub.c).
static const char *ha_unit(const char *unit) {
    if (strcmp(unit, "m3") == 0)    return "m³";
    if (strcmp(unit, "kVARh") == 0) return "kvarh";
    if (strcmp(unit, "VAR") == 0)   return "var";
    return unit;
}

static void ha_class(const ent_t *e, const char **dc, uint32_t *sc, uint32_t *dec) {
    const char *u = e->unit;
    *dc = ""; *sc = STATE_CLASS_NONE; *dec = 2;
    if (strcmp(u, "kWh") == 0)       { *dc = "energy"; *sc = STATE_CLASS_TOTAL_INCREASING; *dec = 3; }
    else if (strcmp(u, "m3") == 0)   { *dc = "water";  *sc = STATE_CLASS_TOTAL_INCREASING; *dec = 3; }
    else if (strcmp(u, "kW") == 0)   { *dc = "power";  *sc = STATE_CLASS_MEASUREMENT; *dec = 3; }
    else if (strcmp(u, "V") == 0)    { *dc = "voltage"; *sc = STATE_CLASS_MEASUREMENT; *dec = 1; }
    else if (strcmp(u, "A") == 0)    { *dc = "current"; *sc = STATE_CLASS_MEASUREMENT; *dec = 2; }
    else if (strcmp(u, "VAR") == 0 || strcmp(u, "kVARh") == 0) { *sc = STATE_CLASS_MEASUREMENT; *dec = 3; }
    else if (strcmp(u, "j.") == 0)   { *dec = 0; }
    if (strcmp(e->field, "cos_fi") == 0)     { *dc = "power_factor"; *sc = STATE_CLASS_MEASUREMENT; *dec = 3; }
    else if (strcmp(e->field, "tg_fi") == 0) { *sc = STATE_CLASS_MEASUREMENT; *dec = 3; }
}

// ---------------------------------------------------------------------------
// Lista encji w NVS
// ---------------------------------------------------------------------------
static void cache_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = 0;
    if (nvs_get_blob(h, KEY_ENTS, NULL, &sz) == ESP_OK && sz >= 1 &&
        (sz - 1) % sizeof(cache_rec_t) == 0) {
        uint8_t *buf = malloc(sz);
        if (buf && nvs_get_blob(h, KEY_ENTS, buf, &sz) == ESP_OK && buf[0] == CACHE_VER) {
            int n = (int)((sz - 1) / sizeof(cache_rec_t));
            const cache_rec_t *r = (const cache_rec_t *)(buf + 1);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < n && s_count < ESPH_MAX_ENT; i++) {
                ent_t *e = &s_ent[s_count];
                memset(e, 0, sizeof(*e));
                memcpy(e->id, r[i].id, sizeof(e->id));       e->id[sizeof(e->id) - 1] = 0;
                memcpy(e->field, r[i].field, sizeof(e->field)); e->field[sizeof(e->field) - 1] = 0;
                memcpy(e->unit, r[i].unit, sizeof(e->unit));   e->unit[sizeof(e->unit) - 1] = 0;
                if (!e->id[0] || !e->field[0]) continue;
                // Pole mogla juz dodac ramka odebrana przed wczytaniem listy.
                bool dup = false;
                for (int k = 0; k < s_count; k++)
                    if (strcmp(s_ent[k].id, e->id) == 0 && strcmp(s_ent[k].field, e->field) == 0) { dup = true; break; }
                if (dup) continue;
                s_count++;
                ent_set_key(s_count - 1);
            }
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "Lista encji z NVS: %d", s_count);
        }
        free(buf);
    }
    nvs_close(h);
}

static void cache_save(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count;
    size_t sz = 1 + (size_t)n * sizeof(cache_rec_t);
    uint8_t *buf = malloc(sz);
    if (!buf) { xSemaphoreGive(s_lock); return; }       // sprobujemy pozniej
    buf[0] = CACHE_VER;
    cache_rec_t *r = (cache_rec_t *)(buf + 1);
    for (int i = 0; i < n; i++) {
        memcpy(r[i].id, s_ent[i].id, sizeof(r[i].id));
        memcpy(r[i].field, s_ent[i].field, sizeof(r[i].field));
        memcpy(r[i].unit, s_ent[i].unit, sizeof(r[i].unit));
    }
    s_cache_dirty = false;
    xSemaphoreGive(s_lock);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, KEY_ENTS, buf, sz) != ESP_OK || nvs_commit(h) != ESP_OK)
            mark_cache_dirty();
        nvs_close(h);
    } else {
        mark_cache_dirty();
    }
    free(buf);
}

// Usuwa pola odpiete w panelu. Tylko gdy lista sledzonych jest juz wczytana -
// wczesniej kazde pole wygladaloby na odpiete i HA skasowalby encje.
static void prune(void) {
    if (!history_tracked_ready()) return;
    int removed = 0;
    for (int i = 0; i < s_count; ) {
        // wanted() czyta listy z NVS/historii - bez blokady tabeli
        if (wanted(s_ent[i].id, s_ent[i].field)) { i++; continue; }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memmove(&s_ent[i], &s_ent[i + 1], (size_t)(s_count - i - 1) * sizeof(ent_t));
        s_count--;
        xSemaphoreGive(s_lock);
        removed++;
    }
    if (removed) {
        ESP_LOGI(TAG, "Usunieto %d odpietych pol", removed);
        mark_cache_dirty();
        mark_list_dirty();
    }
}

// ---------------------------------------------------------------------------
// Odpowiedzi na komunikaty HA
// ---------------------------------------------------------------------------
static void send_hello(void) {
    pw_t w = tx_begin();
    char info[48];
    snprintf(info, sizeof(info), "SIHOS17 (esphome v%s)", ESPHOME_VER);
    pw_u32(&w, 1, API_MAJOR);
    pw_u32(&w, 2, API_MINOR);
    pw_str(&w, 3, info);
    pw_str(&w, 4, s_name);
    tx_send(MSG_HELLO_RESP, &w);
}

static void send_device_info(void) {
    const esp_app_desc_t *app = esp_app_get_description();
    char built[40] = "";
    if (app) snprintf(built, sizeof(built), "%s, %s", app->date, app->time);
    pw_t w = tx_begin();
    pw_str(&w, 2, s_name);
    pw_str(&w, 3, s_mac);
    pw_str(&w, 4, ESPHOME_VER);
    pw_str(&w, 5, built);
    pw_str(&w, 6, "SIHOS17");
    pw_str(&w, 8, "smartinhome.sihos17");
    pw_str(&w, 9, app ? app->version : "");
    pw_u32(&w, 10, 80);                 // HA pokaze link do panelu modulu
    pw_str(&w, 12, "smartinhome.pl");
    pw_str(&w, 13, "SIHOS17");
    tx_send(MSG_DEVINFO_RESP, &w);
}

static void send_list(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_count;
    s_list_dirty = false;
    xSemaphoreGive(s_lock);

    for (int i = 0; i < n && s_csock >= 0; i++) {
        ent_t e;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (i >= s_count) { xSemaphoreGive(s_lock); break; }
        s_ent[i].flags |= EF_LISTED;
        e = s_ent[i];
        xSemaphoreGive(s_lock);

        // Nazwa jest tozsamoscia encji w HA (unique_id = mac/0/sensor/<nazwa>),
        // wiec skladamy ja z ID licznika i pola - nie z nazwy nadanej w panelu,
        // ktora mozna zmienic.
        char oid[48], name[48];
        make_object_id(&e, oid, sizeof(oid));
        snprintf(name, sizeof(name), "%s %s", e.id, e.field);
        const char *dc; uint32_t sc, dec;
        ha_class(&e, &dc, &sc, &dec);

        pw_t w = tx_begin();
        pw_str(&w, 1, oid);
        pw_fixed32(&w, 2, e.key);
        pw_str(&w, 3, name);
        pw_str(&w, 6, ha_unit(e.unit));
        pw_u32(&w, 7, dec);
        pw_str(&w, 9, dc);
        pw_u32(&w, 10, sc);
        if (!tx_send(MSG_LIST_SENSOR, &w)) return;
    }
    if (s_csock < 0) return;
    tx_empty(MSG_LIST_DONE);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count > n) mark_list_dirty();       // doszly w trakcie wysylania
    xSemaphoreGive(s_lock);
}

// force: pierwsza paczka po subskrypcji - wszystkie stany od razu.
static void send_states(bool force) {
    if (!s_subscribed) return;
    uint32_t now = now_ms();
    for (int i = 0; s_csock >= 0; i++) {
        uint32_t key; float val; bool has;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (i >= s_count) { xSemaphoreGive(s_lock); break; }
        ent_t *e = &s_ent[i];
        bool go = (e->flags & EF_LISTED) && (e->flags & EF_DIRTY) &&
                  (force || (uint32_t)(now - e->sent_ms) >= MIN_STATE_MS);
        if (go) {
            e->flags &= (uint8_t)~EF_DIRTY;
            e->sent_ms = now;
        }
        key = e->key; val = e->value; has = (e->flags & EF_VALUE) != 0;
        xSemaphoreGive(s_lock);
        if (!go) continue;

        pw_t w = tx_begin();
        pw_fixed32(&w, 1, key);
        if (has) pw_float(&w, 2, val);
        pw_bool(&w, 3, !has);
        if (!tx_send(MSG_SENSOR_STATE, &w)) return;
        s_states_sent++;
    }
}

// client_info z HelloRequest (pole 1) - tylko do podgladu w panelu.
static void parse_hello(const uint8_t *p, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint32_t tag = 0, sh = 0;
        while (i < n && sh < 35) { uint8_t b = p[i++]; tag |= (uint32_t)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) break; }
        uint32_t f = tag >> 3, wt = tag & 7;
        if (wt == 0) { while (i < n && (p[i++] & 0x80)) {} continue; }
        if (wt != 2) return;
        uint32_t len = 0; sh = 0;
        while (i < n && sh < 35) { uint8_t b = p[i++]; len |= (uint32_t)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) break; }
        if (len > n - i) return;
        if (f == 1) {
            uint32_t m = len < sizeof(s_client_info) - 1 ? len : sizeof(s_client_info) - 1;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (uint32_t k = 0; k < m; k++) {
                char c = (char)p[i + k];
                s_client_info[k] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : ' ';
            }
            s_client_info[m] = 0;
            xSemaphoreGive(s_lock);
        }
        i += len;
    }
}

static void handle(uint32_t type, const uint8_t *p, uint32_t n) {
    // Jak w ESPHome: bez powitania nie ma danych urzadzenia ani encji.
    if (s_cst == C_HELLO_WAIT && (type == MSG_DEVINFO_REQ || type == MSG_LIST_REQ ||
                                  type == MSG_SUB_STATES)) {
        client_close("zapytanie przed powitaniem");
        return;
    }
    switch (type) {
        case MSG_HELLO_REQ:
            parse_hello(p, n);
            send_hello();
            if (s_cst == C_HELLO_WAIT) s_cst = C_READY;
            break;
        case MSG_CONNECT_REQ:
            // Bez hasla. Nowy HA wysyla to razem z powitaniem i nie czeka na
            // odpowiedz, starszy czeka - pusta odpowiedz = haslo poprawne.
            tx_empty(MSG_CONNECT_RESP);
            break;
        case MSG_DISCONNECT_REQ:
            tx_empty(MSG_DISCONNECT_RESP);
            client_close("HA zakonczyl polaczenie");
            break;
        case MSG_DISCONNECT_RESP:
            client_close("odswiezenie listy encji");
            break;
        case MSG_PING_REQ:
            tx_empty(MSG_PING_RESP);
            break;
        case MSG_PING_RESP:
            break;
        case MSG_DEVINFO_REQ:
            send_device_info();
            break;
        case MSG_LIST_REQ:
            send_list();
            break;
        case MSG_SUB_STATES:
            s_subscribed = true;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_count; i++) s_ent[i].flags |= EF_DIRTY;
            xSemaphoreGive(s_lock);
            send_states(true);
            break;
        default:
            // Subskrypcje uslug, stanow HA, logow itp. - nie dotycza czujnikow,
            // ESPHome tez na nie nie odpowiada, gdy nie ma czego wyslac.
            break;
    }
}

// Dekoduje varint z bufora; 0 = za malo danych, -1 = blad.
static int rd_varint(const uint8_t *p, uint32_t n, uint32_t *out) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n && i < 5; i++) {
        v |= (uint32_t)(p[i] & 0x7F) << (7 * i);
        if (!(p[i] & 0x80)) { *out = v; return (int)i + 1; }
    }
    return n >= 5 ? -1 : 0;
}

static void consume(uint32_t k) {
    memmove(s_rx, s_rx + k, s_rx_len - k);
    s_rx_len = (uint16_t)(s_rx_len - k);
}

static void client_read(void) {
    int r = recv(s_csock, s_rx + s_rx_len, RX_CAP - s_rx_len, 0);
    if (r <= 0) { client_close(r == 0 ? "HA zamknal polaczenie" : "blad odczytu"); return; }
    s_rx_len = (uint16_t)(s_rx_len + r);
    s_last_rx_ms = now_ms();
    s_ping_sent = false;

    while (s_rx_len > 0 && s_csock >= 0) {
        if (s_skip) {
            uint32_t k = s_skip < s_rx_len ? s_skip : s_rx_len;
            consume(k); s_skip -= k;
            continue;
        }
        if (s_rx[0] != 0x00) {
            // 0x01 = klient chce szyfrowania Noise, ktorego nie mamy
            client_close(s_rx[0] == 0x01 ? "HA wymaga szyfrowania - usun klucz w integracji"
                                         : "nieznany format ramki");
            return;
        }
        uint32_t len = 0, type = 0;
        int a = rd_varint(s_rx + 1, s_rx_len - 1, &len);
        if (a < 0) { client_close("bledny naglowek"); return; }
        if (a == 0) break;
        int b = rd_varint(s_rx + 1 + a, s_rx_len - 1 - (uint32_t)a, &type);
        if (b < 0 || len > 0xFFFF || type > 0xFFFF) { client_close("bledny naglowek"); return; }
        if (b == 0) break;
        uint32_t hl = 1 + (uint32_t)a + (uint32_t)b;
        if (len > RX_CAP - hl) {
            // Nie potrzebujemy duzych komunikatow - pomijamy tresc bajt po bajcie.
            consume(hl);
            s_skip = len;
            continue;
        }
        if (s_rx_len < hl + len) break;
        handle(type, s_rx + hl, len);
        if (s_csock < 0) return;
        consume(hl + len);
    }
}

static void client_accept(void) {
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int s = accept(s_lsock, (struct sockaddr *)&a, &al);
    if (s < 0) return;
    // Jedno polaczenie. Nowe zastepuje stare - HA po zaniku sieci laczy sie
    // ponownie, zanim my zauwazymy, ze poprzednie gniazdo jest martwe.
    if (s_csock >= 0) client_close("zastapione nowym polaczeniem");
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    s_csock = s;
    s_cst = C_HELLO_WAIT;
    s_accept_ms = s_last_rx_ms = now_ms();
    s_rx_len = 0; s_skip = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    inet_ntoa_r(a.sin_addr, s_peer, sizeof(s_peer));
    s_client_info[0] = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Polaczenie z %s", s_peer);
}

static void tick(void) {
    uint32_t now = now_ms();

    if (s_csock >= 0) {
        if (s_cst == C_HELLO_WAIT && now - s_accept_ms > HELLO_TIMEOUT_MS) {
            client_close("brak powitania");
        } else if (s_cst == C_CLOSING && now - s_closing_ms > CLOSING_MS) {
            client_close("odswiezenie listy encji");
        } else if (now - s_last_rx_ms > DEAD_IDLE_MS) {
            client_close("HA przestal odpowiadac");
        } else if (now - s_last_rx_ms > PING_IDLE_MS && !s_ping_sent) {
            s_ping_sent = tx_empty(MSG_PING_REQ);
        }
    }
    if (s_csock >= 0 && s_cst == C_READY) {
        send_states(false);
        // Zmiana listy pol: prosimy HA o ponowne polaczenie. HA wraca po 5 s
        // i pobiera nowa liste - ESPHome nie ma komunikatu "lista sie zmienila".
        if (s_csock >= 0 && s_list_dirty && s_subscribed &&
            now - s_list_dirty_ms >= RELIST_DEBOUNCE_MS &&
            (s_last_relist_ms == 0 || now - s_last_relist_ms >= RELIST_GAP_MS)) {
            s_last_relist_ms = now;
            if (tx_empty(MSG_DISCONNECT_REQ)) {
                s_cst = C_CLOSING;
                s_closing_ms = now;
                ESP_LOGI(TAG, "Zmiana listy pol - prosze HA o ponowne polaczenie");
            }
        }
    }
    if (now - s_last_prune_ms >= PRUNE_EVERY_MS) {
        s_last_prune_ms = now;
        prune();
    }
    if (s_cache_dirty && now - s_cache_dirty_ms >= CACHE_DELAY_MS) cache_save();
    s_stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

static int listen_open(void) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(ESPHOME_API_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 1) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static void api_task(void *arg) {
    (void)arg;
    cache_load();
    s_lsock = listen_open();
    if (s_lsock < 0) ESP_LOGE(TAG, "Nie mozna otworzyc portu %d", ESPHOME_API_PORT);
    else ESP_LOGI(TAG, "Serwer API ESPHome na porcie %d (%s)", ESPHOME_API_PORT, s_name);

    while (s_run) {
        if (s_lsock < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            s_lsock = listen_open();
            continue;
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s_lsock, &rf);
        int mx = s_lsock;
        if (s_csock >= 0) { FD_SET(s_csock, &rf); if (s_csock > mx) mx = s_csock; }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 250000 };
        int n = select(mx + 1, &rf, NULL, NULL, &tv);
        if (n > 0) {
            if (FD_ISSET(s_lsock, &rf)) client_accept();
            if (s_csock >= 0 && FD_ISSET(s_csock, &rf)) client_read();
        } else if (n < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        tick();
    }

    if (s_csock >= 0) {
        tx_empty(MSG_DISCONNECT_REQ);
        client_close("serwer zatrzymany");
    }
    if (s_lsock >= 0) { close(s_lsock); s_lsock = -1; }
    if (s_cache_dirty) cache_save();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_ent); s_ent = NULL; s_count = 0;
    free(s_rx);  s_rx = NULL;
    free(s_tx);  s_tx = NULL;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "Serwer API ESPHome zatrzymany");
    s_task = NULL;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
bool esphome_api_enabled(void) {
    if (s_enabled < 0) {
        uint8_t v = 0;
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, KEY_EN, &v);
            nvs_close(h);
        }
        s_enabled = v ? 1 : 0;
    }
    return s_enabled == 1;
}

void esphome_api_set_enabled(bool on) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_EN, on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    s_enabled = on ? 1 : 0;
}

void esphome_api_start(void) {
    if (!esphome_api_enabled() || s_task) return;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_lock || !s_done) return;

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_name, sizeof(s_name), "sihos17-%02x%02x%02x", mac[3], mac[4], mac[5]);

    ent_t *ent = calloc(ESPH_MAX_ENT, sizeof(ent_t));
    uint8_t *rx = malloc(RX_CAP), *tx = malloc(TX_CAP);
    if (!ent || !rx || !tx) {
        free(ent); free(rx); free(tx);
        ESP_LOGE(TAG, "Brak pamieci na serwer API ESPHome");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ent = ent; s_rx = rx; s_tx = tx;
    s_count = 0; s_skipped = 0;
    s_list_dirty = false; s_cache_dirty = false;
    s_last_relist_ms = 0; s_last_prune_ms = now_ms();
    xSemaphoreGive(s_lock);

    xSemaphoreTake(s_done, 0);
    s_run = true;
    if (xTaskCreate(api_task, "esphome_api", TASK_STACK, NULL, 3, &s_task) != pdPASS) {
        s_run = false;
        s_task = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_ent); s_ent = NULL; free(s_rx); s_rx = NULL; free(s_tx); s_tx = NULL;
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "Brak pamieci na zadanie serwera API ESPHome");
    }
}

void esphome_api_stop(void) {
    if (!s_task) return;
    s_run = false;
    // Zadanie konczy sie najpozniej po jednym obiegu petli (250 ms) i wyslaniu
    // pozegnania do HA (do 3 s przy zablokowanym gniezdzie).
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(5000)) != pdTRUE)
        ESP_LOGW(TAG, "Zadanie serwera nie zakonczylo sie w 5 s");
}

void esphome_api_field(const char *id_hex, const char *field,
                       double value, const char *unit) {
    if (!s_ent || !s_lock || !id_hex || !field) return;
    if (!wanted(id_hex, field)) return;
    if (!unit) unit = "";

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ent) { xSemaphoreGive(s_lock); return; }
    int idx = -1;
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_ent[i].id, id_hex) == 0 && strcmp(s_ent[i].field, field) == 0) { idx = i; break; }
    if (idx < 0) {
        if (s_count >= ESPH_MAX_ENT) { s_skipped++; xSemaphoreGive(s_lock); return; }
        idx = s_count;
        ent_t *e = &s_ent[idx];
        memset(e, 0, sizeof(*e));
        snprintf(e->id, sizeof(e->id), "%s", id_hex);
        snprintf(e->field, sizeof(e->field), "%s", field);
        snprintf(e->unit, sizeof(e->unit), "%s", unit);
        s_count++;
        ent_set_key(idx);
        mark_list_dirty();
        mark_cache_dirty();
    } else if (strcmp(s_ent[idx].unit, unit) != 0) {
        snprintf(s_ent[idx].unit, sizeof(s_ent[idx].unit), "%s", unit);
        mark_list_dirty();
        mark_cache_dirty();
    }
    s_ent[idx].value = (float)value;
    s_ent[idx].flags |= EF_VALUE | EF_DIRTY;
    xSemaphoreGive(s_lock);
}

int esphome_api_status_json(char *buf, int cap) {
    bool en = esphome_api_enabled();
    bool run = s_task != NULL;
    int ents = 0;
    uint32_t skipped = 0, stack_free = run ? s_stack_free : 0;
    char peer[16] = "", info[40] = "";
    bool ha = false;
    if (s_lock && run) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        ents = s_count;
        skipped = s_skipped;
        ha = s_csock >= 0 && s_subscribed;
        if (s_csock >= 0) {
            snprintf(peer, sizeof(peer), "%s", s_peer);
            snprintf(info, sizeof(info), "%s", s_client_info);
        }
        xSemaphoreGive(s_lock);
    }
    // Sterta zajeta przez dzialajacy serwer: stos zadania i bufory. Bez
    // chwilowych buforow TCP przy wysylaniu listy encji.
    uint32_t ram = run ? (uint32_t)(TASK_STACK + ESPH_MAX_ENT * sizeof(ent_t) + RX_CAP + TX_CAP) : 0;
    return snprintf(buf, cap,
        "{\"enabled\":%s,\"running\":%s,\"port\":%d,\"name\":\"%s\","
        "\"ha\":%s,\"client\":\"%s\",\"client_info\":\"%s\","
        "\"entities\":%d,\"max\":%d,\"skipped\":%u,\"states_sent\":%u,"
        "\"ram\":%u,\"stack_free\":%u,\"last\":\"%s\",\"api\":\"%d.%d\",\"esphome\":\"%s\"}",
        en ? "true" : "false", run ? "true" : "false", ESPHOME_API_PORT, s_name,
        ha ? "true" : "false", peer, info,
        ents, ESPH_MAX_ENT, (unsigned)skipped, (unsigned)s_states_sent,
        (unsigned)ram, (unsigned)stack_free, s_last_reason, API_MAJOR, API_MINOR, ESPHOME_VER);
}
