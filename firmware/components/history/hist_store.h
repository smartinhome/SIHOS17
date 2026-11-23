#pragma once
// beta443: zapis historii na LittleFS - uklad plikow i operacje na nich.
//
// Dlaczego osobny modul: LittleFS jest copy-on-write. Dopisanie na KONCU pliku
// albo nadpisanie ostatniego rekordu kosztuje jeden blok, ale zmiana poczatku
// pliku przepisuje go w calosci. Dawne archiwa z SPIFFS (pierscien + naglowek
// aktualizowany przy kazdym zapisie) byly wiec dla LittleFS najgorszym
// mozliwym wzorcem. Tutaj kazdy plik rosnie tylko na koncu, a stare dane
// sie nie zmieniaja - awaria zapisu moze dotknac najwyzej ostatni rekord.
//
// Uklad (wzgledem hs_root, na module "/spiffs"):
//   a/<klucz>/RRRRMM.bin    archiwum godzinowe: rekordy hist_bucket_t, rosnaco
//   a/<klucz>/last.bin      dokladny czas i stan ostatniego odczytu (8 B)
//   c/<klucz>/RRRRMMDD.bin  krzywa 5-min (srednie), plik na tydzien od poniedzialku
//   m/<klucz>/RRRRMMDD.bin  maksima tych samych kubelkow
// <klucz> to safe_key() z history.c - ten sam, ktory od lat nazywa pliki h_.
//
// Modul nie zalezy od ESP-IDF (stdio + katalogi), wiec da sie go sprawdzic na
// komputerze na zwyklym katalogu.
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "history.h"

#define HS_SAFE_MAX        40      // dluzszy niz SAFE_KEY_MAX - zapas na import
#define HS_CRV_KEEP_DAYS   14      // krzywe 5-min: co najmniej tyle dni wstecz
#define HS_FREE_MIN        (192 * 1024)   // ponizej tego zwalniamy miejsce
#define HS_TS_MIN          1700000000u
#define HS_TS_MAX          2100000000u

extern const char *hs_root;                 // domyslnie "/spiffs"
extern size_t (*hs_free_fn)(void);          // wolne bajty partycji (NULL = bez kontroli)

// ---- archiwum godzinowe ----
// Zapis godziny: nowa godzina dopisywana, ta sama nadpisuje ostatni rekord,
// starsza niz ostatnia jest pomijana (zwraca true - to nie blad zapisu).
// ts_exact >= HS_TS_MIN aktualizuje takze last.bin.
bool hs_arc_put(const char *safe, uint32_t hour_ts, float total, uint32_t ts_exact);
bool hs_arc_last_exact(const char *safe, uint32_t *ts, float *total);
bool hs_arc_first(const char *safe, hist_bucket_t *out);
bool hs_arc_last(const char *safe, hist_bucket_t *out);
int  hs_arc_count(const char *safe, int *months);

// Odczyt rosnaco od OSTATNIEGO rekordu sprzed from_ts (baza dla roznic),
// najdalej do miesiaca zawierajacego to_ts. Wolajacy sam przerywa po ts.
typedef struct {
    char  safe[HS_SAFE_MAX + 1];
    int   ym_next, ym_end;      // kolejny miesiac do otwarcia, ostatni miesiac
    void *f;                    // FILE*
    bool  has_pre;
    hist_bucket_t pre;
} hs_iter_t;
void hs_iter_open(hs_iter_t *it, const char *safe, uint32_t from_ts, uint32_t to_ts);
int  hs_iter_next(hs_iter_t *it, hist_bucket_t *out, int max);
void hs_iter_close(hs_iter_t *it);

// ---- krzywa 5-min ----
bool hs_crv_put(const char *safe, uint32_t bucket_ts, float v, bool maxfile);
// Wiele rekordow naraz (rosnaco) - jedno otwarcie pliku na tydzien. Rekordy
// nie nowsze niz ostatni w pliku sa pomijane.
bool hs_crv_put_many(const char *safe, const hist_bucket_t *recs, int n, bool maxfile);
int  hs_crv_read(const char *safe, uint32_t d0, uint32_t d1,
                 hist_bucket_t *out, int cap, bool maxfile);
bool hs_crv_any(const char *safe, uint32_t ts, bool maxfile);   // jest plik tygodnia ts?

// ---- import plikow z SPIFFS (pierscienie sprzed beta443) ----
// Konwersja oddaje gotowe pliki nowego ukladu przez emit(rel, dane, dlugosc),
// rel wzgledem hs_root (np. "a/<klucz>/202611.bin"). hs_emit_file zapisuje je
// od razu na systemie plikow; przenoszenie z SPIFFS zbiera je w partycji OTA.
typedef bool (*hs_emit_fn)(void *ctx, const char *rel, const void *data, size_t len);
bool hs_emit_file(void *ctx, const char *rel, const void *data, size_t len);
// keep_months > 0: tylko tyle ostatnich miesiecy (liczac od najnowszego rekordu).
bool hs_conv_old_arc(const char *src_path, const char *safe, int keep_months,
                     hs_emit_fn emit, void *ctx, int *recs);
bool hs_conv_old_crv(const char *src_path, const char *safe, bool maxfile,
                     hs_emit_fn emit, void *ctx, int *recs);
// src_path - kopia pliku ha_/hc_/hm_ w dowolnym miejscu. recs - ile rekordow.
bool hs_import_old_arc(const char *src_path, const char *safe, int *recs);
bool hs_import_old_crv(const char *src_path, const char *safe, bool maxfile, int *recs);
// Nazwa pliku nowego ukladu (wzgledem hs_root) bezpieczna do zapisu z kopii:
// a|c|m/<klucz>/<cyfry>.bin, a/<klucz>/last.bin, h_<klucz>.bin, tracked.txt.
bool hs_rel_ok(const char *rel);

// Czytanie naglowka starego archiwum (do przycinania przy przenoszeniu).
typedef struct {
    int count, head, cap, hdr;
    bool v2, v3;
    uint32_t last_ts;
    float last_total;
} hs_old_hdr_t;
bool hs_old_arc_hdr(void *f, hs_old_hdr_t *h);    // f = FILE*

// ---- utrzymanie ----
bool hs_mkdirs(const char *file_path);     // katalogi nadrzedne pliku
void hs_ensure_space(void);                // gdy wolne < HS_FREE_MIN
void hs_prune(bool force);                 // force: zwolnij co najmniej jeden plik
