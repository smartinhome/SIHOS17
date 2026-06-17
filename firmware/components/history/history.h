#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Liczba kubelkow per rozdzielczosc (jak HA Energy, z zapasem)
#define HIST_HOURS   168   // 7 dni godzinowo
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
