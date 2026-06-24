#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Liczba kubelkow per rozdzielczosc (jak HA Energy, z zapasem)
#define HIST_HOURS   168   // 7 dni godzinowo (w RAM)
#define HIST_ARCHIVE_HOURS 744  // 31 dni godzinowo (tylko na flash, archiwum)
#define HIST_DAYS    90    // 90 dni
#define HIST_MONTHS  24    // 24 miesiace
#define HIST_YEARS   10    // 10 lat
#define HIST_REALTIME 60   // ostatnie 60 surowych odczytow

// Pojedynczy kubelek: czas poczatku okresu + total licznika na koniec okresu
typedef struct {
    uint32_t ts;      // unix timestamp poczatku okresu (godzina/doba/mc/rok)
    float    total;   // skumulowany stan licznika na koniec tego okresu
} hist_bucket_t;

// Punkt czasu realnego
typedef struct {
    uint32_t ts;
    float    total;
} hist_rt_t;

// Inicjalizacja: montuje SPIFFS, wczytuje zapisana historie wszystkich licznikow.
void history_init(void);

// Wywolywane przy KAZDEJ odebranej ramce z wyciagnietym total.
// id_hex - identyfikator licznika, total - skumulowany stan, kind - 1=m3, 2=kWh.
void history_on_reading(const char *id_hex, double total, int kind, uint32_t ts_unix);

// Per-pole: zapisuje wartosc konkretnego pola licznika (klucz id+pole).
// cumulative=1 (energia/woda/gaz -> wykres roznic), 0 (moc/napiecie -> wartosc).
// Zapisuje TYLKO gdy pole jest sledzone (history_is_tracked("id:pole")).
void history_on_field(const char *id_hex, const char *field, double value,
                      int kind, int cumulative, uint32_t ts_unix);

// Zwraca JSON z historia danego licznika dla danej rozdzielczosci.
// res: "rt"|"hour"|"day"|"month"|"year". Zapisuje do buf (zwraca dlugosc).
// Zwraca zuzycie (roznice) miedzy kolejnymi kubelkami - styl HA Energy.
int history_get_json(const char *id_hex, const char *res, char *buf, int buf_cap);

// Zwraca godzinowe zuzycie z konkretnego dnia (archiwum miesieczne na flash).
// day_ts: dowolny unix timestamp z wybranego dnia (uzywany jest floor do doby).
// Format JSON jak history_get_json: {"id":..,"kind":..,"cumulative":..,"points":[{"t":..,"v":..}]}
int history_get_day_json(const char *id_hex, uint32_t day_ts, char *buf, int buf_cap);

// Lista licznikow ktore maja historie (dla zakladki). JSON tablica.
int history_list_json(char *buf, int buf_cap);

// Ostatni znany stan licznika (do trwalosci - pokaz po restarcie zanim wplynie ramka).
// Zwraca true gdy znaleziono. out_total, out_kind, out_ts wypelniane.
bool history_last_known(const char *id_hex, double *out_total, int *out_kind, uint32_t *out_ts);

// Sledzenie: ktore liczniki pokazywac w Historii (reszta np. sasiedzi - ukryte).
void history_set_tracked(const char *id_hex, bool tracked);
bool history_is_tracked(const char *id_hex);

// Lista sledzonych kluczy (id:pole) jako JSON tablica stringow.
int history_tracked_json(char *buf, int buf_cap);

// ----- API dla wyswietlacza e-ink -----

// Podsumowanie jednego sledzonego pola/licznika dla ekranu.
typedef struct {
    char     key[28];       // klucz historii (id lub id:pole)
    int      kind;          // 1=woda, 2=prad, 3=gaz
    int      cumulative;    // 1=kumulacyjne (woda/energia/gaz), 0=chwilowe (moc/napiecie)
    bool     has_value;     // czy jest jakikolwiek odczyt
    double   last_total;    // biezacy stan licznika (lub wartosc chwilowa)
    uint32_t last_ts;       // czas ostatniego odczytu
    bool     has_today;     // czy mozna policzyc zuzycie dzis
    double   today;         // zuzycie dzis (cumulative) lub wartosc (chwilowe)
    bool     has_yesterday;
    double   yesterday;     // zuzycie wczoraj
    bool     has_day_before;
    double   day_before;    // zuzycie przedwczoraj
} hist_display_t;

// Wypelnia podsumowanie dla danego klucza. Zwraca true gdy licznik istnieje.
bool history_display_summary(const char *key, hist_display_t *out);

// Liczba unikalnych licznikow (ID) ze sledzonymi polami - liczba stron na e-ink.
// Wypelnia tablice unikalnych ID (do max_ids). Zwraca liczbe.
int history_tracked_meter_ids(char ids[][12], int max_ids);

// Wypelnia pelne klucze sledzenia (id:pole lub id) dla danego ID licznika.
// keys: tablica bufow [n][28]. Zwraca liczbe znalezionych kluczy.
int history_keys_for_id(const char *id, char keys[][28], int max_keys);

// Diagnostyka dla ekranu: liczba sledzonych kluczy + pierwszy klucz (do podgladu).
int history_tracked_count(void);
void history_tracked_first(char *out, int cap);
