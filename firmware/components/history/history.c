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

// Sciezka archiwum godzinowego (miesiac) - tylko na flash, nie w RAM.
static void archive_path(const char *id, char *out, int cap) {
    char safe[28];
    int j = 0;
    for (int i = 0; id[i] && j < (int)sizeof(safe) - 1; i++)
        safe[j++] = (id[i] == ':') ? '_' : id[i];
    safe[j] = 0;
    snprintf(out, cap, "/spiffs/ha_%s.bin", safe);
}

// Wspolny bufor roboczy archiwum (5.8KB .bss) - uzywany przez append i get_day.
// Format pliku archiwum (ring buffer na flash, BEZ wczytywania calosci do RAM):
//   naglowek v2: [u32 magic][int count][int head][u32 last_ts][float last_total]
//   naglowek v1 (stary): [int count][int head]
//   dane: HIST_ARCHIVE_HOURS rekordow hist_bucket_t (ring)
// last_ts/last_total = DOKLADNY czas i wartosc ostatniego odczytu (aktualizowane
// co ramke) - po restarcie dashboard pokazuje prawdziwy czas ostatniego odczytu,
// a nie czas zapisu pliku h_ (ten powstaje tylko przy zamknieciu godziny).
// Rekord logiczny i (0=najstarszy) jest fizycznie na pozycji (head+i)%cap.
#define ARC_HDR_BYTES (2 * (int)sizeof(int))                        // v1
#define ARC_MAGIC_V2  0x53494832u                                   // "SIH2"
#define ARC_HDR_V2    (int)(sizeof(uint32_t)*2 + sizeof(int)*2 + sizeof(float))
#define ARC_DAY_BUF 64   // rekordow do bufora dnia (doba=24, zapas)
static hist_bucket_t s_day_buf[ARC_DAY_BUF];

typedef struct {
    int count, head;
    int hdr;               // rozmiar naglowka w bajtach (v1/v2)
    bool v2;
    uint32_t last_ts;      // tylko v2 (0 w v1)
    float last_total;      // tylko v2
} arc_hdr_t;

// Odczyt naglowka archiwum (v1 lub v2). Zwraca true gdy plik poprawny.
static bool arc_read_header2(FILE *f, arc_hdr_t *h) {
    memset(h, 0, sizeof(*h));
    uint32_t first = 0;
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fread(&first, sizeof(uint32_t), 1, f) != 1) return false;
    if (first == ARC_MAGIC_V2) {
        h->v2 = true; h->hdr = ARC_HDR_V2;
        if (fread(&h->count, sizeof(int), 1, f) != 1) return false;
        if (fread(&h->head,  sizeof(int), 1, f) != 1) return false;
        if (fread(&h->last_ts, sizeof(uint32_t), 1, f) != 1) return false;
        if (fread(&h->last_total, sizeof(float), 1, f) != 1) return false;
    } else {
        // v1: pierwszy int to count. Poprawny count jest maly (<=17520), wiec
        // nie koliduje z magic.
        h->v2 = false; h->hdr = ARC_HDR_BYTES;
        h->count = (int)first;
        if (fread(&h->head, sizeof(int), 1, f) != 1) return false;
    }
    // Wykryj niezgodny format: count/head poza zakresem -> traktuj jako pusty.
    if (h->count < 0 || h->count > HIST_ARCHIVE_HOURS) { h->count = 0; h->head = 0; return false; }
    if (h->head < 0 || h->head >= HIST_ARCHIVE_HOURS)  { h->count = 0; h->head = 0; return false; }
    return true;
}

// Zapis naglowka v2 na poczatku pliku.
static void arc_write_header_v2(FILE *f, const arc_hdr_t *h) {
    uint32_t magic = ARC_MAGIC_V2;
    fseek(f, 0, SEEK_SET);
    fwrite(&magic, sizeof(uint32_t), 1, f);
    fwrite(&h->count, sizeof(int), 1, f);
    fwrite(&h->head,  sizeof(int), 1, f);
    fwrite(&h->last_ts, sizeof(uint32_t), 1, f);
    fwrite(&h->last_total, sizeof(float), 1, f);
}

// Pozycja w pliku (bajty) rekordu fizycznego o indeksie phys (naglowek hdr B).
static long arc_rec_off2(int hdr, int phys) {
    return (long)hdr + (long)phys * (long)sizeof(hist_bucket_t);
}

// Jednorazowa migracja pliku v1 -> v2: kopiowanie strumieniowe do pliku .t,
// potem rename. Rekordy przenoszone 1:1 (fizyczny uklad ringu zachowany),
// wiec nie tracimy zebranego archiwum. Zwraca otwarty plik v2 (r+b) lub NULL.
static FILE *arc_migrate_v2(const char *path, arc_hdr_t *h) {
    char tmp[52];
    snprintf(tmp, sizeof(tmp), "%s.t", path);
    FILE *src = fopen(path, "rb");
    if (!src) return NULL;
    FILE *dst = fopen(tmp, "w+b");
    if (!dst) { fclose(src); return NULL; }
    h->v2 = true; h->hdr = ARC_HDR_V2; h->last_ts = 0; h->last_total = 0;
    arc_write_header_v2(dst, h);
    // Skopiuj region rekordow w calosci (uklad fizyczny bez zmian).
    static uint8_t cp[1024];
    fseek(src, ARC_HDR_BYTES, SEEK_SET);
    size_t n;
    while ((n = fread(cp, 1, sizeof(cp), src)) > 0) {
        if (fwrite(cp, 1, n, dst) != n) { fclose(src); fclose(dst); remove(tmp); return NULL; }
    }
    fclose(src);
    fclose(dst);
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return NULL; }
    ESP_LOGI(TAG, "Archiwum %s zmigrowane do formatu v2", path);
    return fopen(path, "r+b");
}

// Dopisz/zaktualizuj punkt godzinowy w archiwum (ring buffer, strumieniowo).
// NIE wczytuje calego pliku do RAM - tylko naglowek + ostatni rekord.
// ts_exact = dokladny unix ostatniego odczytu (do naglowka v2).
static void archive_append_hour(const char *id, uint32_t hour_ts, float total,
                                uint32_t ts_exact) {
    if (!s_fs_ok) return;
    char path[48]; archive_path(id, path, sizeof(path));

    arc_hdr_t h;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        // nowy plik - utworz z naglowkiem v2
        f = fopen(path, "w+b");
        if (!f) { ESP_LOGW(TAG, "nie moge utworzyc archiwum %s", path); return; }
        memset(&h, 0, sizeof(h));
        h.v2 = true; h.hdr = ARC_HDR_V2;
    } else {
        if (!arc_read_header2(f, &h)) {
            // Niezgodny format - zresetuj plik (pusty ring v2).
            fclose(f);
            f = fopen(path, "w+b");
            if (!f) { ESP_LOGW(TAG, "nie moge zresetowac archiwum %s", path); return; }
            memset(&h, 0, sizeof(h));
            h.v2 = true; h.hdr = ARC_HDR_V2;
        } else if (!h.v2) {
            // Poprawny plik v1 - jednorazowa migracja do v2 (bez utraty danych).
            fclose(f);
            f = arc_migrate_v2(path, &h);
            if (!f) { ESP_LOGW(TAG, "migracja archiwum %s nieudana", path); return; }
        }
    }
    h.last_ts = ts_exact;
    h.last_total = total;

    // Czy ostatnia zapisana godzina == hour_ts? (aktualizacja biezacej godziny)
    if (h.count > 0) {
        int last_phys = (h.head + h.count - 1) % HIST_ARCHIVE_HOURS;
        hist_bucket_t last;
        if (fseek(f, arc_rec_off2(h.hdr, last_phys), SEEK_SET) == 0 &&
            fread(&last, sizeof(last), 1, f) == 1 && last.ts == hour_ts) {
            // ta sama godzina - nadpisz total + swiezy naglowek (dokladny last_ts)
            last.total = total;
            fseek(f, arc_rec_off2(h.hdr, last_phys), SEEK_SET);
            fwrite(&last, sizeof(last), 1, f);
            arc_write_header_v2(f, &h);
            fclose(f);
            return;
        }
    }

    // Nowa godzina - dopisz rekord. Gdy pelne, przesun head (nadpisz najstarszy).
    int write_phys;
    if (h.count >= HIST_ARCHIVE_HOURS) {
        write_phys = h.head;                        // nadpisz najstarszy
        h.head = (h.head + 1) % HIST_ARCHIVE_HOURS; // przesun okno
    } else {
        write_phys = (h.head + h.count) % HIST_ARCHIVE_HOURS;
        h.count++;
    }
    hist_bucket_t rec = { hour_ts, total };
    fseek(f, arc_rec_off2(h.hdr, write_phys), SEEK_SET);
    fwrite(&rec, sizeof(rec), 1, f);
    arc_write_header_v2(f, &h);
    fclose(f);
}

// Odbuduj archiwum godzinowe (ha_) z bufora hours[] w RAM (7 dni godzin wczytanych
// z h_). Wolane przy starcie gdy archiwum jest puste/stare/niezgodne - dzieki temu
// tryb Dzis/Wczoraj pokazuje dane natychmiast po restarcie, bez czekania na ramke.
// Zapisuje tylko gdy archiwum nie ma juz wiecej danych niz hours[] (nie nadpisuje
// pelnego 2-letniego archiwum swiezo zebranego).
static void archive_rebuild_from_hours(meter_hist_t *m) {
    if (!s_fs_ok || m->n_hours == 0) return;
    char path[48]; archive_path(m->id, path, sizeof(path));

    // Sprawdz istniejace archiwum - ile ma poprawnych rekordow.
    int existing = -1;  // -1 = brak/niezgodne
    FILE *f = fopen(path, "rb");
    if (f) {
        arc_hdr_t hh;
        if (arc_read_header2(f, &hh)) existing = hh.count;  // poprawny format v1/v2
        fclose(f);
    }
    // Odbuduj tylko gdy archiwum puste, niezgodne (stary format) lub ma mniej
    // rekordow niz mamy godzin w RAM (nie psujemy bogatszego archiwum).
    if (existing >= m->n_hours) return;

    // Zapisz na nowo: naglowek v2 + godziny z RAM (chronologicznie, jak w hours[]).
    f = fopen(path, "w+b");
    if (!f) { ESP_LOGW(TAG, "nie moge odbudowac archiwum %s", path); return; }
    arc_hdr_t h = { .count = m->n_hours, .head = 0, .hdr = ARC_HDR_V2, .v2 = true,
                    .last_ts = m->last_ts, .last_total = m->last_total };
    arc_write_header_v2(f, &h);
    for (int i = 0; i < m->n_hours; i++) {
        fwrite(&m->hours[i], sizeof(hist_bucket_t), 1, f);
    }
    fclose(f);
    ESP_LOGI(TAG, "Archiwum godzinowe odbudowane z RAM (%d godz) dla %s", h.count, m->id);
}

// Odtworz z archiwum ha_ najswiezszy stan po restarcie. Plik h_ (hours[] itd.)
// zapisywany jest tylko przy ZAMKNIECIU godziny, wiec po restarcie kubelek
// biezacej godziny i last_total sa stare (nawet o ~59 min). Jesli restart/przerwa
// przetnie granice godziny, kubelek przerwanej godziny nigdy nie zostaje
// uzupelniony - wykres pokazuje dla niej ~0, a brakujace zuzycie wpada w
// nastepna godzine. Archiwum ha_ jest natomiast aktualizowane PRZY KAZDEJ ramce
// (archive_append_hour), wiec jego ostatni rekord to stan z ostatniego odczytu
// przed restartem - scal go z seriami w RAM (zero dodatkowych zapisow flash).
static void restore_from_archive(meter_hist_t *m) {
    if (!s_fs_ok) return;
    char path[48]; archive_path(m->id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    arc_hdr_t h;
    bool hdr_ok = arc_read_header2(f, &h);
    if (hdr_ok && h.count > 0) {
        hist_bucket_t rec;
        int phys = (h.head + h.count - 1) % HIST_ARCHIVE_HOURS;   // najnowszy rekord
        if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) == 0 &&
            fread(&rec, sizeof(rec), 1, f) == 1 &&
            rec.ts >= 1700000000) {
            uint32_t last_bh = (m->n_hours > 0) ? m->hours[m->n_hours - 1].ts : 0;
            if (rec.ts >= last_bh) {
                series_update(m->hours,  &m->n_hours,  HIST_HOURS,  rec.ts, rec.total);
                series_update(m->days,   &m->n_days,   HIST_DAYS,   floor_day(rec.ts),   rec.total);
                series_update(m->months, &m->n_months, HIST_MONTHS, floor_month(rec.ts), rec.total);
                series_update(m->years,  &m->n_years,  HIST_YEARS,  floor_year(rec.ts),  rec.total);
                // Rekord archiwum jest co najmniej tak swiezy jak stan z pliku h_
                // (arc pisany co ramke, h_ raz na godzine) - przejmij total.
                if (rec.ts >= floor_hour(m->last_ts)) {
                    m->last_total = rec.total;
                    if (rec.ts > m->last_ts) m->last_ts = rec.ts;
                }
            }
        }
    }
    // Naglowek v2 niesie DOKLADNY czas i wartosc ostatniego odczytu sprzed
    // restartu - niezaleznie od stanu kubelkow przejmij, gdy swiezszy.
    if (hdr_ok && h.v2 && h.last_ts >= 1700000000 && h.last_ts >= m->last_ts) {
        m->last_ts = h.last_ts;
        m->last_total = h.last_total;
    }
    fclose(f);
    // Plik v1 (sprzed formatu z dokladnym czasem): zmigruj JUZ TERAZ, przy
    // starcie - nie czekajac na pierwsza ramke - i zapisz najlepszy znany stan.
    // Dzieki temu kazdy KOLEJNY restart (nawet chwile pozniej) ma naglowek v2,
    // a ramki odbierane od teraz aktualizuja go co odczyt.
    if (hdr_ok && !h.v2) {
        FILE *fm = arc_migrate_v2(path, &h);
        if (fm) {
            h.last_ts = m->last_ts;
            h.last_total = m->last_total;
            arc_write_header_v2(fm, &h);
            fclose(fm);
        }
    }
    ESP_LOGI(TAG, "Odtworzono %s: last_ts=%u total=%.3f (arch %s)",
             m->id, (unsigned)m->last_ts, (double)m->last_total,
             hdr_ok ? (h.v2 ? "v2" : "v1->v2") : "brak/nowy");
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
    fwrite(&m->cumulative, sizeof(int), 1, f);  // na koncu (kompatybilnosc starych plikow)
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
    m->n_hours = (int)fread(m->hours, sizeof(hist_bucket_t), m->n_hours, f);
    fread(&m->n_days, sizeof(int), 1, f);
    if (m->n_days < 0 || m->n_days > HIST_DAYS) m->n_days = 0;
    m->n_days = (int)fread(m->days, sizeof(hist_bucket_t), m->n_days, f);
    fread(&m->n_months, sizeof(int), 1, f);
    if (m->n_months < 0 || m->n_months > HIST_MONTHS) m->n_months = 0;
    m->n_months = (int)fread(m->months, sizeof(hist_bucket_t), m->n_months, f);
    fread(&m->n_years, sizeof(int), 1, f);
    if (m->n_years < 0 || m->n_years > HIST_YEARS) m->n_years = 0;
    m->n_years = (int)fread(m->years, sizeof(hist_bucket_t), m->n_years, f);
    // cumulative na koncu (nowe pliki). Stary plik go nie ma - fread zwroci 0,
    // wtedy wnioskujemy z pola id: napiecie/moc = chwilowe, reszta = kumulacyjne.
    if (fread(&m->cumulative, sizeof(int), 1, f) != 1) {
        bool instant = (strstr(id, "napiecie") || strstr(id, "moc"));
        m->cumulative = instant ? 0 : 1;
    }
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

// Lista sledzonych kluczy jako JSON: ["id:pole","id2:pole2",...]
int history_tracked_json(char *buf, int buf_cap) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = snprintf(buf, buf_cap, "[");
    for (int i = 0; i < s_tracked_count; i++) {
        n += snprintf(buf + n, buf_cap - n, "%s\"%s\"", i ? "," : "", s_tracked[i]);
    }
    n += snprintf(buf + n, buf_cap - n, "]");
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
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

static meter_hist_t *get_or_load(const char *id, bool create_if_missing);

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

    // Wczytaj od razu z flash wszystkie sledzone liczniki (eager load), zeby
    // dane historyczne i ostatni stan byly dostepne natychmiast po restarcie -
    // bez czekania na pierwsza ramke. Inaczej s_meters[].used=false i historia
    // oraz karty dashboardu pokazuja "oczekiwanie" mimo danych na flash.
    int loaded = 0;
    for (int i = 0; i < s_tracked_count; i++) {
        meter_hist_t *m = get_or_load(s_tracked[i], false);
        if (m) {
            loaded++;
            archive_rebuild_from_hours(m);  // odbuduj archiwum ha_ z godzin w RAM
        }
    }
    ESP_LOGI(TAG, "Wczytano historie z flash dla %d/%d licznikow", loaded, s_tracked_count);
}

// znajdz w RAM, lub wczytaj z dysku WPROST do slotu (bez kopii na stosie!)
static meter_hist_t *get_or_load(const char *id, bool create_if_missing) {
    meter_hist_t *m = find_meter(id, false);
    if (m) return m;
    // nie ma w RAM - zaalokuj slot i wczytaj z dysku wprost do niego
    meter_hist_t *slot = find_meter(id, true);
    if (!slot) return NULL;
    if (load_meter(slot, id)) {
        restore_from_archive(slot);   // scal swiezszy stan z archiwum ha_ (po restarcie)
        return slot;   // wczytano z dysku
    }
    // brak pliku
    if (create_if_missing) {
        // slot juz zainicjowany przez find_meter (memset+id+used)
        restore_from_archive(slot);   // h_ mogl zostac usuniety, archiwum moze istniec
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

    // Archiwum godzinowe co ramke - ale tylko dla SLEDZONYCH licznikow (nie pisz
    // na flash dla kazdego zaslyszanego sasiada). To zrodlo odtwarzania stanu
    // biezacej godziny po restarcie (restore_from_archive).
    if (history_is_tracked(id_hex)) archive_append_hour(m->id, bh, ft, ts_unix);

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

    // Archiwum godzinowe (miesiac) na flash dla sledzonego pola.
    archive_append_hour(m->id, bh, ft, ts_unix);

    if (hour_closed) save_meter(m);
    if (s_mutex) xSemaphoreGive(s_mutex);
}

// ---------- JSON historii (zuzycie = roznice) ----------
// Najwczesniejszy znany stan licznika w danym okresie (lub tuz przed nim) - baza
// do policzenia przyrostu pierwszego/biezacego okresu, ktory nie ma poprzednika.
// Szuka w buforach od najdrobniejszego (godziny->dni->miesiace) pierwszego punktu
// o ts >= period_start. Zwraca true gdy znaleziono.
static bool earliest_total_in(meter_hist_t *m, uint32_t period_start,
                              float cur_total, float *out) {
    // 1. Najmniejszy (najwczesniejszy) stan licznika w okresie [period_start, teraz].
    float best = 1e30f; bool found = false;
    hist_bucket_t *series[4] = { m->hours, m->days, m->months, m->years };
    int counts[4] = { m->n_hours, m->n_days, m->n_months, m->n_years };
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < counts[s]; i++) {
            if (series[s][i].ts >= period_start && series[s][i].total < best) {
                best = series[s][i].total; found = true;
            }
        }
    }
    // 2. Gdy brak punktu w okresie LUB baza rowna biezacemu stanowi (pierwsza ramka
    //    ustawila wszystkie bufory na ten sam total) - wez globalnie najmniejszy
    //    znany stan (poczatek zbierania danych) = rzeczywiste naliczenie do teraz.
    if (!found || best >= cur_total - 0.0001f) {
        float gmin = cur_total;
        for (int s = 0; s < 4; s++)
            for (int i = 0; i < counts[s]; i++)
                if (series[s][i].total < gmin) gmin = series[s][i].total;
        best = gmin; found = true;
    }
    *out = best;
    return found;
}

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
        for (int i = 0; i < cnt && n < buf_cap - 60; i++) {
            float v;
            if (m->cumulative) {
                if (i > 0) {
                    v = arr[i].total - arr[i-1].total;  // zuzycie = roznica
                } else {
                    // Pierwszy/biezacy okres bez poprzednika: policz przyrost od
                    // najwczesniejszego znanego stanu w tym okresie (czesciowe naliczenie).
                    float base;
                    uint32_t pstart = arr[i].ts;
                    if (earliest_total_in(m, pstart, arr[i].total, &base) && arr[i].total >= base) {
                        v = arr[i].total - base;
                    } else {
                        continue;  // brak odniesienia - pomin
                    }
                }
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

// Zwraca godzinowe zuzycie z konkretnego dnia z archiwum miesiecznego (flash).
// Czyta plik archiwum, filtruje punkty z wybranej doby, liczy zuzycie jako roznice
// kolejnych totali (dla kumulacyjnych) lub wartosc (dla chwilowych).
int history_get_day_json(const char *id_hex, uint32_t day_ts, char *buf, int buf_cap) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    // Pobierz metadane (kind, cumulative) z RAM jesli licznik zaladowany.
    meter_hist_t *m = get_or_load(id_hex, false);
    int kind = m ? m->kind : 0;
    int cumulative = m ? m->cumulative : 1;

    uint32_t d0 = floor_day(day_ts);          // poczatek wybranej doby
    uint32_t d1 = d0 + 86400;                  // poczatek nastepnej doby

    int n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"points\":[",
                     id_hex ? id_hex : "", kind, cumulative);

    // Zbierz godziny wybranej doby (+1 poprzednia dla roznicy) do malego bufora.
    // ZRODLO 1: bufor hours[] w RAM (ostatnie 7 dni) - niezawodne, te same dane co
    // res=hour. Pokrywa Dzis/Wczoraj/caly tydzien.
    // ZRODLO 2 (fallback): archiwum ha_ na flash - dla dni starszych niz 7 dni.
    int nb = 0;
    hist_bucket_t prev; bool has_prev = false;

    if (m) {
        for (int i = 0; i < m->n_hours && nb < ARC_DAY_BUF; i++) {
            if (m->hours[i].ts >= d0 && m->hours[i].ts < d1) {
                s_day_buf[nb++] = m->hours[i];
            } else if (m->hours[i].ts < d0) {
                prev = m->hours[i]; has_prev = true;
            }
        }
    }

    // Fallback do archiwum tylko gdy w RAM nie znaleziono godzin tej doby (starszy
    // lub pusty dzien). Rekordy w archiwum sa chronologiczne (rosnace ts) w
    // kolejnosci logicznej, wiec NIE skanujemy calego ringu (do 17520 rekordow po
    // jednym fseek+fread na SPIFFS = dziesiatki sekund z trzymanym s_mutex, co
    // blokowalo history_on_reading i zawieszalo modul przy przegladaniu dni bez
    // historii). Zamiast tego: wyszukiwanie binarne pierwszego rekordu doby
    // (~15 odczytow) + odczyt sekwencyjny do konca doby (max ~25 rekordow).
    if (nb == 0) {
        char path[48]; archive_path(id_hex, path, sizeof(path));
        FILE *f = s_fs_ok ? fopen(path, "rb") : NULL;
        if (f) {
            arc_hdr_t h;
            if (arc_read_header2(f, &h) && h.count > 0) {
                hist_bucket_t rec;
                // Najmniejszy indeks logiczny lo z ts >= d0.
                int lo = 0, hi = h.count;
                while (lo < hi) {
                    int mid = lo + (hi - lo) / 2;
                    int phys = (h.head + mid) % HIST_ARCHIVE_HOURS;
                    if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) != 0 ||
                        fread(&rec, sizeof(rec), 1, f) != 1) { lo = h.count; break; }
                    if (rec.ts < d0) lo = mid + 1; else hi = mid;
                }
                // Rekord poprzedzajacy dobe - baza roznicy dla pierwszej godziny.
                if (lo > 0 && lo <= h.count) {
                    int phys = (h.head + lo - 1) % HIST_ARCHIVE_HOURS;
                    if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) == 0 &&
                        fread(&rec, sizeof(rec), 1, f) == 1 && rec.ts < d0) {
                        prev = rec; has_prev = true;
                    }
                }
                // Sekwencyjnie od lo; rekordy rosnace, wiec koniec przy ts >= d1.
                for (int i = lo; i < h.count && nb < ARC_DAY_BUF; i++) {
                    int phys = (h.head + i) % HIST_ARCHIVE_HOURS;
                    if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) != 0) break;
                    if (fread(&rec, sizeof(rec), 1, f) != 1) break;
                    if (rec.ts >= d1) break;
                    if (rec.ts >= d0) s_day_buf[nb++] = rec;
                }
            }
            fclose(f);
        }
    }

    // Wypisz punkty (zuzycie = roznica wzgledem poprzedniej godziny dla kumulacyjnych).
    // Gdy miedzy odczytami jest luka (np. restart przy aktualizacji) krotsza niz
    // GAP_FILL_MAX_H godzin, rozkladamy przyrost rownomiernie na godziny luki i
    // emitujemy punkt dla kazdej brakujacej godziny (mieszczacej sie w dobie) -
    // eliminuje artefakt jednego mikroskopijnego slupka po restarcie.
    const uint32_t GAP_FILL_MAX_H = 6;
    bool first = true;
    for (int i = 0; i < nb && n < buf_cap - 60; i++) {
        if (cumulative) {
            hist_bucket_t *ref = NULL;
            if (i > 0) ref = &s_day_buf[i-1];
            else if (has_prev) ref = &prev;

            if (ref && s_day_buf[i].ts > ref->ts) {
                uint32_t gap_h = (s_day_buf[i].ts - ref->ts) / 3600;
                float delta = s_day_buf[i].total - ref->total;
                if (delta < 0) delta = 0;
                if (gap_h <= 1) {
                    n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                                  first ? "" : ",", (unsigned)s_day_buf[i].ts, delta);
                    first = false;
                } else if (gap_h <= GAP_FILL_MAX_H) {
                    // Rozloz rownomiernie; emituj punkt dla kazdej godziny luki
                    // ktora wpada w wybrana dobe [d0,d1).
                    float per = delta / (float)gap_h;
                    for (uint32_t g = 1; g <= gap_h && n < buf_cap - 60; g++) {
                        uint32_t tg = ref->ts + g * 3600;
                        if (tg < d0 || tg >= d1) continue;
                        n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                                      first ? "" : ",", (unsigned)tg, per);
                        first = false;
                    }
                } else {
                    // Luka za dluga (np. modul wylaczony) - nie rozkladaj, jeden punkt.
                    n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                                  first ? "" : ",", (unsigned)s_day_buf[i].ts, delta);
                    first = false;
                }
            } else {
                n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                              first ? "" : ",", (unsigned)s_day_buf[i].ts, 0.0f);
                first = false;
            }
        } else {
            // Chwilowe (moc/napiecie) - wartosc bezposrednia, bez rozkladu.
            n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                          first ? "" : ",", (unsigned)s_day_buf[i].ts, s_day_buf[i].total);
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

// ----- API dla wyswietlacza e-ink -----

bool history_display_summary(const char *key, hist_display_t *out) {
    if (!key || !out) return false;
    memset(out, 0, sizeof(*out));
    strncpy(out->key, key, sizeof(out->key) - 1);

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_hist_t *m = get_or_load(key, false);
    if (!m) {
        if (s_mutex) xSemaphoreGive(s_mutex);
        return false;
    }

    out->kind       = m->kind;
    out->cumulative = m->cumulative;
    out->last_total = m->last_total;
    out->last_ts    = m->last_ts;
    out->has_value  = (m->last_ts != 0);

    // Granice dni wedlug czasu lokalnego.
    uint32_t now = (uint32_t)time(NULL);
    uint32_t day0 = floor_day(now);              // poczatek dzis
    uint32_t day_1 = floor_day(day0 - 1);        // poczatek wczoraj
    uint32_t day_2 = floor_day(day_1 - 1);       // poczatek przedwczoraj
    uint32_t day_3 = floor_day(day_2 - 1);       // poczatek 3 dni temu

    if (m->cumulative) {
        // WAZNE: bufor dzienny (series_update) trzyma total na KONIEC dnia
        // (ostatni odczyt danego dnia), nie na poczatek. Zatem:
        //   zuzycie dzis      = biezacy total - total na koniec WCZORAJ
        //   zuzycie wczoraj   = total koniec wczoraj - total koniec przedwczoraj
        //   zuzycie przedwcz. = total koniec przedwczoraj - total koniec 3 dni temu
        float t_d1 = 0, t_d2 = 0, t_d3 = 0;
        bool h1 = false, h2 = false, h3 = false;
        for (int i = 0; i < m->n_days; i++) {
            if      (m->days[i].ts == day_1) { t_d1 = m->days[i].total; h1 = true; }
            else if (m->days[i].ts == day_2) { t_d2 = m->days[i].total; h2 = true; }
            else if (m->days[i].ts == day_3) { t_d3 = m->days[i].total; h3 = true; }
        }
        if (h1 && out->has_value) {
            out->today = m->last_total - t_d1;
            if (out->today < 0) out->today = 0;
            out->has_today = true;
        } else if (out->has_value) {
            // Brak danych z wczoraj (np. pierwszy dzien) - dzis = 0 do polnocy.
            out->today = 0;
            out->has_today = true;
        }
        if (h1 && h2) {
            out->yesterday = t_d1 - t_d2;
            if (out->yesterday < 0) out->yesterday = 0;
            out->has_yesterday = true;
        }
        if (h2 && h3) {
            out->day_before = t_d2 - t_d3;
            if (out->day_before < 0) out->day_before = 0;
            out->has_day_before = true;
        }
    } else {
        // Chwilowe (moc/napiecie): "dzis" = biezaca wartosc, dzien = ostatnia z dnia.
        out->today = m->last_total;
        out->has_today = out->has_value;
        for (int i = 0; i < m->n_days; i++) {
            if (m->days[i].ts == day_1) { out->yesterday = m->days[i].total; out->has_yesterday = true; }
            else if (m->days[i].ts == day_2) { out->day_before = m->days[i].total; out->has_day_before = true; }
        }
    }

    if (s_mutex) xSemaphoreGive(s_mutex);
    return true;
}

int history_tracked_meter_ids(char ids[][12], int max_ids) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = 0;
    for (int i = 0; i < s_tracked_count && count < max_ids; i++) {
        // Klucz to "id" lub "id:pole" - wyciagnij sama czesc id.
        char id[12] = {0};
        const char *colon = strchr(s_tracked[i], ':');
        int len = colon ? (int)(colon - s_tracked[i]) : (int)strlen(s_tracked[i]);
        if (len > 11) len = 11;
        memcpy(id, s_tracked[i], len);
        // Czy juz dodane?
        bool dup = false;
        for (int j = 0; j < count; j++)
            if (strcasecmp(ids[j], id) == 0) { dup = true; break; }
        if (!dup) {
            strncpy(ids[count], id, 11);
            ids[count][11] = 0;
            count++;
        }
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return count;
}

int history_keys_for_id(const char *id, char keys[][28], int max_keys) {
    if (!id) return 0;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = 0;
    int idlen = (int)strlen(id);
    for (int i = 0; i < s_tracked_count && count < max_keys; i++) {
        // Klucz pasuje gdy zaczyna sie od "id" i dalej jest ':' lub koniec.
        if (strncasecmp(s_tracked[i], id, idlen) == 0 &&
            (s_tracked[i][idlen] == ':' || s_tracked[i][idlen] == '\0')) {
            strncpy(keys[count], s_tracked[i], 27);
            keys[count][27] = 0;
            count++;
        }
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return count;
}

int history_tracked_count(void) {
    return s_tracked_count;
}

void history_tracked_first(char *out, int cap) {
    if (!out || cap < 1) return;
    if (s_tracked_count > 0) strncpy(out, s_tracked[0], cap - 1), out[cap-1] = 0;
    else out[0] = 0;
}
