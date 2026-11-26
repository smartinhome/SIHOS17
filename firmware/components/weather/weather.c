#include "weather.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "wifi_manager.h"

static const char *TAG = "POGODA";

#define NVS_NS        "weather"
#define FIRST_S       30          // pierwsze pobranie po starcie
#define RETRY_S       300         // ponowienie po bledzie
#define WAIT_S        60          // brak WiFi albo czasu - sprobuj za minute
#define MIN_HEAP      50000       // najwiekszy wolny blok potrzebny na TLS
#define RESP_MAX      4096        // odpowiedz ma ok. 1 KB

static SemaphoreHandle_t s_mx;
static weather_data_t    s_data;
static bool     s_en = false;           // domyslnie wylaczona
static char     s_city[48] = "";
static int32_t  s_lat_e5 = 0, s_lon_e5 = 0;
static int      s_interval = 30;        // minuty
static uint32_t s_ok_ts = 0;            // ostatnie udane pobranie (czas modulu)
static char     s_err[48] = "";
static volatile bool s_busy = false;
static esp_timer_handle_t s_tm;

// ---------------------------------------------------------------- NVS
static void cfg_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8 = 0;
    if (nvs_get_u8(h, "en", &u8) == ESP_OK) s_en = u8 != 0;
    if (nvs_get_u8(h, "int", &u8) == ESP_OK && u8 >= 15 && u8 <= 120) s_interval = u8;
    nvs_get_i32(h, "lat", &s_lat_e5);
    nvs_get_i32(h, "lon", &s_lon_e5);
    size_t n = sizeof(s_city);
    if (nvs_get_str(h, "city", s_city, &n) != ESP_OK) s_city[0] = 0;
    nvs_close(h);
}

static void cfg_save(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "en", s_en ? 1 : 0);
    nvs_set_u8(h, "int", (uint8_t)s_interval);
    nvs_set_i32(h, "lat", s_lat_e5);
    nvs_set_i32(h, "lon", s_lon_e5);
    nvs_set_str(h, "city", s_city);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------- parsowanie
static double jnum(const cJSON *o, const char *k, double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

// "2026-11-26T13:45" albo "2026-11-26" -> unix (czas lokalny) i dzien tygodnia
static uint32_t parse_time(const char *s, int *wday) {
    struct tm tm = {0};
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0;
    int n = sscanf(s ? s : "", "%d-%d-%dT%d:%d", &y, &mo, &d, &hh, &mi);
    if (n < 3) return 0;
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = n >= 5 ? hh : 12; tm.tm_min = n >= 5 ? mi : 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if (wday) *wday = tm.tm_wday;
    return (uint32_t)t;
}

static bool parse(const char *body, weather_data_t *out) {
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    bool ok = false;
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (cJSON_IsObject(cur)) {
        memset(out, 0, sizeof(*out));
        const cJSON *tm = cJSON_GetObjectItemCaseSensitive(cur, "time");
        out->ts       = parse_time(cJSON_IsString(tm) ? tm->valuestring : NULL, NULL);
        out->temp     = (float)jnum(cur, "temperature_2m", NAN);
        out->apparent = (float)jnum(cur, "apparent_temperature", NAN);
        out->humidity = (uint8_t)jnum(cur, "relative_humidity_2m", 0);
        out->precip   = (float)jnum(cur, "precipitation", 0);
        out->wind     = (float)jnum(cur, "wind_speed_10m", 0);
        out->pressure = (float)jnum(cur, "pressure_msl", 0);
        out->code     = (uint8_t)jnum(cur, "weather_code", 3);
        if (cJSON_IsObject(daily)) {
            const cJSON *t  = cJSON_GetObjectItemCaseSensitive(daily, "time");
            const cJSON *c  = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
            const cJSON *mx = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
            const cJSON *mn = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
            int n = cJSON_GetArraySize(t);
            if (n > WEATHER_DAYS) n = WEATHER_DAYS;
            for (int i = 0; i < n; i++) {
                const cJSON *ti = cJSON_GetArrayItem(t, i);
                int wd = 0;
                parse_time(cJSON_IsString(ti) ? ti->valuestring : NULL, &wd);
                const cJSON *ci = cJSON_GetArrayItem(c, i), *xi = cJSON_GetArrayItem(mx, i), *ni = cJSON_GetArrayItem(mn, i);
                out->day[i].wday = (uint8_t)wd;
                out->day[i].code = cJSON_IsNumber(ci) ? (uint8_t)ci->valueint : 3;
                out->day[i].tmax = cJSON_IsNumber(xi) ? (float)xi->valuedouble : NAN;
                out->day[i].tmin = cJSON_IsNumber(ni) ? (float)ni->valuedouble : NAN;
            }
            out->ndays = n;
        }
        out->valid = !isnan(out->temp);
        ok = out->valid;
    }
    cJSON_Delete(root);
    return ok;
}

// ---------------------------------------------------------------- pobieranie
static void set_err(const char *e) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    snprintf(s_err, sizeof(s_err), "%s", e ? e : "");
    xSemaphoreGive(s_mx);
}

static bool fetch(void) {
    char url[384];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,apparent_temperature,relative_humidity_2m,precipitation,"
             "weather_code,wind_speed_10m,pressure_msl"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&timezone=Europe%%2FWarsaw&forecast_days=%d",
             s_lat_e5 / 1e5, s_lon_e5 / 1e5, WEATHER_DAYS);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { set_err("brak pamieci"); return false; }
    bool ok = false;
    char *body = malloc(RESP_MAX + 1);
    if (!body) { set_err("brak pamieci"); esp_http_client_cleanup(c); return false; }
    if (esp_http_client_open(c, 0) != ESP_OK) {
        set_err("brak polaczenia");
    } else {
        esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        int n = 0, r;
        while (n < RESP_MAX && (r = esp_http_client_read(c, body + n, RESP_MAX - n)) > 0) n += r;
        body[n] = 0;
        if (st != 200) {
            char e[32]; snprintf(e, sizeof(e), "HTTP %d", st); set_err(e);
        } else {
            weather_data_t d;
            if (parse(body, &d)) {
                xSemaphoreTake(s_mx, portMAX_DELAY);
                s_data = d;
                s_ok_ts = (uint32_t)time(NULL);
                s_err[0] = 0;
                xSemaphoreGive(s_mx);
                ok = true;
                ESP_LOGI(TAG, "%s: %.1f°C, kod %d (%d B)", s_city, d.temp, d.code, n);
            } else {
                set_err("zla odpowiedz");
            }
        }
    }
    free(body);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static void schedule(uint32_t s) {
    esp_timer_stop(s_tm);
    esp_timer_start_once(s_tm, (uint64_t)s * 1000000ULL);
}

static void fetch_task(void *arg) {
    (void)arg;
    uint32_t next = WAIT_S;
    if (!s_en || (s_lat_e5 == 0 && s_lon_e5 == 0)) {
        next = 0;                                   // wylaczona - nie planuj
    } else if (wifi_manager_get_state() != WIFI_STATE_CONNECTED || time(NULL) < 1700000000) {
        next = WAIT_S;
    } else if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < MIN_HEAP) {
        set_err("za malo RAM");                    // np. trwa aktualizacja
        next = RETRY_S;
    } else {
        next = fetch() ? (uint32_t)s_interval * 60 : RETRY_S;
    }
    if (next) schedule(next);
    s_busy = false;
    vTaskDelete(NULL);
}

// Zadanie tylko na czas pobrania: TLS potrzebuje stosu i sterty jedynie wtedy.
static void timer_cb(void *arg) {
    (void)arg;
    if (s_busy) return;
    s_busy = true;
    if (xTaskCreate(fetch_task, "pogoda", 8192, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        schedule(RETRY_S);
    }
}

// ---------------------------------------------------------------- API
void weather_init(void) {
    s_mx = xSemaphoreCreateMutex();
    cfg_load();
    const esp_timer_create_args_t ta = { .callback = timer_cb, .name = "pogoda" };
    if (esp_timer_create(&ta, &s_tm) != ESP_OK) return;
    if (s_en) schedule(FIRST_S);
    ESP_LOGI(TAG, "Pogoda %s%s%s", s_en ? "wlaczona: " : "wylaczona", s_en ? s_city : "", "");
}

bool weather_enabled(void) { return s_en; }
const char *weather_city(void) { return s_city; }

bool weather_get(weather_data_t *out) {
    if (!s_mx || !out) return false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_mx);
    return out->valid;
}

void weather_configure(bool enabled, const char *city, float lat, float lon, int interval_min) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool moved = false;
    if (city) {
        int32_t la = (int32_t)lroundf(lat * 1e5f), lo = (int32_t)lroundf(lon * 1e5f);
        moved = la != s_lat_e5 || lo != s_lon_e5;
        snprintf(s_city, sizeof(s_city), "%s", city);
        s_lat_e5 = la; s_lon_e5 = lo;
    }
    if (interval_min >= 15 && interval_min <= 120) s_interval = interval_min;
    s_en = enabled;
    if (moved) { memset(&s_data, 0, sizeof(s_data)); s_ok_ts = 0; s_err[0] = 0; }   // stare dane z innego miejsca
    xSemaphoreGive(s_mx);
    cfg_save();
    if (s_tm) {
        esp_timer_stop(s_tm);
        if (s_en) schedule(2);
    }
    ESP_LOGI(TAG, "Ustawienia: %s, %s, co %d min", s_en ? "wlaczona" : "wylaczona", s_city, s_interval);
}

void weather_refresh_now(void) {
    if (s_tm && s_en) schedule(1);
}

int weather_status_json(char *buf, int cap) {
    xSemaphoreTake(s_mx, portMAX_DELAY);
    char city[64];
    // nazwa miejscowosci pochodzi od uzytkownika - bez znakow lamiacych JSON
    int j = 0;
    for (int i = 0; s_city[i] && j < (int)sizeof(city) - 1; i++)
        if (s_city[i] != '"' && s_city[i] != '\\' && (unsigned char)s_city[i] >= 0x20) city[j++] = s_city[i];
    city[j] = 0;
    int n = snprintf(buf, cap,
        "{\"enabled\":%s,\"city\":\"%s\",\"lat\":%.5f,\"lon\":%.5f,\"interval\":%d,"
        "\"ok_ts\":%u,\"err\":\"%s\",\"busy\":%s",
        s_en ? "true" : "false", city, s_lat_e5 / 1e5, s_lon_e5 / 1e5, s_interval,
        (unsigned)s_ok_ts, s_err, s_busy ? "true" : "false");
    if (s_data.valid)
        n += snprintf(buf + n, cap - n, ",\"temp\":%.1f,\"code\":%d,\"ts\":%u",
                      s_data.temp, s_data.code, (unsigned)s_data.ts);
    n += snprintf(buf + n, cap - n, "}");
    xSemaphoreGive(s_mx);
    return n;
}
