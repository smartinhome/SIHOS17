// beta443: zapis historii na LittleFS - patrz opis w hist_store.h.
#include "hist_store.h"
// Dlugosci nazw z readdir sa sprawdzane przed skladaniem sciezek - GCC tego
// nie widzi i zglasza mozliwe obciecie przy kazdym snprintf z d_name.
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
static const char *TAG = "HSTORE";
#define HS_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define HS_LOGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define HS_LOGI(...) (printf(__VA_ARGS__), putchar('\n'))
#define HS_LOGW(...) (printf("W: " __VA_ARGS__), putchar('\n'))
#endif

#define REC ((long)sizeof(hist_bucket_t))

const char *hs_root = "/spiffs";
size_t (*hs_free_fn)(void) = NULL;

// ======================= czas i sciezki =======================

static int ym_of(uint32_t ts) {
    time_t t = ts; struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 12 + tm.tm_mon;
}

// Poczatek tygodnia (poniedzialek 00:00 czasu lokalnego).
static uint32_t week_start(uint32_t ts) {
    time_t t = ts; struct tm tm;
    localtime_r(&t, &tm);
    tm.tm_mday -= (tm.tm_wday + 6) % 7;
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (uint32_t)mktime(&tm);
}

static int ymd_of(uint32_t ts) {
    time_t t = ts; struct tm tm;
    localtime_r(&t, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

static void arc_path(char *out, int cap, const char *safe, int ym) {
    snprintf(out, cap, "%s/a/%s/%04d%02d.bin", hs_root, safe, ym / 12, ym % 12 + 1);
}

static void last_path(char *out, int cap, const char *safe) {
    snprintf(out, cap, "%s/a/%s/last.bin", hs_root, safe);
}

static void crv_path(char *out, int cap, const char *safe, uint32_t ws, bool maxfile) {
    snprintf(out, cap, "%s/%c/%s/%08d.bin", hs_root, maxfile ? 'm' : 'c', safe, ymd_of(ws));
}

static long file_size(FILE *f) {
    if (fseek(f, 0, SEEK_END) != 0) return 0;
    long sz = ftell(f);
    return sz > 0 ? sz : 0;
}

bool hs_mkdirs(const char *file_path) {
    char p[128];
    snprintf(p, sizeof(p), "%s", file_path);
    size_t rl = strlen(hs_root);
    for (char *s = p + rl + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        if (mkdir(p, 0775) != 0 && errno != EEXIST) {
            struct stat st;
            if (stat(p, &st) != 0) { *s = '/'; return false; }
        }
        *s = '/';
    }
    return true;
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// ======================= rekordy =======================

// Dopisz rekord albo nadpisz ostatni (ten sam ts). Rekord starszy niz ostatni
// w pliku jest pomijany. 1 = zapisano, 0 = pominieto, -1 = blad, -2 = brak miejsca.
static int put_rec(const char *path, uint32_t ts, float v) {
    errno = 0;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        if (errno != ENOENT) return -1;
        if (!hs_mkdirs(path)) return errno == ENOSPC ? -2 : -1;
        f = fopen(path, "w+b");
        if (!f) return errno == ENOSPC ? -2 : -1;
    }
    long n = file_size(f) / REC;          // niepelny rekord po zaniku pradu - nadpisany
    long pos = n * REC;
    if (n > 0) {
        hist_bucket_t last;
        if (fseek(f, (n - 1) * REC, SEEK_SET) == 0 && fread(&last, REC, 1, f) == 1) {
            if (last.ts == ts) pos = (n - 1) * REC;
            else if (last.ts > ts) { fclose(f); return 0; }
        }
    }
    hist_bucket_t r = { ts, v };
    int err = 0;
    bool ok = fseek(f, pos, SEEK_SET) == 0 && fwrite(&r, REC, 1, f) == 1;
    if (!ok) err = errno;
    if (fflush(f) != 0) { ok = false; if (!err) err = errno; }
    if (fsync(fileno(f)) != 0) { ok = false; if (!err) err = errno; }
    if (fclose(f) != 0) { ok = false; if (!err) err = errno; }
    if (ok) return 1;
    return err == ENOSPC ? -2 : -1;
}

// Blad zapisu: zwalniamy miejsce tylko, gdy to faktycznie brak miejsca - inny
// blad nie moze kasowac najstarszych danych.
static bool space_problem(int r) {
    return r == -2 || (hs_free_fn && hs_free_fn() < HS_FREE_MIN);
}

static bool put_last(const char *safe, uint32_t ts, float v) {
    char p[96]; last_path(p, sizeof(p), safe);
    FILE *f = fopen(p, "wb");
    if (!f && hs_mkdirs(p)) f = fopen(p, "wb");
    if (!f) return false;
    hist_bucket_t r = { ts, v };
    bool ok = fwrite(&r, REC, 1, f) == 1;
    if (fclose(f) != 0) ok = false;
    return ok;
}

// ======================= archiwum godzinowe =======================

bool hs_arc_put(const char *safe, uint32_t hour_ts, float total, uint32_t ts_exact) {
    char p[96]; arc_path(p, sizeof(p), safe, ym_of(hour_ts));
    if (!exists(p)) hs_ensure_space();             // nowy miesiac - nowy plik
    int r = put_rec(p, hour_ts, total);
    if (r < 0 && space_problem(r)) {                // pelno - zwolnij i sprobuj raz jeszcze
        hs_prune(true);
        r = put_rec(p, hour_ts, total);
    }
    if (r < 0) return false;
    if (ts_exact >= HS_TS_MIN && ts_exact <= HS_TS_MAX && !put_last(safe, ts_exact, total))
        return false;
    return true;
}

bool hs_arc_last_exact(const char *safe, uint32_t *ts, float *total) {
    char p[96]; last_path(p, sizeof(p), safe);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    hist_bucket_t r;
    bool ok = fread(&r, REC, 1, f) == 1 && r.ts >= HS_TS_MIN && r.ts <= HS_TS_MAX;
    fclose(f);
    if (ok) { *ts = r.ts; *total = r.total; }
    return ok;
}

// Miesiace archiwum klucza, rosnaco. Zwraca liczbe.
static int list_months(const char *safe, int *out, int max) {
    char d[80]; snprintf(d, sizeof(d), "%s/a/%s", hs_root, safe);
    DIR *dir = opendir(d);
    if (!dir) return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < max) {
        const char *nm = de->d_name;
        if (strlen(nm) != 10 || strcmp(nm + 6, ".bin") != 0) continue;
        int v = 0; bool dig = true;
        for (int i = 0; i < 6; i++) {
            if (nm[i] < '0' || nm[i] > '9') { dig = false; break; }
            v = v * 10 + (nm[i] - '0');
        }
        if (!dig) continue;
        int ym = (v / 100) * 12 + (v % 100) - 1;
        int j = n++;
        while (j > 0 && out[j - 1] > ym) { out[j] = out[j - 1]; j--; }   // wstawianie
        out[j] = ym;
    }
    closedir(dir);
    return n;
}

#define MAX_MONTHS 300

bool hs_arc_first(const char *safe, hist_bucket_t *out) {
    static int ms[MAX_MONTHS];
    int n = list_months(safe, ms, MAX_MONTHS);
    for (int i = 0; i < n; i++) {
        char p[96]; arc_path(p, sizeof(p), safe, ms[i]);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        bool ok = fread(out, REC, 1, f) == 1;
        fclose(f);
        if (ok) return true;
    }
    return false;
}

bool hs_arc_last(const char *safe, hist_bucket_t *out) {
    static int ms[MAX_MONTHS];
    int n = list_months(safe, ms, MAX_MONTHS);
    for (int i = n - 1; i >= 0; i--) {
        char p[96]; arc_path(p, sizeof(p), safe, ms[i]);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        long k = file_size(f) / REC;
        bool ok = k > 0 && fseek(f, (k - 1) * REC, SEEK_SET) == 0 && fread(out, REC, 1, f) == 1;
        fclose(f);
        if (ok) return true;
    }
    return false;
}

int hs_arc_count(const char *safe, int *months) {
    static int ms[MAX_MONTHS];
    int n = list_months(safe, ms, MAX_MONTHS);
    long recs = 0;
    for (int i = 0; i < n; i++) {
        char p[96]; arc_path(p, sizeof(p), safe, ms[i]);
        struct stat st;
        if (stat(p, &st) == 0) recs += st.st_size / REC;
    }
    if (months) *months = n;
    return (int)recs;
}

// ======================= iterator =======================

void hs_iter_open(hs_iter_t *it, const char *safe, uint32_t from_ts, uint32_t to_ts) {
    memset(it, 0, sizeof(*it));
    snprintf(it->safe, sizeof(it->safe), "%s", safe);
    int ym0 = ym_of(from_ts);
    it->ym_end = ym_of(to_ts > from_ts ? to_ts - 1 : from_ts);
    uint32_t now = (uint32_t)time(NULL);
    if (now >= HS_TS_MIN && it->ym_end > ym_of(now) + 1) it->ym_end = ym_of(now) + 1;
    if (it->ym_end < ym0) it->ym_end = ym0;
    it->ym_next = ym0 + 1;

    char p[96]; arc_path(p, sizeof(p), safe, ym0);
    FILE *f = fopen(p, "rb");
    if (f) {
        long n = file_size(f) / REC, a = 0, b = n;
        while (a < b) {                              // pierwszy rekord z ts >= from_ts
            long mid = (a + b) / 2;
            hist_bucket_t r;
            if (fseek(f, mid * REC, SEEK_SET) != 0 || fread(&r, REC, 1, f) != 1) { b = mid; break; }
            if (r.ts < from_ts) a = mid + 1; else b = mid;
        }
        fseek(f, (a > 0 ? a - 1 : 0) * REC, SEEK_SET);
        it->f = f;
        if (a > 0) return;                           // poprzednik w tym samym pliku
    }
    // Poprzednik = ostatni rekord najblizszego wczesniejszego miesiaca.
    for (int k = 1; k <= 24; k++) {
        arc_path(p, sizeof(p), safe, ym0 - k);
        FILE *g = fopen(p, "rb");
        if (!g) continue;
        long n = file_size(g) / REC;
        if (n > 0 && fseek(g, (n - 1) * REC, SEEK_SET) == 0 && fread(&it->pre, REC, 1, g) == 1)
            it->has_pre = true;
        fclose(g);
        break;
    }
}

int hs_iter_next(hs_iter_t *it, hist_bucket_t *out, int max) {
    int n = 0;
    if (it->has_pre && n < max) { out[n++] = it->pre; it->has_pre = false; }
    while (n < max) {
        if (!it->f) {
            while (it->ym_next <= it->ym_end && !it->f) {
                char p[96]; arc_path(p, sizeof(p), it->safe, it->ym_next++);
                it->f = fopen(p, "rb");
            }
            if (!it->f) break;
        }
        int want = max - n;
        int r = (int)fread(out + n, REC, (size_t)want, (FILE *)it->f);
        n += r;
        if (r < want) { fclose((FILE *)it->f); it->f = NULL; }
    }
    return n;
}

void hs_iter_close(hs_iter_t *it) {
    if (it->f) fclose((FILE *)it->f);
    it->f = NULL;
}

// ======================= krzywa 5-min =======================

// Usun pliki tygodni starszych niz okno przechowywania (dla jednego klucza).
static void crv_prune_key(const char *safe, bool maxfile, uint32_t now_ts) {
    int cut = ymd_of(week_start(now_ts - HS_CRV_KEEP_DAYS * 86400u));
    char d[80]; snprintf(d, sizeof(d), "%s/%c/%s", hs_root, maxfile ? 'm' : 'c', safe);
    DIR *dir = opendir(d);
    if (!dir) return;
    char victims[16][16];
    int nv = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && nv < 16) {
        const char *nm = de->d_name;
        if (strlen(nm) != 12 || strcmp(nm + 8, ".bin") != 0) continue;
        int v = atoi(nm);
        if (v > 19000000 && v < cut) snprintf(victims[nv++], sizeof(victims[0]), "%s", nm);
    }
    closedir(dir);
    for (int i = 0; i < nv; i++) {
        char p[112]; snprintf(p, sizeof(p), "%s/%s", d, victims[i]);
        remove(p);
    }
}

bool hs_crv_put(const char *safe, uint32_t bucket_ts, float v, bool maxfile) {
    char p[96]; crv_path(p, sizeof(p), safe, week_start(bucket_ts), maxfile);
    if (!exists(p)) {                       // nowy tydzien: sprzatnij stare, zrob miejsce
        crv_prune_key(safe, maxfile, bucket_ts);
        hs_ensure_space();
    }
    int r = put_rec(p, bucket_ts, v);
    if (r < 0 && space_problem(r)) { hs_prune(true); r = put_rec(p, bucket_ts, v); }
    return r >= 0;
}

bool hs_crv_put_many(const char *safe, const hist_bucket_t *recs, int n, bool maxfile) {
    bool ok = true;
    int i = 0;
    while (i < n) {
        uint32_t ws = week_start(recs[i].ts);
        char p[96]; crv_path(p, sizeof(p), safe, ws, maxfile);
        if (!exists(p)) hs_ensure_space();
        errno = 0;
        FILE *f = fopen(p, "r+b");
        if (!f && errno == ENOENT && hs_mkdirs(p)) f = fopen(p, "w+b");
        if (!f) return false;
        long k = file_size(f) / REC;
        uint32_t last = 0;
        hist_bucket_t lr;
        if (k > 0 && fseek(f, (k - 1) * REC, SEEK_SET) == 0 && fread(&lr, REC, 1, f) == 1) last = lr.ts;
        fseek(f, k * REC, SEEK_SET);
        for (; i < n && week_start(recs[i].ts) == ws; i++) {
            if (recs[i].ts <= last) continue;
            if (fwrite(&recs[i], REC, 1, f) != 1) { ok = false; break; }
            last = recs[i].ts;
        }
        if (fclose(f) != 0) ok = false;
        if (!ok) return false;
    }
    return ok;
}

int hs_crv_read(const char *safe, uint32_t d0, uint32_t d1,
                hist_bucket_t *out, int cap, bool maxfile) {
    if (d1 <= d0 || cap <= 0) return 0;
    uint32_t ws[2] = { week_start(d0), week_start(d1 - 1) };
    int nw = (ws[1] != ws[0]) ? 2 : 1;
    int n = 0;
    uint32_t prev = 0;
    for (int w = 0; w < nw && n < cap; w++) {
        char p[96]; crv_path(p, sizeof(p), safe, ws[w], maxfile);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        hist_bucket_t chunk[64];
        int r;
        while (n < cap && (r = (int)fread(chunk, REC, 64, f)) > 0) {
            for (int j = 0; j < r && n < cap; j++) {
                if (chunk[j].ts < d0 || chunk[j].ts >= d1) continue;
                if (n > 0 && chunk[j].ts <= prev) continue;
                out[n++] = chunk[j];
                prev = chunk[j].ts;
            }
        }
        fclose(f);
    }
    return n;
}

bool hs_crv_any(const char *safe, uint32_t ts, bool maxfile) {
    char p[96]; crv_path(p, sizeof(p), safe, week_start(ts), maxfile);
    return exists(p);
}

// ======================= utrzymanie miejsca =======================

// Najstarszy plik krzywej (oba katalogi) poza biezacym tygodniem.
static bool delete_oldest_curve(void) {
    uint32_t now = (uint32_t)time(NULL);
    int keep_from = (now >= HS_TS_MIN) ? ymd_of(week_start(now)) : 99999999;
    char best[112] = { 0 };
    int best_v = 0x7FFFFFFF;
    for (int c = 0; c < 2; c++) {
        char d0[64]; snprintf(d0, sizeof(d0), "%s/%c", hs_root, c ? 'm' : 'c');
        DIR *top = opendir(d0);
        if (!top) continue;
        struct dirent *k;
        while ((k = readdir(top)) != NULL) {
            if (k->d_name[0] == '.') continue;
            char d1[112]; snprintf(d1, sizeof(d1), "%s/%.40s", d0, k->d_name);
            DIR *sub = opendir(d1);
            if (!sub) continue;
            struct dirent *e;
            while ((e = readdir(sub)) != NULL) {
                if (strlen(e->d_name) != 12) continue;
                int v = atoi(e->d_name);
                if (v > 19000000 && v < best_v && v < keep_from) {
                    best_v = v;
                    snprintf(best, sizeof(best), "%s/%s", d1, e->d_name);
                }
            }
            closedir(sub);
        }
        closedir(top);
    }
    if (!best[0]) return false;
    HS_LOGW("Brak miejsca - usuwam stara krzywa 5-min %s", best);
    return remove(best) == 0;
}

// Najstarszy miesiac archiwum godzinowego sposrod kluczy majacych >= 2 miesiace.
static bool delete_oldest_month(void) {
    char d0[64]; snprintf(d0, sizeof(d0), "%s/a", hs_root);
    DIR *top = opendir(d0);
    if (!top) return false;
    static int ms[MAX_MONTHS];
    char best[112] = { 0 };
    int best_ym = 0x7FFFFFFF;
    struct dirent *k;
    while ((k = readdir(top)) != NULL) {
        if (k->d_name[0] == '.') continue;
        char safe[HS_SAFE_MAX + 1]; snprintf(safe, sizeof(safe), "%s", k->d_name);
        int n = list_months(safe, ms, MAX_MONTHS);
        if (n >= 2 && ms[0] < best_ym) {
            best_ym = ms[0];
            arc_path(best, sizeof(best), safe, ms[0]);
        }
    }
    closedir(top);
    if (!best[0]) return false;
    HS_LOGW("Brak miejsca - usuwam najstarszy miesiac archiwum %s", best);
    return remove(best) == 0;
}

void hs_prune(bool force) {
    for (int guard = 0; guard < 64; guard++) {
        if (!force && (!hs_free_fn || hs_free_fn() >= HS_FREE_MIN)) return;
        force = false;
        if (delete_oldest_curve()) continue;
        if (delete_oldest_month()) continue;
        return;                                      // nie ma juz czego zwolnic
    }
}

void hs_ensure_space(void) {
    if (hs_free_fn && hs_free_fn() < HS_FREE_MIN) hs_prune(false);
}

// ======================= import starych plikow =======================

#define OLD_ARC_HDR_V1   8
#define OLD_ARC_MAGIC_V2 0x53494832u    // "SIH2"
#define OLD_ARC_HDR_V2   20
#define OLD_ARC_MAGIC_V3 0x53494833u    // "SIH3"
#define OLD_ARC_HDR_V3   24
#define OLD_CRV_MAGIC    0x53494843u    // "SIHC"
#define OLD_CRV_HDR      12

// Ta sama logika co arc_read_header2 z history.c sprzed beta443.
bool hs_old_arc_hdr(void *vf, hs_old_hdr_t *h) {
    FILE *f = (FILE *)vf;
    memset(h, 0, sizeof(*h));
    uint32_t first = 0;
    if (fseek(f, 0, SEEK_SET) != 0 || fread(&first, 4, 1, f) != 1) return false;
    if (first == OLD_ARC_MAGIC_V3) {
        uint32_t cap = 0;
        h->v2 = h->v3 = true; h->hdr = OLD_ARC_HDR_V3;
        if (fread(&cap, 4, 1, f) != 1) return false;
        if (cap < 24 || cap > (uint32_t)HIST_ARCHIVE_HOURS * 4u) return false;
        h->cap = (int)cap;
        if (fread(&h->count, 4, 1, f) != 1 || fread(&h->head, 4, 1, f) != 1 ||
            fread(&h->last_ts, 4, 1, f) != 1 || fread(&h->last_total, 4, 1, f) != 1) return false;
    } else if (first == OLD_ARC_MAGIC_V2) {
        h->v2 = true; h->hdr = OLD_ARC_HDR_V2; h->cap = HIST_ARCHIVE_HOURS_LEGACY;
        if (fread(&h->count, 4, 1, f) != 1 || fread(&h->head, 4, 1, f) != 1 ||
            fread(&h->last_ts, 4, 1, f) != 1 || fread(&h->last_total, 4, 1, f) != 1) return false;
    } else {
        h->hdr = OLD_ARC_HDR_V1; h->cap = HIST_ARCHIVE_HOURS_LEGACY;
        h->count = (int)first;
        if (fread(&h->head, 4, 1, f) != 1) return false;
    }
    if (h->count < 0 || h->count > h->cap || h->head < 0 || h->head >= h->cap) return false;
    long recs = (file_size(f) - h->hdr) / REC;          // nie wiecej, niz plik zawiera
    if (recs < 0) recs = 0;
    if (recs > h->cap) recs = h->cap;
    if (h->count > recs) h->count = (int)recs;
    return true;
}

// Zbieranie rekordow rosnacych w pliki okresow (miesiac albo tydzien). Plik
// okresu powstaje w RAM i idzie do emit() w calosci - tydzien krzywej to
// najwyzej 2016 rekordow (16 KB), miesiac archiwum 744 (6 KB).
#define IMP_BUF_RECS (7 * 288)

typedef struct {
    hist_bucket_t *buf;
    int   n;
    int   key;            // ym (archiwum) albo ymd poczatku tygodnia (krzywa)
    char  rel[64];
    bool  ok;
    int   recs;
    hs_emit_fn emit;
    void *ctx;
} imp_w_t;

static void imp_flush(imp_w_t *w) {
    if (w->n > 0 && !w->emit(w->ctx, w->rel, w->buf, (size_t)w->n * REC)) w->ok = false;
    w->n = 0;
}

static void imp_put(imp_w_t *w, const char *safe, const hist_bucket_t *r, int kind) {
    int key = (kind == 0) ? ym_of(r->ts) : ymd_of(week_start(r->ts));
    if (w->n == 0 || key != w->key) {
        imp_flush(w);
        if (kind == 0)
            snprintf(w->rel, sizeof(w->rel), "a/%s/%04d%02d.bin", safe, key / 12, key % 12 + 1);
        else
            snprintf(w->rel, sizeof(w->rel), "%c/%s/%08d.bin", kind == 2 ? 'm' : 'c', safe, key);
        w->key = key;
    }
    if (w->n >= IMP_BUF_RECS) return;     // nie zdarza sie przy rosnacych ts
    w->buf[w->n++] = *r;
    w->recs++;
}

// Przejscie po pierscieniu w kolejnosci logicznej; rekordy z blednym czasem
// pomijane, ta sama godzina tylko raz (pozniejszy wygrywa), cofniety czas
// pomijany - te same reguly co przy odczycie w history.c sprzed beta443.
static bool imp_ring(FILE *f, int hdr, int cap, int head, int count, uint32_t min_ts,
                     const char *safe, int kind, imp_w_t *w, hist_bucket_t *last_out) {
    static hist_bucket_t chunk[128];
    hist_bucket_t pend = { 0 };
    bool has = false;
    for (int i = 0; i < count; ) {
        int phys = (head + i) % cap;
        int run = cap - phys;
        if (run > count - i) run = count - i;
        if (run > 128) run = 128;
        if (fseek(f, hdr + (long)phys * REC, SEEK_SET) != 0) break;
        int r = (int)fread(chunk, REC, (size_t)run, f);
        for (int j = 0; j < r; j++) {
            const hist_bucket_t *b = &chunk[j];
            if (b->ts < HS_TS_MIN || b->ts > HS_TS_MAX || b->ts < min_ts) continue;
            if (has && b->ts == pend.ts) { pend = *b; continue; }
            if (has && b->ts < pend.ts) continue;
            if (has) imp_put(w, safe, &pend, kind);
            pend = *b; has = true;
        }
        if (r != run) break;
        i += run;
    }
    if (has) { imp_put(w, safe, &pend, kind); *last_out = pend; }
    imp_flush(w);
    return has;
}

// Ostatni rekord logiczny pierscienia (najnowszy).
static bool ring_newest(FILE *f, int hdr, int cap, int head, int count, hist_bucket_t *out) {
    if (count <= 0) return false;
    return fseek(f, hdr + (long)((head + count - 1) % cap) * REC, SEEK_SET) == 0 &&
           fread(out, REC, 1, f) == 1;
}

bool hs_conv_old_arc(const char *src_path, const char *safe, int keep_months,
                     hs_emit_fn emit, void *ctx, int *recs) {
    if (recs) *recs = 0;
    FILE *f = fopen(src_path, "rb");
    if (!f) return false;
    hs_old_hdr_t h;
    if (!hs_old_arc_hdr(f, &h)) { fclose(f); HS_LOGW("Import %s: nieczytelny naglowek", safe); return false; }
    uint32_t min_ts = 0;
    hist_bucket_t nw;
    if (keep_months > 0 && ring_newest(f, h.hdr, h.cap, h.head, h.count, &nw) &&
        nw.ts >= HS_TS_MIN && nw.ts <= HS_TS_MAX) {
        time_t t = nw.ts; struct tm tm;
        localtime_r(&t, &tm);
        tm.tm_mon -= keep_months - 1; tm.tm_mday = 1;
        tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0; tm.tm_isdst = -1;
        min_ts = (uint32_t)mktime(&tm);
    }
    imp_w_t w = { .ok = true, .emit = emit, .ctx = ctx };
    w.buf = malloc(IMP_BUF_RECS * REC);
    if (!w.buf) { fclose(f); return false; }
    hist_bucket_t last = { 0 };
    bool any = imp_ring(f, h.hdr, h.cap, h.head, h.count, min_ts, safe, 0, &w, &last);
    fclose(f);
    free(w.buf);
    if (any) {
        hist_bucket_t lr = last;
        if (h.v2 && h.last_ts >= HS_TS_MIN && h.last_ts <= HS_TS_MAX && h.last_ts >= last.ts) {
            lr.ts = h.last_ts; lr.total = h.last_total;
        }
        char rel[64]; snprintf(rel, sizeof(rel), "a/%s/last.bin", safe);
        if (!emit(ctx, rel, &lr, REC)) w.ok = false;
    }
    if (recs) *recs = w.recs;
    return w.ok;
}

bool hs_conv_old_crv(const char *src_path, const char *safe, bool maxfile,
                     hs_emit_fn emit, void *ctx, int *recs) {
    if (recs) *recs = 0;
    FILE *f = fopen(src_path, "rb");
    if (!f) return false;
    uint32_t magic = 0;
    int count = 0, head = 0;
    const int cap = HIST_CURVE_ARC;
    bool ok = fread(&magic, 4, 1, f) == 1 && magic == OLD_CRV_MAGIC &&
              fread(&count, 4, 1, f) == 1 && fread(&head, 4, 1, f) == 1 &&
              count >= 0 && count <= cap && head >= 0 && head < cap;
    if (!ok) { fclose(f); return false; }
    long inf = (file_size(f) - OLD_CRV_HDR) / REC;
    if (inf < 0) inf = 0;
    if (count > inf && head == 0) count = (int)inf;
    // Granica z danych, nie z zegara (przy przenoszeniu czas bywa jeszcze nieznany):
    // tylko ostatnie HS_CRV_KEEP_DAYS + tydzien przed najnowszym kubelkiem.
    uint32_t min_ts = 0;
    hist_bucket_t newest;
    if (ring_newest(f, OLD_CRV_HDR, cap, head, count, &newest) &&
        newest.ts > (HS_CRV_KEEP_DAYS + 7) * 86400u)
        min_ts = newest.ts - (HS_CRV_KEEP_DAYS + 7) * 86400u;
    imp_w_t w = { .ok = true, .emit = emit, .ctx = ctx };
    w.buf = malloc(IMP_BUF_RECS * REC);
    if (!w.buf) { fclose(f); return false; }
    hist_bucket_t last;
    imp_ring(f, OLD_CRV_HDR, cap, head, count, min_ts, safe, maxfile ? 2 : 1, &w, &last);
    fclose(f);
    free(w.buf);
    if (recs) *recs = w.recs;
    return w.ok;
}

bool hs_emit_file(void *ctx, const char *rel, const void *data, size_t len) {
    (void)ctx;
    char p[112]; snprintf(p, sizeof(p), "%s/%s", hs_root, rel);
    if (!hs_mkdirs(p)) return false;
    FILE *f = fopen(p, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) remove(p);
    return ok;
}

bool hs_import_old_arc(const char *src_path, const char *safe, int *recs) {
    return hs_conv_old_arc(src_path, safe, 0, hs_emit_file, NULL, recs);
}

bool hs_import_old_crv(const char *src_path, const char *safe, bool maxfile, int *recs) {
    return hs_conv_old_crv(src_path, safe, maxfile, hs_emit_file, NULL, recs);
}

// ======================= nazwy z kopii =======================

static bool key_ok(const char *s, size_t n) {
    if (n == 0 || n > HS_SAFE_MAX || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(c == '_' || c == '-' || (c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

bool hs_rel_ok(const char *rel) {
    size_t n = strlen(rel);
    if (strcmp(rel, "tracked.txt") == 0) return true;
    if (n > 6 && strncmp(rel, "h_", 2) == 0 && strcmp(rel + n - 4, ".bin") == 0)
        return key_ok(rel + 2, n - 6);
    if (n < 4 || (rel[0] != 'a' && rel[0] != 'c' && rel[0] != 'm') || rel[1] != '/') return false;
    const char *k = rel + 2;
    const char *sl = strchr(k, '/');
    if (!sl || !key_ok(k, (size_t)(sl - k))) return false;
    const char *fn = sl + 1;
    size_t fl = strlen(fn);
    if (rel[0] == 'a' && strcmp(fn, "last.bin") == 0) return true;
    size_t digits = (rel[0] == 'a') ? 6 : 8;
    if (fl != digits + 4 || strcmp(fn + digits, ".bin") != 0) return false;
    for (size_t i = 0; i < digits; i++) if (fn[i] < '0' || fn[i] > '9') return false;
    return true;
}
