#include "history.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "HISTORY";
#define MAX_HIST_METERS 16

// Stan historii pojedynczego licznika
typedef struct {
    char     id[28];          // klucz: "id" lub "id:pole" (np. 56989134:moc_kw)
    int      kind;            // 1=woda, 2=prad, 3=gaz
    int      cumulative;      // 1=kumulacyjne (roznice), 0=chwilowe (wartosc)
    bool     used;
    float    last_total;
    uint32_t last_ts;
    hist_bucket_t hours[HIST_HOURS];   int n_hours;
    hist_bucket_t days[HIST_DAYS];     int n_days;
    hist_bucket_t months[HIST_MONTHS]; int n_months;
    hist_bucket_t years[HIST_YEARS];   int n_years;
    hist_rt_t     rt[HIST_REALTIME];   int n_rt;
} meter_hist_t;

static meter_hist_t s_meters[MAX_HIST_METERS];
// Lista sledzonych licznikow (pokazywane w Historii). Reszta (sasiedzi) ukryta.
#define MAX_TRACKED 24
static char s_tracked[MAX_TRACKED][28];
static int  s_tracked_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_fs_ok = false;

// ---------- Pomocnicze: zaokraglenia czasu ----------
static uint32_t floor_hour(uint32_t t)  { return t - (t % 3600); }
static uint32_t floor_day(uint32_t t) {
    time_t tt = t; struct tm tm; localtime_r(&tt, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    return (uint32_t)mktime(&tm);
}
static uint32_t floor_month(uint32_t t) {
    time_t tt = t; struct tm tm; localtime_r(&tt, &tm);
    tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    return (uint32_t)mktime(&tm);
}
static uint32_t floor_year(uint32_t t) {
    time_t tt = t; struct tm tm; localtime_r(&tt, &tm);
    tm.tm_mon = 0; tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    return (uint32_t)mktime(&tm);
}

// Dodaj/aktualizuj kubelek w serii kolowej. Jesli ostatni ma ten sam ts -> aktualizuj total.
// Inaczej dodaj nowy (przesun gdy pelny).
static void series_update(hist_bucket_t *arr, int *n, int cap, uint32_t bts, float total) {
    if (*n > 0 && arr[*n - 1].ts == bts) {
        arr[*n - 1].total = total;   // ten sam okres - zaktualizuj koncowy total
        return;
    }
    if (*n < cap) {
        arr[*n].ts = bts; arr[*n].total = total; (*n)++;
    } else {
        memmove(arr, arr + 1, (cap - 1) * sizeof(hist_bucket_t));
        arr[cap - 1].ts = bts; arr[cap - 1].total = total;
    }
}

static void rt_update(meter_hist_t *m, uint32_t ts, float total) {
    if (m->n_rt < HIST_REALTIME) {
        m->rt[m->n_rt].ts = ts; m->rt[m->n_rt].total = total; m->n_rt++;
    } else {
        memmove(m->rt, m->rt + 1, (HIST_REALTIME - 1) * sizeof(hist_rt_t));
        m->rt[HIST_REALTIME - 1].ts = ts; m->rt[HIST_REALTIME - 1].total = total;
    }
}

// ---------- SPIFFS zapis/odczyt ----------
static void meter_path(const char *id, char *out, int cap) {
    // zamien ':' na '_' (SPIFFS nie lubi dwukropka w nazwie)
    char safe[28];
    int j = 0;
    for (int i = 0; id[i] && j < (int)sizeof(safe) - 1; i++)
        safe[j++] = (id[i] == ':') ? '_' : id[i];
    safe[j] = 0;
    snprintf(out, cap, "/spiffs/h_%s.bin", safe);
}

static void save_meter(meter_hist_t *m) {
    if (!s_fs_ok) return;
    char path[48]; meter_path(m->id, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "nie moge zapisac %s", path); return; }
    // Zapis: kind, last_total, last_ts, potem kazda seria (n + dane)
    fwrite(&m->kind, sizeof(int), 1, f);
    fwrite(&m->last_total, sizeof(float), 1, f);
    fwrite(&m->last_ts, sizeof(uint32_t), 1, f);
    fwrite(&m->n_hours, sizeof(int), 1, f);  fwrite(m->hours, sizeof(hist_bucket_t), m->n_hours, f);
    fwrite(&m->n_days, sizeof(int), 1, f);   fwrite(m->days, sizeof(hist_bucket_t), m->n_days, f);
    fwrite(&m->n_months, sizeof(int), 1, f); fwrite(m->months, sizeof(hist_bucket_t), m->n_months, f);
    fwrite(&m->n_years, sizeof(int), 1, f);  fwrite(m->years, sizeof(hist_bucket_t), m->n_years, f);
    fclose(f);
}

static bool load_meter(meter_hist_t *m, const char *id) {
    if (!s_fs_ok) return false;
    char path[48]; meter_path(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    memset(m, 0, sizeof(*m));
    strncpy(m->id, id, sizeof(m->id) - 1);
    m->used = true;
    size_t r = 0;
    r += fread(&m->kind, sizeof(int), 1, f);
    r += fread(&m->last_total, sizeof(float), 1, f);
    r += fread(&m->last_ts, sizeof(uint32_t), 1, f);
    fread(&m->n_hours, sizeof(int), 1, f);
    if (m->n_hours < 0 || m->n_hours > HIST_HOURS) m->n_hours = 0;
    fread(m->hours, sizeof(hist_bucket_t), m->n_hours, f);
    fread(&m->n_days, sizeof(int), 1, f);
    if (m->n_days < 0 || m->n_days > HIST_DAYS) m->n_days = 0;
    fread(m->days, sizeof(hist_bucket_t), m->n_days, f);
    fread(&m->n_months, sizeof(int), 1, f);
    if (m->n_months < 0 || m->n_months > HIST_MONTHS) m->n_months = 0;
    fread(m->months, sizeof(hist_bucket_t), m->n_months, f);
    fread(&m->n_years, sizeof(int), 1, f);
    if (m->n_years < 0 || m->n_years > HIST_YEARS) m->n_years = 0;
    fread(m->years, sizeof(hist_bucket_t), m->n_years, f);
    fclose(f);
    return true;
}

// znajdz lub zaalokuj slot licznika
static meter_hist_t *find_meter(const char *id, bool create) {
    for (int i = 0; i < MAX_HIST_METERS; i++)
        if (s_meters[i].used && strcmp(s_meters[i].id, id) == 0) return &s_meters[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_HIST_METERS; i++)
        if (!s_meters[i].used) {
            memset(&s_meters[i], 0, sizeof(meter_hist_t));
            strncpy(s_meters[i].id, id, sizeof(s_meters[i].id) - 1);
            s_meters[i].used = true;
            return &s_meters[i];
        }
    return NULL;
}

// ---------- Inicjalizacja ----------
// ---------- Sledzenie licznikow ----------
#define TRACKED_PATH "/spiffs/tracked.txt"

static void tracked_save(void) {
    if (!s_fs_ok) return;
    FILE *f = fopen(TRACKED_PATH, "wb");
    if (!f) return;
    for (int i = 0; i < s_tracked_count; i++)
        fprintf(f, "%s\n", s_tracked[i]);
    fclose(f);
}

static void tracked_load(void) {
    s_tracked_count = 0;
    if (!s_fs_ok) return;
    FILE *f = fopen(TRACKED_PATH, "rb");
    if (!f) return;
    char line[36];
    while (fgets(line, sizeof(line), f) && s_tracked_count < MAX_TRACKED) {
        // usun biale znaki z konca
        int len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = 0;
        if (len > 0) {
            strncpy(s_tracked[s_tracked_count], line, sizeof(s_tracked[0]) - 1);
            s_tracked_count++;
        }
    }
    fclose(f);
}

bool history_is_tracked(const char *id_hex) {
    if (!id_hex) return false;
    for (int i = 0; i < s_tracked_count; i++)
        if (strcasecmp(s_tracked[i], id_hex) == 0) return true;
    return false;
}

void history_set_tracked(const char *id_hex, bool tracked) {
    if (!id_hex) return;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool exists = false;
    int idx = -1;
    for (int i = 0; i < s_tracked_count; i++)
        if (strcasecmp(s_tracked[i], id_hex) == 0) { exists = true; idx = i; break; }

    if (tracked && !exists && s_tracked_count < MAX_TRACKED) {
        strncpy(s_tracked[s_tracked_count], id_hex, sizeof(s_tracked[0]) - 1);
        s_tracked[s_tracked_count][sizeof(s_tracked[0]) - 1] = 0;
        s_tracked_count++;
        tracked_save();
    } else if (!tracked && exists) {
        // usun przesuwajac reszte
        for (int i = idx; i < s_tracked_count - 1; i++)
            memcpy(s_tracked[i], s_tracked[i+1], sizeof(s_tracked[0]));
        s_tracked_count--;
        tracked_save();
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
}

void history_init(void) {
    s_mutex = xSemaphoreCreateMutex();

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "littlefs",   // partycja z subtype spiffs
        .max_files = 6,
        .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount blad: %s", esp_err_to_name(ret));
        s_fs_ok = false;
        return;
    }
    s_fs_ok = true;
    size_t total = 0, used = 0;
    esp_spiffs_info("littlefs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS zamontowany: %u/%u B", (unsigned)used, (unsigned)total);
    tracked_load();
    ESP_LOGI(TAG, "Sledzonych licznikow: %d", s_tracked_count);

    // Wczytaj historie wszystkich plikow h_*.bin
    // (proste: probujemy wczytac dla wszystkich slotow przez liste katalogu)
    // SPIFFS nie ma katalogow - listujemy przez opendir na /spiffs
    // Wczytanie nastapi leniwie przy pierwszym odczycie/zapisie kazdego licznika.
}

// znajdz w RAM, lub wczytaj z dysku WPROST do slotu (bez kopii na stosie!)
static meter_hist_t *get_or_load(const char *id, bool create_if_missing) {
    meter_hist_t *m = find_meter(id, false);
    if (m) return m;
    // nie ma w RAM - zaalokuj slot i wczytaj z dysku wprost do niego
    meter_hist_t *slot = find_meter(id, true);
    if (!slot) return NULL;
    if (load_meter(slot, id)) {
        return slot;   // wczytano z dysku
    }
    // brak pliku
    if (create_if_missing) {
        // slot juz zainicjowany przez find_meter (memset+id+used)
        return slot;
    }
    // nie tworzymy - zwolnij slot
    slot->used = false;
    return NULL;
}

// ---------- Glowne wejscie: nowy odczyt ----------
void history_on_reading(const char *id_hex, double total, int kind, uint32_t ts_unix) {
    if (!id_hex || ts_unix < 1700000000) return;  // wymaga zsynchronizowanego czasu
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    meter_hist_t *m = get_or_load(id_hex, true);
    if (!m) { if (s_mutex) xSemaphoreGive(s_mutex); return; }

    float ft = (float)total;
    m->kind = kind;
    m->last_total = ft;
    m->last_ts = ts_unix;

    // Czy zamknela sie godzina? (porownaj z ostatnim kubelkiem godzinowym)
    uint32_t bh = floor_hour(ts_unix);
    bool hour_closed = (m->n_hours > 0 && m->hours[m->n_hours-1].ts != bh);

    series_update(m->hours,  &m->n_hours,  HIST_HOURS,  bh, ft);
    series_update(m->days,   &m->n_days,   HIST_DAYS,   floor_day(ts_unix),   ft);
    series_update(m->months, &m->n_months, HIST_MONTHS, floor_month(ts_unix), ft);
    series_update(m->years,  &m->n_years,  HIST_YEARS,  floor_year(ts_unix),  ft);
    rt_update(m, ts_unix, ft);

    // Zapis do flash tylko gdy zamknela sie godzina (oszczedzamy zapisy)
    if (hour_closed) save_meter(m);

    if (s_mutex) xSemaphoreGive(s_mutex);
}

// Per-pole: klucz "id:pole". Zbiera TYLKO gdy pole sledzone (Etap A).
void history_on_field(const char *id_hex, const char *field, double value,
                      int kind, int cumulative, uint32_t ts_unix) {
    if (!id_hex || !field || ts_unix < 1700000000) return;
    char key[28];
    snprintf(key, sizeof(key), "%s:%s", id_hex, field);

    // ETAP A: zbieramy tylko sledzone pola (sasiedzi i niewybrane ignorowane)
    if (!history_is_tracked(key)) return;

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_hist_t *m = get_or_load(key, true);
    if (!m) { if (s_mutex) xSemaphoreGive(s_mutex); return; }

    float ft = (float)value;
    m->kind = kind;
    m->cumulative = cumulative;
    m->last_total = ft;
    m->last_ts = ts_unix;

    uint32_t bh = floor_hour(ts_unix);
    bool hour_closed = (m->n_hours > 0 && m->hours[m->n_hours-1].ts != bh);

    series_update(m->hours,  &m->n_hours,  HIST_HOURS,  bh, ft);
    series_update(m->days,   &m->n_days,   HIST_DAYS,   floor_day(ts_unix),   ft);
    series_update(m->months, &m->n_months, HIST_MONTHS, floor_month(ts_unix), ft);
    series_update(m->years,  &m->n_years,  HIST_YEARS,  floor_year(ts_unix),  ft);
    rt_update(m, ts_unix, ft);

    if (hour_closed) save_meter(m);
    if (s_mutex) xSemaphoreGive(s_mutex);
}

// ---------- JSON historii (zuzycie = roznice) ----------
int history_get_json(const char *id_hex, const char *res, char *buf, int buf_cap) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_hist_t *m = get_or_load(id_hex, false);
    int n = 0;
    if (!m) {
        n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":0,\"points\":[]}", id_hex ? id_hex : "");
        if (s_mutex) xSemaphoreGive(s_mutex);
        return n;
    }

    hist_bucket_t *arr = m->hours; int cnt = m->n_hours;
    if      (strcmp(res, "day")   == 0) { arr = m->days;   cnt = m->n_days; }
    else if (strcmp(res, "month") == 0) { arr = m->months; cnt = m->n_months; }
    else if (strcmp(res, "year")  == 0) { arr = m->years;  cnt = m->n_years; }

    n += snprintf(buf + n, buf_cap - n, "{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"points\":[", m->id, m->kind, m->cumulative);
    // zuzycie = total[i] - total[i-1]; pierwszy punkt pomijamy (brak odniesienia)
    bool first = true;
    if (strcmp(res, "rt") == 0) {
        // czas realny: pokazujemy total (nie roznice)
        for (int i = 0; i < m->n_rt && n < buf_cap - 60; i++) {
            n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                          first ? "" : ",", (unsigned)m->rt[i].ts, m->rt[i].total);
            first = false;
        }
    } else {
        for (int i = 1; i < cnt && n < buf_cap - 60; i++) {
            float v;
            if (m->cumulative) {
                v = arr[i].total - arr[i-1].total;  // zuzycie = roznica
                if (v < 0) v = 0;
            } else {
                v = arr[i].total;  // chwilowe (moc/napiecie) = wartosc
            }
            n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                          first ? "" : ",", (unsigned)arr[i].ts, v);
            first = false;
        }
    }
    n += snprintf(buf + n, buf_cap - n, "]}");
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
}

int history_list_json(char *buf, int buf_cap) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = snprintf(buf, buf_cap, "[");
    bool first = true;
    for (int i = 0; i < MAX_HIST_METERS; i++) {
        if (!s_meters[i].used) continue;
        n += snprintf(buf + n, buf_cap - n, "%s{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"last\":%.3f,\"ts\":%u,\"tracked\":%s}",
                      first ? "" : ",", s_meters[i].id, s_meters[i].kind, s_meters[i].cumulative,
                      s_meters[i].last_total, (unsigned)s_meters[i].last_ts,
                      history_is_tracked(s_meters[i].id) ? "true" : "false");
        first = false;
    }
    n += snprintf(buf + n, buf_cap - n, "]");
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
}

bool history_last_known(const char *id_hex, double *out_total, int *out_kind, uint32_t *out_ts) {
    bool found = false;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_hist_t *m = get_or_load(id_hex, false);
    if (m) {
        *out_total = m->last_total; *out_kind = m->kind; *out_ts = m->last_ts;
        found = true;
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return found;
}
