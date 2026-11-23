// beta443: przeniesienie historii z SPIFFS na LittleFS.
//
// Obie wersje uzywaja tej samej partycji, wiec dane trzeba najpierw wyniesc.
// Miejsce robocze to druga partycja aplikacji (OTA) - po starcie nowej wersji
// stara aplikacja nie jest juz potrzebna, a partycja ma 1,75 MB. Kolejnosc:
//   1. SPIFFS czytany tylko do odczytu; pliki zamieniane od razu na nowy
//      uklad (miesiace archiwum, tygodnie krzywych) i zapisywane w partycji
//      OTA jako ciag wpisow [u8 dl. nazwy][nazwa][u32 dl. danych][dane].
//   2. Odczyt kontrolny calosci (CRC), dopiero potem naglowek kopii.
//   3. NVS "mig"=1 - od tej chwili kopia robocza jest jedynym zrodlem.
//   4. Formatowanie LittleFS, wgranie plikow, "mig"=2.
// Zanik zasilania w kroku 1-2: SPIFFS nietkniety, przy starcie wszystko od
// nowa. W kroku 4: przy starcie formatowanie i wgrywanie jeszcze raz.
#include <stdio.h>
// Dlugosci nazw z readdir sa sprawdzane przed skladaniem sciezek - GCC tego
// nie widzi i zglasza mozliwe obciecie przy kazdym snprintf z d_name.
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_littlefs.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "hist_store.h"
#include "hist_migrate.h"

static const char *TAG = "HMIGR";

#define LABEL      "littlefs"
#define BASE       "/spiffs"
#define STG_MAGIC  0x31474D53u      // "SMG1"
#define STG_VER    1u
#define SEC        4096
#define STG_DATA   SEC              // dane od drugiego sektora, naglowek w pierwszym
#define RAW_MAX    (16 * 1024)      // najwiekszy plik h_ przenoszony wprost

typedef struct {
    uint32_t magic, ver, len, crc, files, reserved;
} stg_hdr_t;

typedef struct {
    const esp_partition_t *p;
    uint8_t *buf;          // bufor sektora
    size_t   fill;         // bajtow w buforze
    size_t   off;          // adres nastepnego sektora w partycji
    uint32_t len, crc;
    bool     ok;
    int      files;
} stg_t;

// ---------------- NVS ----------------

static uint8_t nvs_u8(const char *k, uint8_t def) {
    nvs_handle_t h;
    uint8_t v = def;
    if (nvs_open("sih_fs", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, k, &v) != ESP_OK) v = def;
        nvs_close(h);
    }
    return v;
}

static void nvs_set(const char *k, uint8_t v) {
    nvs_handle_t h;
    if (nvs_open("sih_fs", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, k, v);
        nvs_commit(h);
        nvs_close(h);
    }
}

// ---------------- zapis kopii roboczej ----------------

static void stg_reset(stg_t *s) {
    s->fill = 0; s->off = STG_DATA; s->len = 0; s->crc = 0; s->ok = true; s->files = 0;
}

static bool stg_flush(stg_t *s) {
    if (!s->ok || s->fill == 0) return s->ok;
    if (s->off + SEC > s->p->size) { s->ok = false; return false; }
    if (esp_partition_erase_range(s->p, s->off, SEC) != ESP_OK ||
        esp_partition_write(s->p, s->off, s->buf, s->fill) != ESP_OK) {
        ESP_LOGE(TAG, "Zapis kopii roboczej nieudany (adres 0x%x)", (unsigned)s->off);
        s->ok = false;
        return false;
    }
    s->crc = esp_rom_crc32_le(s->crc, s->buf, s->fill);
    s->len += s->fill;
    s->off += SEC;
    s->fill = 0;
    return true;
}

static bool stg_put(stg_t *s, const void *d, size_t n) {
    const uint8_t *b = (const uint8_t *)d;
    while (n > 0 && s->ok) {
        size_t take = SEC - s->fill;
        if (take > n) take = n;
        memcpy(s->buf + s->fill, b, take);
        s->fill += take; b += take; n -= take;
        if (s->fill == SEC) stg_flush(s);
    }
    return s->ok;
}

static bool stg_entry(void *ctx, const char *rel, const void *data, size_t len) {
    stg_t *s = (stg_t *)ctx;
    size_t nl = strlen(rel);
    if (nl == 0 || nl > 63) return true;
    uint8_t n8 = (uint8_t)nl;
    uint32_t l32 = (uint32_t)len;
    stg_put(s, &n8, 1);
    stg_put(s, rel, nl);
    stg_put(s, &l32, 4);
    stg_put(s, data, len);
    if (s->ok) s->files++;
    return s->ok;
}

static bool ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

// Klucz z nazwy starego pliku "<pfx><klucz>.bin".
static bool old_key(const char *nm, size_t pfx, char *out) {
    size_t n = strlen(nm);
    if (n <= pfx + 4 || n - pfx - 4 > HS_SAFE_MAX) return false;
    memcpy(out, nm + pfx, n - pfx - 4);
    out[n - pfx - 4] = 0;
    return true;
}

// Jedno podejscie: keep_months = 0 cale archiwum, curves = czy krzywe 5-min.
static bool stage_attempt(stg_t *s, uint8_t *raw, int keep_months, bool curves,
                          int *n_arc, int *n_crv) {
    stg_reset(s);
    *n_arc = 0; *n_crv = 0;
    for (int pass = 0; pass < 3 && s->ok; pass++) {
        if (pass == 2 && !curves) break;
        DIR *d = opendir(BASE);
        if (!d) { s->ok = false; break; }
        struct dirent *de;
        while ((de = readdir(d)) != NULL && s->ok) {
            const char *nm = de->d_name;
            char path[300];
            snprintf(path, sizeof(path), BASE "/%s", nm);
            char key[HS_SAFE_MAX + 1];
            if (pass == 0) {
                bool want = (strncmp(nm, "h_", 2) == 0 && ends_with(nm, ".bin")) ||
                            strcmp(nm, "tracked.txt") == 0 || strcmp(nm, "rebase.flg") == 0;
                if (!want || strlen(nm) > 63) continue;
                FILE *f = fopen(path, "rb");
                if (!f) continue;
                size_t n = fread(raw, 1, RAW_MAX, f);
                bool more = fgetc(f) != EOF;
                fclose(f);
                if (more) { ESP_LOGW(TAG, "%s za duzy - pomijam", nm); continue; }
                stg_entry(s, nm, raw, n);
            } else if (pass == 1) {
                if (strncmp(nm, "ha_", 3) != 0 || !ends_with(nm, ".bin") || !old_key(nm, 3, key)) continue;
                int r = 0;
                if (!hs_conv_old_arc(path, key, keep_months, stg_entry, s, &r) && s->ok)
                    ESP_LOGW(TAG, "Archiwum %s nieczytelne - pomijam", nm);
                *n_arc += r;
            } else {
                bool mx = strncmp(nm, "hm_", 3) == 0;
                if ((!mx && strncmp(nm, "hc_", 3) != 0) || !ends_with(nm, ".bin") || !old_key(nm, 3, key)) continue;
                int r = 0;
                hs_conv_old_crv(path, key, mx, stg_entry, s, &r);
                *n_crv += r;
            }
        }
        closedir(d);
    }
    uint8_t z = 0;
    stg_put(s, &z, 1);
    stg_flush(s);
    return s->ok;
}

// Odczyt kontrolny: CRC tego, co faktycznie lezy w partycji.
static bool stg_crc(const esp_partition_t *p, uint32_t len, uint32_t *crc_out, uint8_t *buf) {
    uint32_t crc = 0, done = 0;
    while (done < len) {
        uint32_t n = len - done > SEC ? SEC : len - done;
        // dane leza sektor po sektorze; tylko ostatni jest niepelny
        if (esp_partition_read(p, STG_DATA + done, buf, n) != ESP_OK) return false;
        crc = esp_rom_crc32_le(crc, buf, n);
        done += n;
    }
    *crc_out = crc;
    return true;
}

static bool stg_valid(const esp_partition_t *p, stg_hdr_t *h) {
    if (esp_partition_read(p, 0, h, sizeof(*h)) != ESP_OK) return false;
    if (h->magic != STG_MAGIC || h->ver != STG_VER) return false;
    if (h->len == 0 || h->len > p->size - STG_DATA) return false;
    uint8_t *buf = malloc(SEC);
    if (!buf) return false;
    uint32_t crc = 0;
    bool ok = stg_crc(p, h->len, &crc, buf) && crc == h->crc;
    free(buf);
    return ok;
}

static bool stage_all(const esp_partition_t *p, stg_hdr_t *hdr) {
    static const struct { int keep; bool curves; } plan[] = {
        { 0, true }, { 0, false }, { 24, false }, { 12, false }, { 3, false },
    };
    stg_t s = { .p = p };
    s.buf = malloc(SEC);
    uint8_t *raw = malloc(RAW_MAX);
    if (!s.buf || !raw) { free(s.buf); free(raw); return false; }
    int64_t t0 = esp_timer_get_time();
    bool ok = false;
    int n_arc = 0, n_crv = 0;
    for (size_t i = 0; i < sizeof(plan) / sizeof(plan[0]) && !ok; i++) {
        ok = stage_attempt(&s, raw, plan[i].keep, plan[i].curves, &n_arc, &n_crv);
        if (!ok)
            ESP_LOGW(TAG, "Kopia robocza sie nie miesci (archiwum %d mies., krzywe %s) - probuje mniej",
                     plan[i].keep, plan[i].curves ? "tak" : "nie");
        else if (i > 0)
            ESP_LOGW(TAG, "Przenoszone: archiwum %s, krzywe 5-min %s",
                     plan[i].keep ? "ostatnie miesiace" : "w calosci", plan[i].curves ? "tak" : "nie");
    }
    uint32_t crc = 0;
    if (ok && (!stg_crc(p, s.len, &crc, s.buf) || crc != s.crc)) {
        ESP_LOGE(TAG, "Kopia robocza: odczyt kontrolny sie nie zgadza");
        ok = false;
    }
    if (ok) {
        memset(hdr, 0, sizeof(*hdr));
        hdr->magic = STG_MAGIC; hdr->ver = STG_VER;
        hdr->len = s.len; hdr->crc = s.crc; hdr->files = (uint32_t)s.files;
        memset(s.buf, 0xFF, SEC);
        memcpy(s.buf, hdr, sizeof(*hdr));
        ok = esp_partition_erase_range(p, 0, SEC) == ESP_OK &&
             esp_partition_write(p, 0, s.buf, sizeof(*hdr)) == ESP_OK;
        stg_hdr_t chk;
        if (ok) ok = stg_valid(p, &chk);
    }
    if (ok)
        ESP_LOGW(TAG, "Kopia robocza gotowa: %d plikow, %u B, godzin %d, punktow 5-min %d, %u s",
                 s.files, (unsigned)s.len, n_arc, n_crv,
                 (unsigned)((esp_timer_get_time() - t0) / 1000000));
    free(s.buf);
    free(raw);
    return ok;
}

// ---------------- wgranie na LittleFS ----------------

typedef struct {
    const esp_partition_t *p;
    uint8_t *buf;
    uint32_t pos, end;      // pozycja wzgledem STG_DATA
    uint32_t bpos, blen;    // okno bufora
} rd_t;

static bool rd_get(rd_t *r, void *out, uint32_t n) {
    uint8_t *o = (uint8_t *)out;
    while (n > 0) {
        if (r->pos >= r->end) return false;
        if (r->pos < r->bpos || r->pos >= r->bpos + r->blen) {
            r->bpos = r->pos;
            r->blen = r->end - r->pos > SEC ? SEC : r->end - r->pos;
            if (esp_partition_read(r->p, STG_DATA + r->bpos, r->buf, r->blen) != ESP_OK) return false;
        }
        uint32_t avail = r->bpos + r->blen - r->pos;
        uint32_t take = avail < n ? avail : n;
        memcpy(o, r->buf + (r->pos - r->bpos), take);
        r->pos += take; o += take; n -= take;
    }
    return true;
}

static int restore_stg(const esp_partition_t *p, const stg_hdr_t *h, int *failed) {
    rd_t r = { .p = p, .end = h->len };
    r.buf = malloc(SEC);
    uint8_t *tmp = malloc(SEC);
    int ok_n = 0;
    *failed = 0;
    if (!r.buf || !tmp) { free(r.buf); free(tmp); *failed = 1; return 0; }
    while (1) {
        uint8_t nl;
        if (!rd_get(&r, &nl, 1) || nl == 0) break;
        char rel[64] = { 0 };
        uint32_t len;
        if (nl > 63 || !rd_get(&r, rel, nl) || !rd_get(&r, &len, 4) || len > h->len) { (*failed)++; break; }
        bool allowed = hs_rel_ok(rel) || strcmp(rel, "rebase.flg") == 0;
        char path[112];
        snprintf(path, sizeof(path), BASE "/%s", rel);
        FILE *f = NULL;
        if (allowed && hs_mkdirs(path)) f = fopen(path, "wb");
        bool wok = f != NULL;
        uint32_t left = len;
        bool rd_ok = true;
        while (left > 0) {
            uint32_t n = left > SEC ? SEC : left;
            if (!rd_get(&r, tmp, n)) { rd_ok = false; break; }
            if (wok && fwrite(tmp, 1, n, f) != n) wok = false;
            left -= n;
        }
        if (f && fclose(f) != 0) wok = false;
        if (!allowed) continue;
        if (wok && rd_ok) ok_n++;
        else { (*failed)++; remove(path); ESP_LOGE(TAG, "Wgranie %s nieudane", rel); }
        if (!rd_ok) break;
    }
    free(r.buf);
    free(tmp);
    return ok_n;
}

static bool mount_lfs(void) {
    esp_vfs_littlefs_conf_t c = {
        .base_path = BASE,
        .partition_label = LABEL,
        .format_if_mount_failed = false,
    };
    return esp_vfs_littlefs_register(&c) == ESP_OK;
}

static bool format_and_mount(void) {
    esp_err_t e = esp_littlefs_format(LABEL);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Formatowanie LittleFS nieudane: %s", esp_err_to_name(e));
        return false;
    }
    return mount_lfs();
}

static bool finish(const esp_partition_t *p, const stg_hdr_t *h) {
    uint8_t tries = nvs_u8("migtry", 0);
    if (tries >= 3) {
        // Trzy przerwane proby z rzedu - cos stale wywraca wgrywanie. Lepiej
        // historia od zera niz modul restartujacy sie w kolko.
        ESP_LOGE(TAG, "Przenoszenie historii przerwane 3 razy - pamiec historii od zera");
        bool m = format_and_mount();
        nvs_set("mig", 2);
        nvs_set("migtry", 0);
        return m;
    }
    nvs_set("migtry", tries + 1);
    int64_t t0 = esp_timer_get_time();
    if (!format_and_mount()) return false;
    int failed = 0;
    int n = restore_stg(p, h, &failed);
    nvs_set("mig", 2);
    nvs_set("migtry", 0);
    size_t t = 0, u = 0;
    esp_littlefs_info(LABEL, &t, &u);
    ESP_LOGW(TAG, "Historia przeniesiona na LittleFS: %d plikow, nieudanych %d, zajete %u/%u B, %u s",
             n, failed, (unsigned)u, (unsigned)t,
             (unsigned)((esp_timer_get_time() - t0) / 1000000));
    return true;
}

size_t hist_fs_free(void) {
    size_t t = 0, u = 0;
    if (esp_littlefs_info(LABEL, &t, &u) != ESP_OK) return SIZE_MAX;
    return t > u ? t - u : 0;
}

bool hist_fs_mount(void) {
    hs_root = BASE;
    const esp_partition_t *stp = esp_ota_get_next_update_partition(NULL);
    uint8_t mig = nvs_u8("mig", 0);
    stg_hdr_t h;

    if (mig == 1) {
        if (stp && stg_valid(stp, &h)) {
            ESP_LOGW(TAG, "Przenoszenie historii przerwane - wgrywam ponownie z kopii roboczej");
            return finish(stp, &h);
        }
        ESP_LOGE(TAG, "Kopia robocza przenoszenia nieczytelna");
        nvs_set("mig", 0);
    }

    if (mount_lfs()) {
        if (mig != 2) nvs_set("mig", 2);
        return true;
    }

    esp_vfs_spiffs_conf_t sc = {
        .base_path = BASE,
        .partition_label = LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    if (esp_vfs_spiffs_register(&sc) == ESP_OK) {
        size_t t = 0, u = 0;
        esp_spiffs_info(LABEL, &t, &u);
        // Kopia robocza nadpisuje poprzednia aplikacje. Gdy nowa wersja nie jest
        // jeszcze potwierdzona, bootloader moglby do niej wrocic - wtedy czekamy.
        esp_ota_img_states_t rs = ESP_OTA_IMG_UNDEFINED;
        const esp_partition_t *run = esp_ota_get_running_partition();
        if (run && esp_ota_get_state_partition(run, &rs) == ESP_OK && rs == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGE(TAG, "Aplikacja niepotwierdzona - przenoszenie historii przy nastepnym starcie");
            esp_vfs_spiffs_unregister(LABEL);
            return false;
        }
        ESP_LOGW(TAG, "Historia na SPIFFS (zajete %u B) - przenoszenie na LittleFS, nie odlaczaj zasilania",
                 (unsigned)u);
        bool ok = stp && stage_all(stp, &h);
        esp_vfs_spiffs_unregister(LABEL);
        if (!ok) {
            ESP_LOGE(TAG, "Przenoszenie historii nieudane - historia wylaczona, dane na partycji nietkniete");
            return false;
        }
        nvs_set("migtry", 0);
        nvs_set("mig", 1);
        return finish(stp, &h);
    }

    ESP_LOGW(TAG, "Partycja historii pusta albo nieczytelna - zakladam LittleFS");
    if (!format_and_mount()) return false;
    nvs_set("mig", 2);
    return true;
}
