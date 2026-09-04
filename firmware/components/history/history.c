#include "history.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>       // fsync, unlink, fileno
#include <sys/stat.h>     // stat() dla recovery sierotek .tmp
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"

// FAZA 3a: helper do atomic write. Przyjmuje docelowa sciezke z ".bin" na koncu,
// zwraca sciezke z ".tmp" (ta sama dlugosc - SPIFFS ogranicza nazwy do 31 znakow,
// wiec dodawanie sufiksu ".tmp" do juz maksymalnej sciezki by ja przekroczylo).
// Bez atomic write reset zasilania w srodku fwrite() ucinal plik - historia dnia
// (lub calego licznika) znikala. Z atomic: reset zostawia ".tmp" jako smiec ALE
// oryginalny ".bin" jest nietkniety, wiec przy nastepnym starcie modul ma pelna
// wersje historii sprzed zapisu.
static void path_to_tmp(const char *path, char *tmp, size_t cap) {
    // Zamien koncowke ".bin" na ".tmp". Zaklada ze wszystkie h_/ha_/tracked pliki
    // koncza sie na ".bin" (tracked.txt obsluzone osobno - patrz tracked_save).
    strlcpy(tmp, path, cap);
    size_t n = strlen(tmp);
    if (n >= 4 && strcmp(tmp + n - 4, ".bin") == 0) {
        memcpy(tmp + n - 4, ".tmp", 4);
    } else {
        // Fallback: dopisz .tmp gdyby ktos zmienil rozszerzenie
        strlcat(tmp, ".tmp", cap);
    }
}

// Wymus flush z bufora C stdio i z bufora VFS/SPIFFS na flash. Wolane przed
// atomic rename - bez tego dane wisza w bufirze VFS a rename podmienialby
// "pusty" plik. SPIFFS wspiera fsync w ESP-IDF.
static void flush_to_flash(FILE *f) {
    if (!f) return;
    fflush(f);
    int fd = fileno(f);
    if (fd >= 0) fsync(fd);
}

static const char *TAG = "HISTORY";
// Tyle samo, ile MAX_TRACKED - inaczej czesc sledzonych pol nie dostawala
// slotu i po cichu nie miala historii (Amiplus daje ~20 pol po becie 257).
// Sloty sa alokowane NA ZADANIE, wiec zapas nie kosztuje RAM dopoki nieuzyty.
#define MAX_HIST_METERS 24

// Stan historii pojedynczego licznika
typedef struct {
    char     id[40];          // klucz: "id" lub "id:pole" (np. 56989134:moc_kw)
                              // 40 znakow - patrz s_tracked; pole NIE trafia
                              // na flash (jest w nazwie pliku), wiec zmiana
                              // rozmiaru nie uniewaznia istniejacych h_*.bin
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
    // Krzywa dnia (styl Home Assistant) dla pol CHWILOWYCH (moc, napiecie):
    // SREDNIA z ramek w kazdym kubelku 5-MINUTOWYM, ring 24h (288 pkt).
    // Bufor DYNAMICZNY (2.3KB) - alokowany tylko dla pol, ktore go uzywaja.
    // FAZA 5a zmiana: wczesniej 1440 pkt LWW (last-write-wins), teraz 288 pkt
    // ze srednia obliczana on-the-fly (running sum + count przy przejsciu bucketu).
    hist_bucket_t *curve;              int n_curve;
    // Running average state dla BIEZACEGO (jeszcze niezamknietego) kubelka curve.
    // Kazda nowa ramka: curve_sum += value, curve_count++. Przy zmianie bucket_ts:
    // srednia = sum/count, zapisuje sie do curve[], reset sum/count.
    // TYLKO W RAM (nie zapisywane) - stan tymczasowy jednego okna.
    float    curve_sum;
    int      curve_count;
    uint32_t curve_bucket_ts;          // TS aktualnego 5-min bucket (0 = brak)
    // FAZA 5a: krzywa dnia POPRZEDNIEGO - dla wykresu wczoraj (symetria z dzis).
    // Rotacja: przy zmianie doby (floor_day) kopiujemy curve -> curve_yesterday,
    // ustawiamy curve_yesterday_day_ts = poprzednia doba, reset curve.
    // Bufor DYNAMICZNY alokowany PRZY PIERWSZEJ ROTACJI (leniwie) - pierwszego
    // dnia po flashu jest NULL, wczoraj fallback do archiwum godzinnego (jak dotad).
    hist_bucket_t *curve_yesterday;    int n_curve_yesterday;
    uint32_t curve_yesterday_day_ts;   // TS doby (floor_day) dla ktorej to jest wczoraj
    // Stan licznika o polnocy - REZERWA na pierwsze godziny po instalacji, gdy
    // nie ma jeszcze kubelka sprzed polnocy. TYLKO W RAM (nie zapisywane): po
    // restarcie w srodku dnia baza jest nieznana i liczenie MUSI spasc na
    // kubelki godzinowe - inaczej za punkt odniesienia wzielibysmy stan sprzed
    // restartu i "dzis" spadloby do zera (regresja z bety 264).
    bool     rebased;                  // czy juz obsluzono przywrocenie kopii
    uint32_t rebase_ts;                // granica przywrocenia (0 = brak)
    uint32_t day_base_ts;
    float    day_base_total;
    uint32_t last_save_ts;             // ostatni zapis h_ (zapis okresowy, RAM-only)
    uint32_t last_arc_ts;              // ostatni zapis ha_ (throttling, RAM-only)
} meter_hist_t;

// Sloty historii alokowane NA ZADANIE (2.9KB kazdy). Wczesniej 16 pelnych
// struktur siedzialo w .bss = 46.6KB zajete od startu, niezaleznie od tego
// ile licznikow jest realnie sledzonych (konfiguracja dopuszcza max 8).
static meter_hist_t *s_meters[MAX_HIST_METERS];

// Zwraca slot i (alokujac go przy pierwszym uzyciu) albo NULL przy braku RAM.
static meter_hist_t *slot_get(int i) {
    if (i < 0 || i >= MAX_HIST_METERS) return NULL;
    if (!s_meters[i]) {
        s_meters[i] = calloc(1, sizeof(meter_hist_t));
        if (!s_meters[i]) ESP_LOGE(TAG, "brak RAM na slot historii %d", i);
    }
    return s_meters[i];
}
// Lista sledzonych licznikow (pokazywane w Historii). Reszta (sasiedzi) ukryta.
#define MAX_TRACKED 24
// 40 znakow: najdluzszy klucz to id(8)+':'+nazwa pola, np.
// "56989134:energia_bierna_c_kvarh" (31). Przy 28 klucze byly po cichu
// obcinane i sledzenie nie dzialalo (np. poprzedni_miesiac_m3 dla IZAR).
static char s_tracked[MAX_TRACKED][40];
static int  s_tracked_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static bool s_fs_ok = false;
// Po przywroceniu kopii stan licznika zdazyl sie zmienic, a zuzycie z tego
// okresu NIE zostalo zarejestrowane. Bez tego firmware probowal je dopisac do
// dzisiejszych godzin (jeden ogromny slupek albo kilka identycznych po rozlozeniu
// luki). Znacznik mowi: przy pierwszej ramce po restarcie zacznij dobe od nowa.
#define REBASE_FLAG "/spiffs/rebase.flg"
static bool s_rebase_pending = false;

// ---------- Pomocnicze: zaokraglenia czasu ----------
static uint32_t floor_hour(uint32_t t)  { return t - (t % 3600); }
// Alokacja bufora krzywej na zadanie; NULL-safe (bez krzywej gdy brak RAM).
// Zapas sterty, ktory musi zostac PO alokacji krzywej (11.2 KB kazda).
// Amiplus ma ~10 pol chwilowych; bez tego progu zaznaczenie ich wszystkich
// zjadloby ponad 100 KB i wywrocilo moduł. Brak krzywej nie psuje wykresu -
// dzien rysuje sie wtedy z kubelkow godzinowych (grubsza rozdzielczosc).
// Bufor na JSON doby przy liczeniu sumy dla e-ink: max ~48 slupkow po ~30 B.
#define DAY_SUM_BUF 3072

#define CURVE_HEAP_RESERVE (60 * 1024)

static bool ensure_curve(meter_hist_t *m) {
    if (m->curve) return true;
    size_t need = HIST_CURVE * sizeof(hist_bucket_t);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeb < need + CURVE_HEAP_RESERVE) {
        ESP_LOGW(TAG, "%s: brak RAM na krzywa minutowa (wolne %u B) - dane godzinowe",
                 m->id, (unsigned)freeb);
        m->n_curve = 0;
        return false;
    }
    m->curve = calloc(HIST_CURVE, sizeof(hist_bucket_t));
    if (!m->curve) { m->n_curve = 0; return false; }
    return true;
}
static void free_curve(meter_hist_t *m) {
    if (m->curve) { free(m->curve); m->curve = NULL; }
    m->n_curve = 0;
    // FAZA 5a: curve_yesterday zwalniane razem - alokowane parami przy rotacji.
    if (m->curve_yesterday) { free(m->curve_yesterday); m->curve_yesterday = NULL; }
    m->n_curve_yesterday = 0;
    m->curve_yesterday_day_ts = 0;
    m->curve_sum = 0.0f;
    m->curve_count = 0;
    m->curve_bucket_ts = 0;
}

// Forward declarations dla curve_add_sample_avg (uzywa floor_day/series_update
// ktore sa zdefiniowane pozniej w tym pliku).
static uint32_t floor_day(uint32_t t);
static void series_update(hist_bucket_t *arr, int *n, int cap, uint32_t bts, float total);
// FAZA 9b: dopisanie ZAMKNIETEGO kubelka 5-min do archiwum hc_*.bin na flashu
// (definicja przy pozostalych funkcjach archiwum - potrzebuje safe_key()).
static void curve_archive_append(const char *id, uint32_t bucket_ts, float value);

// FAZA 5a: dodaje probke do krzywej dnia z running average.
// - Detekcja przejscia bucketu 5-min: zamyka poprzedni sredniej (sum/count),
//   otwiera nowy z reset sum/count.
// - Detekcja przejscia doby (nowy day floor): kopiuje curve -> curve_yesterday
//   (leniwa alokacja przy pierwszej rotacji), resetuje curve dla nowej doby.
// - Kazda ramka aktualizuje curve[bucket] BIEŻĄCĄ srednia - dzieki temu wykres
//   biezacego bucketu ~"zmienia sie" w czasie rzeczywistym, nie zawiesza na starej.
static void curve_add_sample_avg(meter_hist_t *m, float value, uint32_t ts_unix) {
    if (!ensure_curve(m)) return;
    uint32_t bucket_ts = ts_unix - (ts_unix % HIST_CURVE_INTERVAL_SEC);

    // 1. Detekcja przejscia doby - rotacja curve -> curve_yesterday.
    if (m->curve_bucket_ts != 0) {
        uint32_t old_day = floor_day(m->curve_bucket_ts);
        uint32_t new_day = floor_day(ts_unix);
        if (old_day != new_day) {
            // Zamknij ostatni bucket poprzedniej doby (jesli byla otwarta srednia).
            if (m->curve_count > 0) {
                float avg = m->curve_sum / (float)m->curve_count;
                series_update(m->curve, &m->n_curve, HIST_CURVE,
                              m->curve_bucket_ts, avg);
                // FAZA 9b: ostatni kubelek doby trafia na flash zanim curve[]
                // zostanie wyczyszczona pod nowa dobe.
                curve_archive_append(m->id, m->curve_bucket_ts, avg);
            }
            // Alokuj curve_yesterday jesli jeszcze nie ma (pierwszy dzien po flashu = NULL).
            if (!m->curve_yesterday) {
                m->curve_yesterday = calloc(HIST_CURVE, sizeof(hist_bucket_t));
            }
            if (m->curve_yesterday) {
                memcpy(m->curve_yesterday, m->curve, HIST_CURVE * sizeof(hist_bucket_t));
                m->n_curve_yesterday = m->n_curve;
                m->curve_yesterday_day_ts = old_day;
                ESP_LOGI(TAG, "%s: curve->curve_yesterday (%d pkt, day_ts=%u)",
                         m->id, m->n_curve_yesterday, (unsigned)old_day);
            } else {
                ESP_LOGW(TAG, "%s: brak RAM na curve_yesterday - wczoraj z godzinnego", m->id);
                m->n_curve_yesterday = 0;
            }
            // Reset curve na nowa dobe.
            m->n_curve = 0;
            m->curve_sum = 0.0f;
            m->curve_count = 0;
            m->curve_bucket_ts = 0;
            memset(m->curve, 0, HIST_CURVE * sizeof(hist_bucket_t));
        }
    }

    // 2. Detekcja przejscia bucketu 5-min - zamknij poprzedni (final srednia).
    if (m->curve_bucket_ts != bucket_ts) {
        if (m->curve_count > 0 && m->curve_bucket_ts != 0) {
            float avg = m->curve_sum / (float)m->curve_count;
            series_update(m->curve, &m->n_curve, HIST_CURVE,
                          m->curve_bucket_ts, avg);
            // FAZA 9b: kubelek jest domkniety - zapisz go do archiwum 5-min.
            // Jeden zapis na 5 min na pole (60x rzadziej niz archiwum godzinowe,
            // ktore pisze co ramke) - zuzycie flasha praktycznie bez zmian.
            curve_archive_append(m->id, m->curve_bucket_ts, avg);
        }
        m->curve_bucket_ts = bucket_ts;
        m->curve_sum = 0.0f;
        m->curve_count = 0;
    }

    // 3. Dodaj probke do biezacej sredniej + zapisz partial average do curve
    //    (dzieki temu bufor pokazuje aktualna srednia BIEZACEGO bucketu, nie
    //    zamknieta poprzednia).
    m->curve_sum += value;
    m->curve_count++;
    series_update(m->curve, &m->n_curve, HIST_CURVE,
                  bucket_ts, m->curve_sum / (float)m->curve_count);
}

// tm_isdst = -1: pozwol mktime wyliczyc czy w tym momencie obowiazuje CEST czy CET.
// Bez tego w dobie zmiany czasu (marzec/pazdziernik) mktime przyjmowal biezacy DST
// dla polnocy poprzedniej doby - kubelek doby przesuwal sie o 1 h, rebase w
// history_on_reading (day_base_ts != d0_now) potrafil zdublowac lub zgubic slupek.
static uint32_t floor_day(uint32_t t) {
    time_t tt = t; struct tm tm; localtime_r(&tt, &tm);
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (uint32_t)mktime(&tm);
}
static uint32_t floor_month(uint32_t t) {
    time_t tt = t; struct tm tm; localtime_r(&tt, &tm);
    tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (uint32_t)mktime(&tm);
}
static uint32_t floor_year(uint32_t t) {
    time_t tt = t; struct tm tm; localtime_r(&tt, &tm);
    tm.tm_mon = 0; tm.tm_mday = 1; tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
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
// SPIFFS ma limit nazwy obiektu: 32 bajty Z terminatorem, czyli 31 znakow.
// Nazwa obiektu to "/ha_" + klucz + ".bin" (prefiks /spiffs jest obcinany),
// wiec na sam klucz zostaja 23 znaki. Dluzsze klucze (np.
// "56989134:energia_bierna_c_kvarh") powodowaly ciche bledy zapisu -
// historia takiego pola nigdy nie trafiala na flash.
#define SAFE_KEY_MAX 23

static void safe_key(const char *id, char *out) {
    char tmp[48];
    int j = 0;
    for (int i = 0; id[i] && j < (int)sizeof(tmp) - 1; i++)
        tmp[j++] = (id[i] == ':') ? '_' : id[i];
    tmp[j] = 0;
    if (j <= SAFE_KEY_MAX) { memcpy(out, tmp, j + 1); return; }
    // Za dlugie: skroc i dopnij 4-znakowy skrot PELNEGO klucza (FNV-1a),
    // zeby np. ..._bierna_c_kvarh i ..._bierna_l_kvarh nie zlaly sie w jedno.
    uint32_t h = 2166136261u;
    for (int i = 0; id[i]; i++) { h ^= (uint8_t)id[i]; h *= 16777619u; }
    int keep = SAFE_KEY_MAX - 5;          // miejsce na "_" + 4 hex
    memcpy(out, tmp, keep);
    snprintf(out + keep, 6, "_%04x", (unsigned)(h & 0xFFFF));
}

static void meter_path(const char *id, char *out, int cap) {
    char safe[SAFE_KEY_MAX + 1];
    safe_key(id, safe);
    snprintf(out, cap, "/spiffs/h_%s.bin", safe);
}

// Sciezka archiwum godzinowego (miesiac) - tylko na flash, nie w RAM.
static void archive_path(const char *id, char *out, int cap) {
    char safe[SAFE_KEY_MAX + 1];
    safe_key(id, safe);
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
// FAZA 3a: skladamy caly 20-bajtowy naglowek w RAM buforze i piszemy JEDNYM
// fwrite. Poprzednio bylo 5 osobnych fwrite - reset zasilania miedzy 1. a 2.
// zostawial magic zapisany ale count=smiec z poprzedniego pliku, a arc_read_header2
// widzial 'v2 z nieprawidlowa count' -> ponizej bounds-check ustawial count=0,
// glowe=0 i traktowal archiwum jako puste (17520 rekordow godzinowych efektywnie
// porzucone). Pojedynczy fwrite = SPIFFS albo zapisuje calosc, albo nie zapisuje.
static void arc_write_header_v2(FILE *f, const arc_hdr_t *h) {
    uint8_t buf[ARC_HDR_V2];   // ARC_HDR_V2 = 20 (uint32 + int*2 + uint32 + float)
    _Static_assert(ARC_HDR_V2 == sizeof(uint32_t) + sizeof(int)*2 + sizeof(uint32_t) + sizeof(float),
                   "arc_hdr_t rozmiar zmieniony - aktualizuj arc_write_header_v2");
    uint32_t magic = ARC_MAGIC_V2;
    size_t off = 0;
    memcpy(buf + off, &magic,        sizeof(uint32_t)); off += sizeof(uint32_t);
    memcpy(buf + off, &h->count,     sizeof(int));      off += sizeof(int);
    memcpy(buf + off, &h->head,      sizeof(int));      off += sizeof(int);
    memcpy(buf + off, &h->last_ts,   sizeof(uint32_t)); off += sizeof(uint32_t);
    memcpy(buf + off, &h->last_total,sizeof(float));    off += sizeof(float);
    fseek(f, 0, SEEK_SET);
    fwrite(buf, off, 1, f);
}

// Pozycja w pliku (bajty) rekordu fizycznego o indeksie phys (naglowek hdr B).
static long arc_rec_off2(int hdr, int phys) {
    return (long)hdr + (long)phys * (long)sizeof(hist_bucket_t);
}

// Jednorazowa migracja pliku v1 -> v2: kopiowanie strumieniowe do pliku .t,
// potem rename. Rekordy przenoszone 1:1 (fizyczny uklad ringu zachowany),
// wiec nie tracimy zebranego archiwum. Zwraca otwarty plik v2 (r+b) lub NULL.
static FILE *arc_migrate_v2(const char *path, arc_hdr_t *h) {
    // Stala krotka nazwa pliku tymczasowego: SPIFFS ogranicza nazwe obiektu do
    // 31 znakow (z wiodacym '/'), a najdluzsze archiwa (np. ha_..._napiecie_l1_v.bin
    // = 30 zn.) z sufiksem juz sie nie miescily - fopen padal i migracja byla
    // "nieudana" wlasnie dla pol napiec. Migracje sa serializowane mutexem
    // historii, wiec jedna wspolna nazwa jest bezpieczna.
    const char *tmp = "/spiffs/arc_mig.tmp";
    remove(tmp);   // sprzatnij pozostalosc po ew. przerwanej migracji
    remove("/spiffs/arc_mig.dst");
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
    // Marker celu PRZED usunieciem oryginalu: utrata zasilania w oknie
    // remove(path)..rename(tmp,path) nie zgubi archiwum - history_init
    // dokonczy przenoszenie przy nastepnym starcie.
    FILE *mk = fopen("/spiffs/arc_mig.dst", "wb");
    if (mk) { fwrite(path, 1, strlen(path), mk); fclose(mk); }
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); remove("/spiffs/arc_mig.dst"); return NULL; }
    remove("/spiffs/arc_mig.dst");
    ESP_LOGI(TAG, "Archiwum %s zmigrowane do formatu v2", path);
    return fopen(path, "r+b");
}

// Weryfikacja i naprawa archiwum z uszkodzonym naglowkiem (np. count=17520
// przy realnych ~200 rekordach po przerwanym zapisie na starszym firmware).
// Objaw: ring udaje pelny i kazda nowa godzina nadpisuje logicznie najstarszy
// slot - poczatek historii przesuwa sie do przodu o godzine na godzine.
// Naprawa: od NAJNOWSZEGO rekordu idziemy wstecz po logicznych indeksach,
// dopoki rekordy sa czytelne, z sensownym ts i scisle malejace; ten ciagly
// ogon to prawdziwe dane. Nowy naglowek: head=fizyczna pozycja pierwszego
// poprawnego, count=dlugosc ogona. Rekordy zostaja na miejscu.
#define ARC_TS_MIN 1700000000u
#define ARC_TS_MAX 2100000000u
static bool arc_validate_repair(const char *path, FILE *f, arc_hdr_t *h) {
    if (h->count <= 0) return true;
    hist_bucket_t rec;
    // Pierwszy i ostatni logiczny rekord.
    int p_first = h->head;
    int p_last  = (h->head + h->count - 1) % HIST_ARCHIVE_HOURS;
    bool first_ok = (fseek(f, arc_rec_off2(h->hdr, p_first), SEEK_SET) == 0 &&
                     fread(&rec, sizeof(rec), 1, f) == 1 &&
                     rec.ts >= ARC_TS_MIN && rec.ts <= ARC_TS_MAX);
    hist_bucket_t last;
    bool last_ok = (fseek(f, arc_rec_off2(h->hdr, p_last), SEEK_SET) == 0 &&
                    fread(&last, sizeof(last), 1, f) == 1 &&
                    last.ts >= ARC_TS_MIN && last.ts <= ARC_TS_MAX);
    if (first_ok && last_ok) return true;   // naglowek wyglada zdrowo
    if (!last_ok) return false;             // nawet koniec zly - nie do naprawy tutaj

    // Idz wstecz od konca: licz dlugosc poprawnego ogona.
    int valid = 1;
    uint32_t prev_ts = last.ts;
    for (int back = 1; back < h->count; back++) {
        int phys = (p_last - back + HIST_ARCHIVE_HOURS) % HIST_ARCHIVE_HOURS;
        if (fseek(f, arc_rec_off2(h->hdr, phys), SEEK_SET) != 0) break;
        if (fread(&rec, sizeof(rec), 1, f) != 1) break;
        if (rec.ts < ARC_TS_MIN || rec.ts > ARC_TS_MAX) break;
        if (rec.ts >= prev_ts) break;       // musi scisle malec idac wstecz
        prev_ts = rec.ts;
        valid++;
    }
    int new_head = (p_last - (valid - 1) + HIST_ARCHIVE_HOURS) % HIST_ARCHIVE_HOURS;
    ESP_LOGW(TAG, "Archiwum %s: naprawa naglowka (bylo count=%d head=%d, jest count=%d head=%d)",
             path, h->count, h->head, valid, new_head);
    h->count = valid;
    h->head  = new_head;
    if (h->v2) {
        arc_write_header_v2(f, h);
    } else {
        fseek(f, 0, SEEK_SET);
        fwrite(&h->count, sizeof(int), 1, f);
        fwrite(&h->head,  sizeof(int), 1, f);
    }
    return true;
}


// Dopisz/zaktualizuj punkt godzinowy w archiwum (ring buffer, strumieniowo).
// NIE wczytuje calego pliku do RAM - tylko naglowek + ostatni rekord.
// ts_exact = dokladny unix ostatniego odczytu (do naglowka v2).
static void archive_append_hour(const char *id, uint32_t hour_ts, float total,
                                uint32_t ts_exact) {
    if (!s_fs_ok) return;
    char path[64]; archive_path(id, path, sizeof(path));

    arc_hdr_t h;
    errno = 0;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        if (errno != ENOENT) {
            ESP_LOGE(TAG, "Archiwum %s: otwarcie nieudane (errno=%d) - pomijam zapis", path, errno);
            return;
        }
        // nowy plik - utworz z naglowkiem v2
        f = fopen(path, "w+b");
        if (!f) { ESP_LOGW(TAG, "nie moge utworzyc archiwum %s", path); return; }
        memset(&h, 0, sizeof(h));
        h.v2 = true; h.hdr = ARC_HDR_V2;
    } else {
        if (!arc_read_header2(f, &h)) {
            // Nieczytelny naglowek podczas pracy: NIE kasuj po cichu. Zachowaj
            // plik jako /spiffs/arc_bad.bin (dowod + szansa recznego odzyskania),
            // dopiero potem zaloz swiezy ring. Poprzednio plik byl zerowany
            // przy kazdym takim zdarzeniu - to jedna z drog utraty historii.
            fclose(f);
            remove("/spiffs/arc_bad.bin");
            if (rename(path, "/spiffs/arc_bad.bin") == 0)
                ESP_LOGE(TAG, "Archiwum %s nieczytelne - odlozone jako arc_bad.bin", path);
            f = fopen(path, "w+b");
            if (!f) { ESP_LOGW(TAG, "nie moge odtworzyc archiwum %s", path); return; }
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
            // FAZA 3a: wymus flush do flasha przed fclose. Bez tego SPIFFS trzyma
            // rekord w bufferze VFS - reset zasilania w oknie ~kilku sek moglby
            // stracic zapisane odczyty mimo tego ze fclose sam nie flushuje SPIFFS.
            flush_to_flash(f);
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
    flush_to_flash(f);
    fclose(f);
}

// ================= FAZA 9b: archiwum krzywej 5-min (hc_*.bin) =================
// Rownolegle do archiwum godzinowego ha_*.bin trzymamy ring 5-minutowy dla pol
// CHWILOWYCH (moc kW, napiecie). Dzieki temu wykres doby ma rozdzielczosc 5 min
// dla KAZDEGO dnia z okna retencji, a nie tylko dla dzis/wczoraj (dwa bufory
// w RAM). Format jest celowo prostszy niz ha_ (jedna wersja, brak last_ts):
//   naglowek: [u32 magic "SIHC"][int count][int head]
//   dane:     HIST_CURVE_ARC rekordow hist_bucket_t (ring, ts rosnie logicznie)
// Rekord logiczny i (0 = najstarszy) lezy fizycznie na (head+i)%HIST_CURVE_ARC.
// Zapis nastepuje tylko przy ZAMKNIECIU kubelka (raz na 5 min na pole).
#define CARC_MAGIC   0x53494843u                                   // "SIHC"
#define CARC_HDR     (int)(sizeof(uint32_t) + sizeof(int) * 2)     // 12 B
// Nie zakladaj NOWEGO archiwum krzywej, gdy na partycji zostalo mniej niz tyle.
// Kazdy plik urosnie docelowo do HIST_CURVE_ARC * 8 B (~207 KB przy 90 dniach),
// wiec ten prog powstrzymuje zapychanie flasha przy duzej liczbie pol chwilowych.
#define CARC_MIN_FREE (512 * 1024)

static void curve_archive_path(const char *id, char *out, int cap) {
    char safe[SAFE_KEY_MAX + 1];
    safe_key(id, safe);
    snprintf(out, cap, "/spiffs/hc_%s.bin", safe);
}

typedef struct { int count, head; } carc_hdr_t;

static long carc_rec_off(int phys) {
    return (long)CARC_HDR + (long)phys * (long)sizeof(hist_bucket_t);
}

static bool carc_read_header(FILE *f, carc_hdr_t *h) {
    uint32_t magic = 0;
    h->count = 0; h->head = 0;
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fread(&magic, sizeof(magic), 1, f) != 1) return false;
    if (magic != CARC_MAGIC) return false;
    if (fread(&h->count, sizeof(int), 1, f) != 1) return false;
    if (fread(&h->head,  sizeof(int), 1, f) != 1) return false;
    if (h->count < 0 || h->count > HIST_CURVE_ARC)  { h->count = 0; h->head = 0; return false; }
    if (h->head  < 0 || h->head  >= HIST_CURVE_ARC) { h->count = 0; h->head = 0; return false; }
    return true;
}

// Naglowek skladany w RAM i pisany JEDNYM fwrite - jak arc_write_header_v2.
// Reset zasilania w srodku kilku malych fwrite zostawialby magic + smieciowy
// count, czyli archiwum "poprawne, ale puste".
static void carc_write_header(FILE *f, const carc_hdr_t *h) {
    uint8_t buf[CARC_HDR];
    uint32_t magic = CARC_MAGIC;
    size_t off = 0;
    memcpy(buf + off, &magic,    sizeof(uint32_t)); off += sizeof(uint32_t);
    memcpy(buf + off, &h->count, sizeof(int));      off += sizeof(int);
    memcpy(buf + off, &h->head,  sizeof(int));      off += sizeof(int);
    fseek(f, 0, SEEK_SET);
    fwrite(buf, off, 1, f);
}

// Czy na partycji jest jeszcze zapas na zalozenie nowego archiwum krzywej.
static bool carc_space_for_new_file(const char *id) {
    size_t total = 0, used = 0;
    if (esp_spiffs_info("littlefs", &total, &used) != ESP_OK) return true;
    if (total > used && (total - used) >= CARC_MIN_FREE) return true;
    ESP_LOGW(TAG, "%s: brak zapasu flasha na archiwum 5-min (wolne %u B, prog %u B)"
                  " - starsze dni tego pola zostana godzinowe",
             id ? id : "?", (unsigned)(total > used ? total - used : 0),
             (unsigned)CARC_MIN_FREE);
    return false;
}

static void curve_archive_append(const char *id, uint32_t bucket_ts, float value) {
    if (!s_fs_ok || !id) return;
    if (bucket_ts < ARC_TS_MIN || bucket_ts > ARC_TS_MAX) return;   // czas niezsynchronizowany
    char path[64]; curve_archive_path(id, path, sizeof(path));

    carc_hdr_t h = { 0, 0 };
    errno = 0;
    FILE *f = fopen(path, "r+b");
    if (!f) {
        if (errno != ENOENT) return;
        if (!carc_space_for_new_file(id)) return;
        f = fopen(path, "w+b");
        if (!f) { ESP_LOGW(TAG, "nie moge utworzyc %s", path); return; }
        carc_write_header(f, &h);
    } else if (!carc_read_header(f, &h)) {
        // Nieczytelny naglowek: zaloz ring od nowa. W odroznieniu od ha_ nie
        // odkladamy kopii - to dane pochodne, archiwum godzinowe zostaje cale.
        fclose(f);
        remove(path);
        ESP_LOGW(TAG, "%s: archiwum 5-min nieczytelne - zakladam od nowa", id);
        f = fopen(path, "w+b");
        if (!f) return;
        h.count = 0; h.head = 0;
        carc_write_header(f, &h);
    }

    if (h.count > 0) {
        int last_phys = (h.head + h.count - 1) % HIST_CURVE_ARC;
        hist_bucket_t last;
        if (fseek(f, carc_rec_off(last_phys), SEEK_SET) == 0 &&
            fread(&last, sizeof(last), 1, f) == 1) {
            if (last.ts == bucket_ts) {
                // Ten sam kubelek (domkniecie po wczesniejszym zapisie) - nadpisz.
                last.total = value;
                fseek(f, carc_rec_off(last_phys), SEEK_SET);
                fwrite(&last, sizeof(last), 1, f);
                flush_to_flash(f);
                fclose(f);
                return;
            }
            if (last.ts > bucket_ts) {
                // Czas cofnal sie (korekta NTP) - nie psuj monotonicznosci ringu,
                // bo na niej opiera sie wyszukiwanie binarne przy odczycie doby.
                fclose(f);
                return;
            }
        }
    }

    int write_phys;
    if (h.count >= HIST_CURVE_ARC) {
        write_phys = h.head;                          // nadpisz najstarszy
        h.head = (h.head + 1) % HIST_CURVE_ARC;
    } else {
        write_phys = (h.head + h.count) % HIST_CURVE_ARC;
        h.count++;
    }
    hist_bucket_t rec = { bucket_ts, value };
    fseek(f, carc_rec_off(write_phys), SEEK_SET);
    fwrite(&rec, sizeof(rec), 1, f);
    carc_write_header(f, &h);
    flush_to_flash(f);
    fclose(f);
}

// Punkty 5-min dla doby [d0, d1) z archiwum na flashu. Zwraca liczbe punktow.
// Wyszukiwanie binarne po indeksie LOGICZNYM - nie wczytuje calego ringu do RAM.
static int curve_archive_read_day(const char *id, uint32_t d0, uint32_t d1,
                                  hist_bucket_t *out, int cap) {
    if (!s_fs_ok || !id || !out || cap <= 0) return 0;
    char path[64]; curve_archive_path(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    carc_hdr_t h;
    if (!carc_read_header(f, &h) || h.count <= 0) { fclose(f); return 0; }

    int lo = 0, hi = h.count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        hist_bucket_t r;
        int phys = (h.head + mid) % HIST_CURVE_ARC;
        if (fseek(f, carc_rec_off(phys), SEEK_SET) != 0 ||
            fread(&r, sizeof(r), 1, f) != 1) { hi = mid; break; }
        if (r.ts < d0) lo = mid + 1; else hi = mid;
    }
    int n = 0;
    for (int i = lo; i < h.count && n < cap; i++) {
        hist_bucket_t r;
        int phys = (h.head + i) % HIST_CURVE_ARC;
        if (fseek(f, carc_rec_off(phys), SEEK_SET) != 0 ||
            fread(&r, sizeof(r), 1, f) != 1) break;
        if (r.ts >= d1) break;
        if (r.ts >= d0) out[n++] = r;
    }
    fclose(f);
    return n;
}

// Zasianie archiwum przy PIERWSZYM starcie po aktualizacji: przepisuje krzywe
// dzis + wczoraj (juz obecne w h_*.bin v2) do nowego hc_*.bin, zeby te dwa dni
// nie stracily rozdzielczosci 5 min w chwili, gdy przestana byc dzis/wczoraj.
// Robi to JEDNYM otwarciem pliku - petla po curve_archive_append oznaczalaby
// do 576 cykli fopen/fsync/fclose i sekundy zwloki przy starcie.
static void curve_archive_seed(meter_hist_t *m) {
    if (!s_fs_ok || !m || m->cumulative) return;
    if (m->n_curve <= 0 && m->n_curve_yesterday <= 0) return;
    char path[64]; curve_archive_path(m->id, path, sizeof(path));
    FILE *chk = fopen(path, "rb");
    if (chk) { fclose(chk); return; }          // archiwum juz istnieje - nie ruszamy
    if (!carc_space_for_new_file(m->id)) return;
    FILE *f = fopen(path, "w+b");
    if (!f) return;
    carc_hdr_t h = { 0, 0 };
    carc_write_header(f, &h);
    uint32_t prev = 0;
    for (int src = 0; src < 2; src++) {        // najpierw wczoraj, potem dzis
        hist_bucket_t *a  = src ? m->curve   : m->curve_yesterday;
        int            na = src ? m->n_curve : m->n_curve_yesterday;
        if (!a || na <= 0) continue;
        for (int i = 0; i < na; i++) {
            if (a[i].ts < ARC_TS_MIN || a[i].ts > ARC_TS_MAX) continue;
            if (a[i].ts <= prev) continue;     // ring musi rosnac scisle
            fseek(f, carc_rec_off(h.count), SEEK_SET);
            fwrite(&a[i], sizeof(hist_bucket_t), 1, f);
            h.count++;
            prev = a[i].ts;
        }
    }
    carc_write_header(f, &h);
    flush_to_flash(f);
    fclose(f);
    ESP_LOGI(TAG, "%s: zasiano archiwum 5-min z RAM (%d pkt)", m->id, h.count);
}

// Diagnostyka przy starcie - zasieg archiwum krzywej (analogicznie do ha_).
static void curve_archive_log_range(const char *id) {
    char path[64]; curve_archive_path(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    carc_hdr_t h;
    if (carc_read_header(f, &h) && h.count > 0) {
        hist_bucket_t r0, r1;
        int p0 = h.head, p1 = (h.head + h.count - 1) % HIST_CURVE_ARC;
        if (fseek(f, carc_rec_off(p0), SEEK_SET) == 0 && fread(&r0, sizeof(r0), 1, f) == 1 &&
            fseek(f, carc_rec_off(p1), SEEK_SET) == 0 && fread(&r1, sizeof(r1), 1, f) == 1) {
            ESP_LOGI(TAG, "Archiwum 5-min %s: %d pkt (max %d), od ts=%u do ts=%u",
                     id, h.count, HIST_CURVE_ARC, (unsigned)r0.ts, (unsigned)r1.ts);
        }
    }
    fclose(f);
}
// ==================== koniec: archiwum krzywej 5-min ====================

// Odbuduj archiwum godzinowe (ha_) z bufora hours[] w RAM (7 dni godzin wczytanych
// z h_). Wolane przy starcie gdy archiwum jest puste/stare/niezgodne - dzieki temu
// tryb Dzis/Wczoraj pokazuje dane natychmiast po restarcie, bez czekania na ramke.
// Zapisuje tylko gdy archiwum nie ma juz wiecej danych niz hours[] (nie nadpisuje
// pelnego 2-letniego archiwum swiezo zebranego).
static void archive_rebuild_from_hours(meter_hist_t *m) {
    if (!s_fs_ok || m->n_hours == 0) return;
    char path[64]; archive_path(m->id, path, sizeof(path));

    // Poprawne archiwum NIGDY nie jest nadpisywane - co najwyzej UZUPELNIANE
    // o godziny z RAM nowsze niz jego ostatni rekord (archiwum moglo powstac
    // pozniej niz zbieranie danych). Wczesniejsza wersja przy kazdym klopocie
    // z odczytem naglowka tworzyla plik od zera, kasujac wielomiesieczna
    // historie do 7 dni z RAM.
    errno = 0;
    FILE *f = fopen(path, "r+b");
    if (f) {
        arc_hdr_t h;
        if (arc_read_header2(f, &h) && arc_validate_repair(path, f, &h)) {
            uint32_t last = 0;
            if (h.count > 0) {
                hist_bucket_t rec;
                int phys = (h.head + h.count - 1) % HIST_ARCHIVE_HOURS;
                if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) == 0 &&
                    fread(&rec, sizeof(rec), 1, f) == 1) last = rec.ts;
            }
            int added = 0;
            for (int i = 0; i < m->n_hours; i++) {
                if (m->hours[i].ts <= last) continue;
                int wp;
                if (h.count >= HIST_ARCHIVE_HOURS) {
                    wp = h.head; h.head = (h.head + 1) % HIST_ARCHIVE_HOURS;
                } else {
                    wp = (h.head + h.count) % HIST_ARCHIVE_HOURS; h.count++;
                }
                fseek(f, arc_rec_off2(h.hdr, wp), SEEK_SET);
                fwrite(&m->hours[i], sizeof(hist_bucket_t), 1, f);
                added++;
            }
            if (added) {
                if (h.v2) {
                    if (h.last_ts < m->last_ts) { h.last_ts = m->last_ts; h.last_total = m->last_total; }
                    arc_write_header_v2(f, &h);
                } else {
                    fseek(f, 0, SEEK_SET);
                    fwrite(&h.count, sizeof(int), 1, f);
                    fwrite(&h.head, sizeof(int), 1, f);
                }
                ESP_LOGI(TAG, "Archiwum %s uzupelnione o %d godz z RAM", path, added);
            }
            fclose(f);
            return;
        }
        fclose(f);
        remove("/spiffs/arc_bad.bin");
        if (rename(path, "/spiffs/arc_bad.bin") == 0)
            ESP_LOGE(TAG, "Archiwum %s nieczytelne - odlozone jako arc_bad.bin", path);
        ESP_LOGW(TAG, "Archiwum %s ma nieczytelny naglowek - odtwarzam z RAM (7 dni)", path);
    }

    // Utworz od nowa TYLKO gdy plik naprawde nie istnieje (ENOENT). Kazdy inny
    // powod nieudanego otwarcia (uszkodzenie po przerwanym zapisie, brak
    // zasobow) NIE moze konczyc sie nadpisaniem - zostaje glosny blad w logu.
    if (!f && errno != ENOENT) {
        ESP_LOGE(TAG, "Archiwum %s: otwarcie nieudane (errno=%d) - NIE odtwarzam", path, errno);
        return;
    }
    // Brak pliku lub naglowek nie do odczytania: utworz od nowa z godzin w RAM.
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
    char path[64]; archive_path(m->id, path, sizeof(path));
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
        } else {
            ESP_LOGW(TAG, "migracja archiwum %s przy starcie nieudana", path);
        }
    }
    ESP_LOGI(TAG, "Odtworzono %s: last_ts=%u total=%.3f (arch %s)",
             m->id, (unsigned)m->last_ts, (double)m->last_total,
             hdr_ok ? (h.v2 ? "v2" : "v1->v2") : "brak/nowy");
}

// --- FAZA 3b: wersjonowanie h_*.bin (magic + version + CRC32) ---
// Wczesniej h_ nie mial magic ani sumy kontrolnej - bit-flip na SPIFFS (nawet
// po latach) dawal odczyt smieci jako n_hours/n_days, ratowal tylko bounds-check
// ktory zerowal historie do zera. Teraz kazdy plik zaczyna sie od 12-bajtowego
// naglowka:
//   [0..3]  uint32 magic  = "SIH1" (0x53494831 LE)
//   [4..7]  uint32 version = 1
//   [8..11] uint32 payload_crc32 (IEEE 802.3, wyliczone po zapisaniu payload)
//   [12..]  payload = ten sam format co stary v0 (kind, last_total, ...)
// Backward compat: load_meter sniffuje pierwszy uint32; jesli != MAGIC to plik
// v0, te 4 bajty to `kind` (male 0-3, nie moze przypadkiem trafic w 1397909041)
// -> wczytujemy po staremu bez CRC. Migracja v0->v1 przy nastepnym save_meter
// (transparentna).
#define METER_MAGIC 0x53494831u   // "SIH1" (little-endian, historyczna nazwa - kolejne wersje maja rosnacy METER_VER)
#define METER_VER   2u            // v2 = FAZA 5a: dopisany curve_yesterday (day_ts + n + tablica)
#define METER_VER_V1 1u           // v1 = FAZA 3b: pierwsza wersja z magic + CRC (bez curve_yesterday)
#define METER_HDR   12            // magic(4) + ver(4) + crc(4)

// CRC32 IEEE 802.3 (jak w gzip/PNG) - bez lookup table, bez zaleznosci od
// esp_rom_crc.h/esp_crc.h. Dla ~14 KB pliku licznika liczy sie w mikrosekundach.
static uint32_t hist_crc32_update(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

static void save_meter(meter_hist_t *m) {
    if (!s_fs_ok) return;
    char path[64]; meter_path(m->id, path, sizeof(path));
    // Atomic write: zapis do .tmp, potem fsync + rename. Reset zasilania w
    // srodku fwrite() zostawia .tmp jako smiec (zignorowany przy starcie),
    // ale oryginalny .bin jest nadal caly - historia sie nie traci.
    char tmp[64]; path_to_tmp(path, tmp, sizeof(tmp));
    FILE *f = fopen(tmp, "wb");
    if (!f) { ESP_LOGW(TAG, "nie moge zapisac %s", tmp); return; }
    // Naglowek v1: magic + version + CRC placeholder (0). Prawdziwe CRC
    // wpisujemy po zakonczeniu zapisu (fseek do 8-go bajta).
    uint32_t magic = METER_MAGIC, ver = METER_VER, crc_placeholder = 0;
    fwrite(&magic, sizeof(uint32_t), 1, f);
    fwrite(&ver, sizeof(uint32_t), 1, f);
    fwrite(&crc_placeholder, sizeof(uint32_t), 1, f);
    // Payload + on-the-fly CRC. Makro W: fwrite + hist_crc32_update jednym
    // ruchem, zeby zaden zapis nie zostal pominiety w sumie kontrolnej.
    uint32_t crc = 0;
    #define W(ptr, sz) do { \
        fwrite((ptr), (sz), 1, f); \
        crc = hist_crc32_update(crc, (ptr), (sz)); \
    } while (0)
    #define WARR(ptr, elsz, n) do { \
        if ((n) > 0) { \
            fwrite((ptr), (elsz), (n), f); \
            crc = hist_crc32_update(crc, (ptr), (size_t)(elsz) * (size_t)(n)); \
        } \
    } while (0)
    W(&m->kind, sizeof(int));
    W(&m->last_total, sizeof(float));
    W(&m->last_ts, sizeof(uint32_t));
    W(&m->n_hours, sizeof(int));   WARR(m->hours,  sizeof(hist_bucket_t), m->n_hours);
    W(&m->n_days, sizeof(int));    WARR(m->days,   sizeof(hist_bucket_t), m->n_days);
    W(&m->n_months, sizeof(int));  WARR(m->months, sizeof(hist_bucket_t), m->n_months);
    W(&m->n_years, sizeof(int));   WARR(m->years,  sizeof(hist_bucket_t), m->n_years);
    W(&m->cumulative, sizeof(int));
    W(&m->n_curve, sizeof(int));
    if (m->curve && m->n_curve > 0)
        WARR(m->curve, sizeof(hist_bucket_t), m->n_curve);
    // Baza doby NA KONCU pliku (stare h_*.bin nadal sie wczytuja). Musi przezyc
    // restart: bez niej modul uruchomiony ponownie w srodku doby nie ma od czego
    // liczyc zuzycia i wykres dnia byl pusty.
    W(&m->day_base_ts, sizeof(uint32_t));
    W(&m->day_base_total, sizeof(float));
    W(&m->rebase_ts, sizeof(uint32_t));
    // FAZA 5a (v2): curve_yesterday - kopia krzywej z dnia poprzedniego dla
    // symetrycznego wykresu wczoraj (te same 5-min bucket'y co dzis). Pola
    // TYLKO v2 - stare pliki v1 nie mialy, load_meter musi obsluzyc oba warianty.
    // Zapisujemy nawet gdy pusta - pusta = zero curve_yesterday_day_ts + zero n.
    uint32_t cy_day = m->curve_yesterday_day_ts;
    int      cy_n   = m->n_curve_yesterday;
    if (cy_n < 0 || cy_n > HIST_CURVE) cy_n = 0;
    W(&cy_day, sizeof(uint32_t));
    W(&cy_n,   sizeof(int));
    if (cy_n > 0 && m->curve_yesterday)
        WARR(m->curve_yesterday, sizeof(hist_bucket_t), cy_n);
    #undef W
    #undef WARR
    // Wpisz obliczone CRC w miejscu placeholdera (offset 8 = po magic+ver).
    fseek(f, 8, SEEK_SET);
    fwrite(&crc, sizeof(uint32_t), 1, f);
    flush_to_flash(f);   // wymuszona synchronizacja z flashem przed rename
    fclose(f);
    // SPIFFS ESP-IDF NIE potrafi zrename nad istniejacy plik (POSIX pozwala,
    // SPIFFS zwraca EIO=5). Musimy usunac dst przed rename. Mikrookno miedzy
    // remove i rename (reset zasilania -> tylko .tmp na dysku, brak .bin)
    // jest naprawiane przez hist_recover_stale_tmp() przy starcie modulu.
    remove(path);
    if (rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "atomic rename %s -> %s nieudany (errno=%d)", tmp, path, errno);
        unlink(tmp);   // sprzatnij smiec, historia zostaje w starym .bin (juz usuniety!)
        return;
    }
    m->last_save_ts = m->last_ts;
}

static bool load_meter(meter_hist_t *m, const char *id) {
    if (!s_fs_ok) return false;
    char path[64]; meter_path(id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    free_curve(m);            // wskaznik zaraz zniknie w memset - nie wyciekaj
    memset(m, 0, sizeof(*m));
    strncpy(m->id, id, sizeof(m->id) - 1);
    m->used = true;

    // FAZA 3b: sniff naglowka. Pierwszy uint32 == METER_MAGIC -> plik v1 z CRC.
    // Kolizja z v0 (gdzie 1. uint32 to `kind` typu int, wartosc 0-3) niemozliwa
    // bo METER_MAGIC = 1397909041 (0x53494831). Po detekcji v1 wczytujemy ver+crc,
    // reszte payload z on-the-fly CRC. Na koncu porownujemy - mismatch = warn
    // (dane wczytujemy mimo to, bo bounds-check na kazdym n_xxx jak wczesniej;
    // pojedynczy bit-flip w zerowanym polu i tak by nie zmienil widocznego stanu).
    uint32_t sniff = 0;
    if (fread(&sniff, sizeof(uint32_t), 1, f) != 1) { fclose(f); return false; }
    bool v1 = (sniff == METER_MAGIC);
    uint32_t expected_crc = 0;
    uint32_t crc = 0;
    if (v1) {
        uint32_t ver = 0;
        if (fread(&ver, sizeof(uint32_t), 1, f) != 1 ||
            fread(&expected_crc, sizeof(uint32_t), 1, f) != 1) {
            ESP_LOGW(TAG, "load_meter %s: uciety naglowek v1", id);
            fclose(f); return false;
        }
        if (ver != METER_VER && ver != METER_VER_V1) {
            ESP_LOGW(TAG, "load_meter %s: nieznana wersja v%u (obsluguje v%u/v%u)",
                     id, (unsigned)ver, METER_VER_V1, METER_VER);
            // Nie porzucamy - probujemy wczytac po staremu (moze byc nadzbior).
        }
        // v1 = FAZA 3b (bez curve_yesterday), v2 = FAZA 5a (z curve_yesterday).
        // Migrator v1->v2 jest transparentny: pola nowe nie istnieja w pliku v1,
        // fread zwroci 0, curve_yesterday zostanie puste, nastepny save_meter
        // zapisze plik jako v2 (z pustym curve_yesterday na start).
        // Payload wczytujemy dalej z on-the-fly CRC (poczatek payloadu = kind).
    } else {
        // Plik v0 - pierwszy uint32 to `kind`. Przypisz i kontynuuj po staremu.
        m->kind = (int)sniff;
    }

    // Makro R: fread + hist_crc32_update (tylko dla v1, dla v0 CRC nie liczymy).
    #define R(ptr, sz) do { \
        if (fread((ptr), (sz), 1, f) == 1 && v1) \
            crc = hist_crc32_update(crc, (ptr), (sz)); \
    } while (0)
    #define RARR_INTO(field, cap, field_max) do { \
        R(&(field), sizeof(int)); \
        if ((field) < 0 || (field) > (field_max)) (field) = 0; \
        size_t got = fread((cap), sizeof(hist_bucket_t), (size_t)(field), f); \
        if (v1) crc = hist_crc32_update(crc, (cap), got * sizeof(hist_bucket_t)); \
        (field) = (int)got; \
    } while (0)

    if (v1) {
        // Payload kind czytamy dopiero teraz (dla v0 juz jest w m->kind z sniff).
        R(&m->kind, sizeof(int));
    }
    R(&m->last_total, sizeof(float));
    R(&m->last_ts, sizeof(uint32_t));
    RARR_INTO(m->n_hours,  m->hours,  HIST_HOURS);
    RARR_INTO(m->n_days,   m->days,   HIST_DAYS);
    RARR_INTO(m->n_months, m->months, HIST_MONTHS);
    RARR_INTO(m->n_years,  m->years,  HIST_YEARS);

    // cumulative na koncu (nowe pliki v0 mialy go, starsze nie - fallback z id).
    int cum = 0;
    size_t got_cum = fread(&cum, sizeof(int), 1, f);
    if (got_cum == 1) {
        if (v1) crc = hist_crc32_update(crc, &cum, sizeof(int));
        m->cumulative = cum;
    } else {
        bool instant = (strstr(id, "napiecie") || strstr(id, "moc"));
        m->cumulative = instant ? 0 : 1;
    }

    // Krzywa dnia - dopisana na koncu w nowszych wersjach; brak = 0 punktow.
    int nc = 0;
    size_t got_nc = fread(&nc, sizeof(int), 1, f);
    if (got_nc == 1) {
        if (v1) crc = hist_crc32_update(crc, &nc, sizeof(int));
        if (nc < 0 || nc > HIST_CURVE) nc = 0;
        m->n_curve = nc;
        if (nc > 0 && ensure_curve(m)) {
            size_t got_c = fread(m->curve, sizeof(hist_bucket_t), nc, f);
            if (v1) crc = hist_crc32_update(crc, m->curve, got_c * sizeof(hist_bucket_t));
            m->n_curve = (int)got_c;
        } else {
            m->n_curve = 0;
        }
    } else {
        m->n_curve = 0;
    }

    // Baza doby - dopisana na koncu w nowszych wersjach; brak w starym pliku = 0.
    uint32_t dbt = 0; float dbtot = 0.0f; uint32_t rbt = 0;
    if (fread(&dbt, sizeof(uint32_t), 1, f) == 1 &&
        fread(&dbtot, sizeof(float), 1, f) == 1) {
        if (v1) {
            crc = hist_crc32_update(crc, &dbt, sizeof(uint32_t));
            crc = hist_crc32_update(crc, &dbtot, sizeof(float));
        }
        m->day_base_ts = dbt;
        m->day_base_total = dbtot;
    } else {
        m->day_base_ts = 0;
        m->day_base_total = 0;
    }
    if (fread(&rbt, sizeof(uint32_t), 1, f) == 1) {
        if (v1) crc = hist_crc32_update(crc, &rbt, sizeof(uint32_t));
        m->rebase_ts = rbt;
    } else {
        m->rebase_ts = 0;
    }

    // FAZA 5a (format v2): curve_yesterday - opcjonalny, obecny tylko w plikach
    // v2. Dla v1 fread zwroci 0 -> curve_yesterday pusty (do rotacji przy pierwszym
    // przejsciu doby). Kolejnosc pol: day_ts, n, tablica curve_yesterday.
    uint32_t cy_day = 0;
    int      cy_n = 0;
    if (fread(&cy_day, sizeof(uint32_t), 1, f) == 1) {
        if (v1) crc = hist_crc32_update(crc, &cy_day, sizeof(uint32_t));
        if (fread(&cy_n, sizeof(int), 1, f) == 1) {
            if (v1) crc = hist_crc32_update(crc, &cy_n, sizeof(int));
            if (cy_n < 0 || cy_n > HIST_CURVE) cy_n = 0;
            if (cy_n > 0) {
                m->curve_yesterday = calloc(HIST_CURVE, sizeof(hist_bucket_t));
                if (m->curve_yesterday) {
                    size_t got = fread(m->curve_yesterday, sizeof(hist_bucket_t), cy_n, f);
                    if (v1) crc = hist_crc32_update(crc, m->curve_yesterday,
                                                     got * sizeof(hist_bucket_t));
                    m->n_curve_yesterday = (int)got;
                    m->curve_yesterday_day_ts = cy_day;
                } else {
                    ESP_LOGW(TAG, "load_meter %s: brak RAM na curve_yesterday - pominiete", id);
                    // Skip rekordy w pliku zeby CRC dalej sie zgadzalo.
                    hist_bucket_t tmp;
                    for (int k = 0; k < cy_n; k++) {
                        if (fread(&tmp, sizeof(tmp), 1, f) != 1) break;
                        if (v1) crc = hist_crc32_update(crc, &tmp, sizeof(tmp));
                    }
                }
            }
        }
    }
    #undef R
    #undef RARR_INTO
    fclose(f);

    // Weryfikacja CRC dla v1. Konserwatywnie: warn ale zaakceptuj (dane juz
    // ograniczone przez bounds-check na kazdym n_xxx, a pojedynczy bit-flip w
    // pustej sekcji nie zmieni widocznego stanu). Kolejny save_meter przelicze
    // CRC i sytuacja sie zsynchronizuje.
    if (v1 && crc != expected_crc) {
        ESP_LOGW(TAG, "load_meter %s: CRC mismatch (0x%08x vs 0x%08x) - "
                      "prawdopodobny bit-flip na SPIFFS lub uciety zapis",
                 id, (unsigned)crc, (unsigned)expected_crc);
    } else if (v1) {
        ESP_LOGI(TAG, "load_meter %s: OK (v1, CRC 0x%08x)", id, (unsigned)crc);
    } else {
        ESP_LOGI(TAG, "load_meter %s: OK (v0, brak CRC)", id);
    }
    return true;
}

// znajdz lub zaalokuj slot licznika
static meter_hist_t *find_meter(const char *id, bool create) {
    for (int i = 0; i < MAX_HIST_METERS; i++)
        if (s_meters[i] && s_meters[i]->used && strcmp(s_meters[i]->id, id) == 0) return s_meters[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_HIST_METERS; i++)
        if (!s_meters[i] || !s_meters[i]->used) {
            meter_hist_t *sl = slot_get(i);
            if (!sl) continue;                 // brak RAM - sprobuj kolejny slot
            free_curve(sl);                    // wskaznik zniknie w memset
            memset(sl, 0, sizeof(meter_hist_t));
            strncpy(sl->id, id, sizeof(sl->id) - 1);
            sl->used = true;
            return sl;
        }
    return NULL;
}

// ---------- Inicjalizacja ----------
// ---------- Sledzenie licznikow ----------
#define TRACKED_PATH "/spiffs/tracked.txt"
#define TRACKED_TMP  "/spiffs/tracked.tmp"

static void tracked_save(void) {
    if (!s_fs_ok) return;
    // Atomic write: reset zasilania podczas przepisywania listy tracked
    // (ustawianie/usuwanie sledzenia licznika) mogl zostawic uciety plik ->
    // po restarcie modul tracil sledzenie czesci licznikow. Tmp + rename
    // gwarantuje ze zawsze widzimy albo stara pelna liste, albo nowa pelna.
    FILE *f = fopen(TRACKED_TMP, "wb");
    if (!f) return;
    for (int i = 0; i < s_tracked_count; i++)
        fprintf(f, "%s\n", s_tracked[i]);
    flush_to_flash(f);
    fclose(f);
    // SPIFFS wymaga usuniecia dst przed rename (patrz komentarz w save_meter).
    remove(TRACKED_PATH);
    if (rename(TRACKED_TMP, TRACKED_PATH) != 0) {
        ESP_LOGW(TAG, "tracked atomic rename nieudany (errno=%d)", errno);
        unlink(TRACKED_TMP);
    }
}

static void tracked_load(void) {
    s_tracked_count = 0;
    if (!s_fs_ok) return;
    FILE *f = fopen(TRACKED_PATH, "rb");
    if (!f) return;
    char line[48];
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

void history_erase_all(void) {
    // Reset fabryczny: kasuje CALA historie z flasha i liste sledzonych.
    // Konfiguracja siedzi w NVS (nvs_config_reset), ale historia to osobny
    // system plikow - bez tego po resecie zostawaly wykresy i zaznaczone pola.
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    // 1. zwolnij sloty w RAM, zeby nic sie nie zapisalo z powrotem
    for (int i = 0; i < MAX_HIST_METERS; i++) {
        if (s_meters[i]) {
            free_curve(s_meters[i]);
            free(s_meters[i]);
            s_meters[i] = NULL;
        }
    }
    s_tracked_count = 0;
    memset(s_tracked, 0, sizeof(s_tracked));

    // 2. skasuj pliki historii i liste sledzonych
    int removed = 0;
    if (s_fs_ok) {
        // FAZA 9b: doszedl trzeci plik na pole (hc_ obok h_ i ha_), wiec przy
        // 24 sledzonych polach lista ofiar nie miesci sie w jednym przebiegu.
        // Bufor zostaje maly (1.6 KB stosu) - zamiast tego skanujemy katalog
        // kilka razy, az przestana pojawiac sie pasujace pliki.
        for (int pass = 0; pass < 6; pass++) {
            DIR *dir = opendir("/spiffs");
            if (!dir) break;
            struct dirent *de;
            char victims[40][40];
            int nv = 0;
            while ((de = readdir(dir)) != NULL && nv < 40) {
                const char *nm = de->d_name;
                // "hc_" (archiwum krzywej 5-min) NIE lapie sie na "h_" -
                // drugi znak rozni sie od '_'; trzeba je wymienic osobno.
                if (strncmp(nm, "h_", 2) == 0 || strncmp(nm, "ha_", 3) == 0 ||
                    strncmp(nm, "hc_", 3) == 0 ||
                    strcmp(nm, "tracked.txt") == 0 || strncmp(nm, "arc_", 4) == 0) {
                    snprintf(victims[nv], sizeof(victims[0]), "%.39s", nm);
                    nv++;
                }
            }
            closedir(dir);
            if (nv == 0) break;
            // kasujemy PO zamknieciu katalogu - usuwanie w trakcie readdir
            // potrafi pominac wpisy
            for (int i = 0; i < nv; i++) {
                char path[64];
                snprintf(path, sizeof(path), "/spiffs/%.50s", victims[i]);
                if (remove(path) == 0) removed++;
            }
        }
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "Reset fabryczny: skasowano %d plikow historii", removed);
}

size_t history_free_curves(void) {
    // Krzywe minutowe (po 11.2 KB) sa odtwarzalne - po restarcie i tak buduja sie
    // od nowa, a dane godzinowe zostaja. Zwalniamy je przed OTA, bo TLS do GitHuba
    // potrzebuje sporego, spojnego kawalka sterty.
    size_t freed = 0;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_HIST_METERS; i++) {
        if (!s_meters[i]) continue;
        if (s_meters[i]->curve)           freed += HIST_CURVE * sizeof(hist_bucket_t);
        if (s_meters[i]->curve_yesterday) freed += HIST_CURVE * sizeof(hist_bucket_t);
        if (s_meters[i]->curve || s_meters[i]->curve_yesterday)
            free_curve(s_meters[i]);   // zwalnia oba
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Zwolniono %u B buforow krzywych + curve_yesterday (OTA)", (unsigned)freed);
    return freed;
}

// Suma zuzycia za dobe liczona DOKLADNIE tak, jak rysuje ja panel: przez
// wywolanie tej samej funkcji, ktora buduje wykres, i zsumowanie slupkow.
// Dzieki temu e-ink i UI nie moga sie rozjechac - nie ma dwoch algorytmow.
bool history_day_sum(const char *key, uint32_t day_ts, float *out_sum) {
    if (!key || !out_sum) return false;
    char *buf = malloc(DAY_SUM_BUF);
    if (!buf) return false;
    int n = history_get_day_json(key, day_ts, buf, DAY_SUM_BUF);
    if (n <= 0) { free(buf); return false; }
    float sum = 0;
    bool any = false;
    const char *p = buf;
    while ((p = strstr(p, "\"v\":")) != NULL) {
        sum += (float)atof(p + 4);
        any = true;
        p += 4;
    }
    free(buf);
    if (!any) return false;
    *out_sum = sum;
    return true;
}

void history_mark_rebase(void) {
    FILE *f = fopen(REBASE_FLAG, "wb");
    if (f) { fputc(1, f); fclose(f); }
}

bool history_fs_ok(void) { return s_fs_ok; }

void history_tracked_limits(int *used, int *max) {
    if (used) *used = s_tracked_count;
    if (max)  *max  = MAX_TRACKED;
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
        // beta346: dotad ta funkcja byla calkowicie niema, wiec z logow nie dalo sie
        // stwierdzic, czy sledzenie w ogole zostalo wlaczone.
        ESP_LOGI(TAG, "Sledzenie WLACZONE: %s (%d/%d)", id_hex, s_tracked_count, MAX_TRACKED);
    } else if (!tracked && exists) {
        // usun przesuwajac reszte
        for (int i = idx; i < s_tracked_count - 1; i++)
            memcpy(s_tracked[i], s_tracked[i+1], sizeof(s_tracked[0]));
        s_tracked_count--;
        tracked_save();
        ESP_LOGI(TAG, "Sledzenie WYLACZONE: %s (%d/%d)", id_hex, s_tracked_count, MAX_TRACKED);
        // Zwolnij slot RAM (2.9KB + ew. krzywa 11.5KB) - inaczej wisialby do
        // restartu. Pliki na flashu zostaja, wiec ponowne wlaczenie sledzenia
        // odzyskuje historie.
        for (int i = 0; i < MAX_HIST_METERS; i++) {
            if (s_meters[i] && s_meters[i]->used &&
                strcasecmp(s_meters[i]->id, id_hex) == 0) {
                save_meter(s_meters[i]);
                free_curve(s_meters[i]);
                free(s_meters[i]);
                s_meters[i] = NULL;
                break;
            }
        }
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
}

static meter_hist_t *get_or_load(const char *id, bool create_if_missing);

// FAZA 3a hotfix: sirotki .tmp po reset zasilania w mikrooknie miedzy remove(dst)
// a rename(tmp, dst) w save_meter/tracked_save. Iteruje /spiffs i dla kazdego
// pliku *.tmp: jesli docelowy .bin (lub tracked.txt) istnieje -> tmp to smiec,
// unlink. Jesli docelowy nie istnieje -> odzyskaj przez rename (dane w .tmp sa
// swiezsze niz brak pliku). Sensowne tylko po SPIFFS mount, przed load_meter.
static void hist_recover_stale_tmp(void) {
    if (!s_fs_ok) return;
    DIR *d = opendir("/spiffs");
    if (!d) return;
    int recovered = 0, cleaned = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t nlen = strlen(name);
        if (nlen < 5 || strcmp(name + nlen - 4, ".tmp") != 0) continue;

        // struct dirent::d_name w newlib ma 256 B. SPIFFS w praktyce ogranicza
        // nazwe do ~31 znakow, ale GCC z -Werror=format-truncation nie zna tego
        // runtime-limitu, wiec bufor musi teoretycznie zmiescic caly d_name.
        // 264 = 256 (d_name) + 8 ("/spiffs/").
        char src_path[264]; snprintf(src_path, sizeof(src_path), "/spiffs/%s", name);
        char dst_path[264];
        // tracked.tmp -> tracked.txt (specjalny case), inaczej *.tmp -> *.bin.
        if (strcmp(name, "tracked.tmp") == 0) {
            snprintf(dst_path, sizeof(dst_path), "/spiffs/tracked.txt");
        } else {
            snprintf(dst_path, sizeof(dst_path), "/spiffs/%s", name);
            size_t dlen = strlen(dst_path);
            memcpy(dst_path + dlen - 4, ".bin", 4);
        }

        struct stat st;
        if (stat(dst_path, &st) == 0) {
            // Docelowy istnieje - tmp to smiec po nieudanym rename (dst byl OK).
            unlink(src_path);
            cleaned++;
        } else {
            // Brak docelowego - tmp to najswiezsze co mamy, odzyskaj.
            if (rename(src_path, dst_path) == 0) {
                recovered++;
                ESP_LOGI(TAG, "Odzyskano sierotke: %s -> %s", src_path, dst_path);
            } else {
                ESP_LOGW(TAG, "Nie moge odzyskac %s (errno=%d) - usuwam", src_path, errno);
                unlink(src_path);
                cleaned++;
            }
        }
    }
    closedir(d);
    if (recovered + cleaned > 0) {
        ESP_LOGI(TAG, "Recovery .tmp: %d odzyskanych, %d usunietych", recovered, cleaned);
    }
}

void history_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "littlefs",   // partycja z subtype spiffs
        .max_files = 6,
        // NIGDY nie formatuj automatycznie: nieudane montowanie (np. brudny
        // stan po twardym resecie) kasowaloby CALA historie wszystkich
        // licznikow. Wolimy prace bez historii i glosny blad w logu.
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        // Rozroznij DWA przypadki nieudanego montowania:
        //  a) fabrycznie nowy modul - partycja nigdy nie byla sformatowana.
        //     Trzeba ja sformatowac RAZ, inaczej historia nigdy nie zadziala.
        //  b) modul, ktory kiedys mial dzialajacy system plikow - tu format
        //     skasowalby cala historie, wiec NIE formatujemy (jak dotad).
        // Rozroznienie po znaczniku w NVS ustawianym po pierwszym udanym montowaniu.
        bool was_ok_before = false;
        nvs_handle_t nh;
        if (nvs_open("sih_fs", NVS_READWRITE, &nh) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(nh, "mounted_ok", &v) == ESP_OK && v == 1) was_ok_before = true;
            nvs_close(nh);
        }
        if (was_ok_before) {
            ESP_LOGE(TAG, "SPIFFS mount blad: %s - NIE formatuje (chronie historie)",
                     esp_err_to_name(ret));
            s_fs_ok = false;
            return;
        }
        ESP_LOGW(TAG, "SPIFFS mount blad: %s - pierwsze uruchomienie, formatuje partycje",
                 esp_err_to_name(ret));
        conf.format_if_mount_failed = true;
        ret = esp_vfs_spiffs_register(&conf);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS format nieudany: %s", esp_err_to_name(ret));
            s_fs_ok = false;
            return;
        }
        ESP_LOGW(TAG, "SPIFFS sformatowany - historia bedzie zapisywana");
    }
    s_fs_ok = true;
    // FAZA 3a hotfix: przed jakimkolwiek load_meter posprzataj sierotki .tmp
    // po ewentualnym reset zasilania w mikrooknie miedzy remove i rename.
    hist_recover_stale_tmp();
    {   // znacznik po przywroceniu kopii - kasujemy od razu, dziala jednorazowo
        FILE *rf = fopen(REBASE_FLAG, "rb");
        if (rf) { fclose(rf); remove(REBASE_FLAG); s_rebase_pending = true;
            ESP_LOGW(TAG, "Po przywroceniu kopii: biezaca doba zostanie zaczeta od nowa"); }
    }
    // Zapamietaj, ze system plikow byl juz sprawny - od teraz nigdy nie formatujemy
    // automatycznie (ochrona historii przed skasowaniem po brudnym resecie).
    {
        nvs_handle_t nh;
        if (nvs_open("sih_fs", NVS_READWRITE, &nh) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(nh, "mounted_ok", &v) != ESP_OK || v != 1) {
                nvs_set_u8(nh, "mounted_ok", 1);
                nvs_commit(nh);
            }
            nvs_close(nh);
        }
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("littlefs", &total, &used);
    ESP_LOGI(TAG, "SPIFFS zamontowany: %u/%u B", (unsigned)used, (unsigned)total);
    // Dokoncz ewentualna przerwana migracje archiwum: marker wskazuje cel,
    // a plik tymczasowy zawiera juz pelna, poprawna kopie v2.
    {
        FILE *mk = fopen("/spiffs/arc_mig.dst", "rb");
        if (mk) {
            char dst[48] = {0};
            fread(dst, 1, sizeof(dst) - 1, mk);
            fclose(mk);
            if (dst[0]) {
                FILE *chk = fopen(dst, "rb");
                if (chk) {
                    fclose(chk);            // cel istnieje - migracja sie udala
                } else if (rename("/spiffs/arc_mig.tmp", dst) == 0) {
                    ESP_LOGW(TAG, "Dokonczono przerwana migracje archiwum %s", dst);
                }
            }
            remove("/spiffs/arc_mig.dst");
        }
    }
    remove("/spiffs/arc_mig.tmp");   // pozostalosc po ew. przerwanej migracji
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
            archive_rebuild_from_hours(m);  // uzupelnij archiwum ha_ z godzin w RAM
            // FAZA 9b: pierwszy start po aktualizacji - przenies krzywe dzis/wczoraj
            // z RAM do nowego archiwum 5-min (potem juz rosnie samo, co 5 min).
            curve_archive_seed(m);
            curve_archive_log_range(m->id);
            // Diagnostyka retencji: zasieg archiwum w logu przy kazdym starcie -
            // jesli poczatek kiedykolwiek "przeskoczy" do przodu, od razu widac.
            {
                char apath[48]; archive_path(m->id, apath, sizeof(apath));
                FILE *af = fopen(apath, "rb");
                if (af) {
                    arc_hdr_t ah;
                    if (arc_read_header2(af, &ah) && ah.count > 0) {
                        hist_bucket_t r0, r1;
                        int p0 = ah.head, p1 = (ah.head + ah.count - 1) % HIST_ARCHIVE_HOURS;
                        if (fseek(af, arc_rec_off2(ah.hdr, p0), SEEK_SET) == 0 &&
                            fread(&r0, sizeof(r0), 1, af) == 1 &&
                            fseek(af, arc_rec_off2(ah.hdr, p1), SEEK_SET) == 0 &&
                            fread(&r1, sizeof(r1), 1, af) == 1) {
                            ESP_LOGI(TAG, "Archiwum %s: %d godz, od ts=%u do ts=%u",
                                     m->id, ah.count, (unsigned)r0.ts, (unsigned)r1.ts);
                        }
                    }
                    fclose(af);
                }
            }
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
    if (!slot) {
        // Brak wolnego slotu - pole nie bedzie mialo historii ani wykresu.
        // Glosny log, bo wczesniej konczylo sie to cicho i wygladalo jak blad UI.
        ESP_LOGW(TAG, "%s: brak wolnego slotu historii (limit %d)", id, MAX_HIST_METERS);
        return NULL;
    }
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
    free_curve(slot);
    slot->used = false;
    return NULL;
}

// ---------- Glowne wejscie: nowy odczyt ----------
void history_on_reading(const char *id_hex, double total, int kind, uint32_t ts_unix) {
    if (!id_hex || ts_unix < 1700000000) return;  // wymaga zsynchronizowanego czasu
    // Historie zbieramy WYLACZNIE dla sledzonych licznikow. Zaslyszani sasiedzi
    // (kazdy nadajnik w zasiegu) zajmowali wczesniej slot 2.9KB na stercie i plik
    // na flashu, mimo ze nikt ich nie ogladal. Podglad ramek ("Lap licznik") i
    // dekodowanie dzialaja niezaleznie - to rezygnacja wylacznie z historii.
    if (!history_is_tracked(id_hex)) return;
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
    if (!m->cumulative)
        curve_add_sample_avg(m, ft, ts_unix);   // FAZA 5a: 5-min bucket + srednia

    // Archiwum godzinowe co ramke - ale tylko dla SLEDZONYCH licznikow (nie pisz
    // na flash dla kazdego zaslyszanego sasiada). To zrodlo odtwarzania stanu
    // biezacej godziny po restarcie (restore_from_archive).
    if (ts_unix - m->last_arc_ts >= 60 || floor_hour(ts_unix) != floor_hour(m->last_arc_ts)) {
        archive_append_hour(m->id, bh, ft, ts_unix);
        m->last_arc_ts = ts_unix;
    }

    // Zapis: przy zamknieciu godziny LUB co >=15 min (krzywa dnia + biezaca
    // godzina nie przepadaja przy utracie zasilania; kontrolowany restart
    // dodatkowo robi pelny history_flush()).
    if (hour_closed || ts_unix - m->last_save_ts >= 900) save_meter(m);

    if (s_mutex) xSemaphoreGive(s_mutex);
}

// Per-pole: klucz "id:pole". Zbiera TYLKO gdy pole sledzone (Etap A).
void history_on_field(const char *id_hex, const char *field, double value,
                      int kind, int cumulative, uint32_t ts_unix) {
    if (!id_hex || !field || ts_unix < 1700000000) return;
    char key[40];
    snprintf(key, sizeof(key), "%s:%s", id_hex, field);

    // ETAP A: zbieramy tylko sledzone pola (sasiedzi i niewybrane ignorowane)
    if (!history_is_tracked(key)) return;

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_hist_t *m = get_or_load(key, true);
    if (!m) { if (s_mutex) xSemaphoreGive(s_mutex); return; }

    float ft = (float)value;
    m->kind = kind;
    m->cumulative = cumulative;
    {
        uint32_t d0_now = floor_day(ts_unix);
        if (s_rebase_pending && !m->rebased) {
            // Pierwsza ramka po przywroceniu kopii. Danych z kopii NIE ruszamy -
            // maja zostac. Zapamietujemy tylko granice: zuzycie miedzy ostatnim
            // stanem z kopii a tym odczytem nie bylo rejestrowane, wiec pokazemy
            // je jako JEDEN slupek (a nie rozlozone po godzinach).
            m->rebase_ts = ts_unix;
            m->rebased   = true;
            ESP_LOGW(TAG, "%s: przywrocono kopie, roznica pokazana jako jeden slupek", m->id);
        }
        if (m->day_base_ts != d0_now) {
            if (m->last_ts == 0) {
                // Pierwszy odczyt tego licznika w ogole - to nasz punkt startowy.
                m->day_base_total = ft;  m->day_base_ts = d0_now;
            } else if (floor_day(m->last_ts) < d0_now) {
                // Realnie przekroczylismy polnoc przy pracujacym module.
                m->day_base_total = m->last_total;  m->day_base_ts = d0_now;
            } else if (m->day_base_ts == 0 && !m->rebased) {
                // Baza NIEZNANA mimo trwajacej doby - tak jest po aktualizacji
                // firmware lub resecie fabrycznym w srodku dnia. Kubelki
                // godzinowe i dobowe trzymaja OSTATNIA wartosc, wiec nie da sie
                // z nich odtworzyc stanu sprzed godziny - stad zera na wykresie
                // i na e-inku az do polnocy. Jedyne zrodlo wczesniejszych
                // wartosci to bufor odczytow na zywo: bierzemy z niego najstarszy
                // pomiar biezacej doby.
                float base = ft;
                for (int k = 0; k < m->n_rt; k++)
                    if (m->rt[k].ts >= d0_now && m->rt[k].total < base)
                        base = m->rt[k].total;
                m->day_base_total = base;
                m->day_base_ts    = d0_now;
            }
        }
    }
    m->last_total = ft;
    m->last_ts = ts_unix;

    uint32_t bh = floor_hour(ts_unix);
    bool hour_closed = (m->n_hours > 0 && m->hours[m->n_hours-1].ts != bh);

    series_update(m->hours,  &m->n_hours,  HIST_HOURS,  bh, ft);
    series_update(m->days,   &m->n_days,   HIST_DAYS,   floor_day(ts_unix),   ft);
    series_update(m->months, &m->n_months, HIST_MONTHS, floor_month(ts_unix), ft);
    series_update(m->years,  &m->n_years,  HIST_YEARS,  floor_year(ts_unix),  ft);
    rt_update(m, ts_unix, ft);
    if (!m->cumulative)
        curve_add_sample_avg(m, ft, ts_unix);   // FAZA 5a: 5-min bucket + srednia

    // Archiwum godzinowe na flash dla sledzonego pola - z throttlingiem 60 s:
    // ramki co ~10 s daja 6x mniej goracych zapisow (mniejsze ryzyko przerwania
    // zapisu w chwili resetu), a kubelek i tak trzyma ostatnia wartosc godziny.
    if (ts_unix - m->last_arc_ts >= 60 || floor_hour(ts_unix) != floor_hour(m->last_arc_ts)) {
        archive_append_hour(m->id, bh, ft, ts_unix);
        m->last_arc_ts = ts_unix;
    }

    if (hour_closed || ts_unix - m->last_save_ts >= 900) save_meter(m);
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
    // beta348: bufory hours/days/months trzymaja OSTATNIA wartosc swojego okresu,
    // wiec tuz po wlaczeniu sledzenia wszystkie maja ten sam, biezacy total i
    // powyzsze minimum wychodzi rowne stanowi biezacemu - zuzycie liczylo sie
    // jako 0 az do restartu, ktory wczytywal z flasha starsze punkty.
    // m->day_base_total to pierwszy odczyt doby (ten sam, ktorego uzywa wykres
    // dobowy i e-ink) - jedyna wartosc, ktora naprawde jest wczesniejsza.
    // Bierzemy ja, gdy nalezy do liczonego okresu i jest nizsza od dotychczasowej.
    // beta349: kierunek porownania byl odwrocony. day_base_ts to poczatek DOBY
    // (polnoc), a period_start dla widoku godzinowego to poczatek godziny -
    // polnoc jest wczesniejsza, wiec warunek "day_base_ts >= period_start" byl
    // falszywy i baza nigdy nie wchodzila. Dziala tylko dla widoku doby, gdzie
    // period_start == d0 == day_base_ts.
    // Poprawnie: pierwszy odczyt doby jest wiarygodnym WCZESNIEJSZYM punktem
    // dla kazdego okresu zaczynajacego sie w tej samej dobie lub pozniej.
    if (m->day_base_ts != 0 && period_start >= m->day_base_ts &&
        (!found || m->day_base_total < best)) {
        best = m->day_base_total;
        found = true;
    }
    *out = best;
    return found;
}

// Stan licznika (total) na moment ts: najswiezszy kubelek godzinowy o ts<=t.
// Zrodlo: RAM (godziny) i archiwum 2-letnie. Zwraca false gdy brak odniesienia.
static bool total_at_ts(meter_hist_t *m, uint32_t t, float *out) {
    bool found = false; uint32_t best_ts = 0; float best = 0;
    for (int i = 0; i < m->n_hours; i++) {
        if (m->hours[i].ts <= t && m->hours[i].ts >= best_ts) {
            best_ts = m->hours[i].ts; best = m->hours[i].total; found = true;
        }
    }
    if (found && best_ts + 7200 >= t) { *out = best; return true; }
    if (!s_fs_ok) { if (found) { *out = best; return true; } return false; }
    char path[64]; archive_path(m->id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) { if (found) { *out = best; return true; } return false; }
    arc_hdr_t h;
    if (arc_read_header2(f, &h) && h.count > 0) {
        int lo = 0, hi = h.count;               // pierwszy rekord z ts > t
        hist_bucket_t rec;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            int phys = (h.head + mid) % HIST_ARCHIVE_HOURS;
            if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) != 0 ||
                fread(&rec, sizeof(rec), 1, f) != 1) { lo = 0; break; }
            if (rec.ts <= t) lo = mid + 1; else hi = mid;
        }
        if (lo > 0) {                            // rekord tuz przed/na t
            int phys = (h.head + lo - 1) % HIST_ARCHIVE_HOURS;
            if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) == 0 &&
                fread(&rec, sizeof(rec), 1, f) == 1 && rec.ts <= t) {
                if (!found || rec.ts >= best_ts) { best = rec.total; found = true; }
            }
        }
    }
    fclose(f);
    if (found) { *out = best; return true; }
    return false;
}

// Kolejna granica kubelka: doba (+1 dzien) lub miesiac (+1 miesiac), lokalnie.
static uint32_t next_bucket(uint32_t t, char unit) {
    time_t tt = t; struct tm tm; localtime_r(&tt, &tm);
    if (unit == 'm') { tm.tm_mon += 1; tm.tm_mday = 1; }
    else             { tm.tm_mday += 1; }
    tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return (uint32_t)mktime(&tm);
}

int history_range_json(const char *id_hex, uint32_t from, uint32_t to,
                       char unit, char *buf, int buf_cap) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_hist_t *m = get_or_load(id_hex, false);
    if (!m) {
        int n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":0,\"points\":[]}",
                         id_hex ? id_hex : "");
        if (s_mutex) xSemaphoreGive(s_mutex);
        return n;
    }
    int n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"points\":[",
                     m->id, m->kind, m->cumulative);
    bool first = true;
    uint32_t now = (uint32_t)time(NULL);
    for (uint32_t b = from; b < to && n < buf_cap - 64; ) {
        uint32_t e = next_bucket(b, unit);
        if (b > now) break;                       // okresy przyszle - pomijamy
        float v = 0; bool have = false;
        if (m->cumulative) {
            float t0, t1;
            uint32_t end = (e > now) ? now : e;   // biezacy okres: stan "na teraz"
            if (total_at_ts(m, end, &t1)) {
                if (total_at_ts(m, b, &t0)) {
                    if (t1 >= t0) { v = t1 - t0; have = true; }
                } else {
                    // Brak stanu na POCZATKU okresu (dane zaczely sie w jego
                    // trakcie - np. pierwszy miesiac zbierania): licz przyrost
                    // od najwczesniejszego znanego stanu wewnatrz okresu,
                    // zamiast pomijac caly slupek.
                    float base;
                    if (earliest_total_in(m, b, t1, &base) && t1 >= base) {
                        v = t1 - base; have = true;
                    }
                }
            }
        } else {
            // chwilowe: srednia z kubelkow godzinowych okresu (RAM)
            float sum = 0; int cnt = 0;
            for (int i = 0; i < m->n_hours; i++) {
                if (m->hours[i].ts >= b && m->hours[i].ts < e) { sum += m->hours[i].total; cnt++; }
            }
            if (cnt) { v = sum / cnt; have = true; }
        }
        if (have) {
            n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                          first ? "" : ",", (unsigned)b, v);
            first = false;
        }
        b = e;
    }
    n += snprintf(buf + n, buf_cap - n, "]}");
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
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
int history_debug_day(const char *key, uint32_t day_ts, char *buf, int cap) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_hist_t *m = get_or_load(key, false);
    uint32_t d0 = floor_day(day_ts), d1 = d0 + 86400;
    int n = snprintf(buf, cap, "{\"key\":\"%s\",\"d0\":%u,\"d1\":%u", key ? key : "", (unsigned)d0, (unsigned)d1);
    if (!m) {
        n += snprintf(buf + n, cap - n, ",\"slot\":\"BRAK - licznik nie zaladowany\"}");
        if (s_mutex) xSemaphoreGive(s_mutex);
        return n;
    }
    bool ram_covers = (m->n_hours > 0 && m->hours[0].ts <= d0);
    n += snprintf(buf + n, cap - n,
        ",\"cumulative\":%d,\"kind\":%d,\"last_ts\":%u,\"last_total\":%.3f"
        ",\"n_hours\":%d,\"hours0_ts\":%u,\"hoursN_ts\":%u,\"ram_covers\":%s,\"n_curve\":%d",
        m->cumulative, m->kind, (unsigned)m->last_ts, (double)m->last_total,
        m->n_hours, (unsigned)(m->n_hours ? m->hours[0].ts : 0),
        (unsigned)(m->n_hours ? m->hours[m->n_hours-1].ts : 0),
        ram_covers ? "true" : "false", m->n_curve);
    int ram_in_day = 0;
    for (int i = 0; i < m->n_hours; i++)
        if (m->hours[i].ts >= d0 && m->hours[i].ts < d1) ram_in_day++;
    n += snprintf(buf + n, cap - n, ",\"ram_godzin_w_dobie\":%d", ram_in_day);
    char path[64]; archive_path(key, path, sizeof(path));
    FILE *f = s_fs_ok ? fopen(path, "rb") : NULL;
    if (!f) {
        n += snprintf(buf + n, cap - n, ",\"arch\":\"BRAK PLIKU %s\"}", path);
        if (s_mutex) xSemaphoreGive(s_mutex);
        return n;
    }
    arc_hdr_t h;
    if (!arc_read_header2(f, &h)) {
        n += snprintf(buf + n, cap - n, ",\"arch\":\"NAGLOWEK NIECZYTELNY\"}");
        fclose(f);
        if (s_mutex) xSemaphoreGive(s_mutex);
        return n;
    }
    n += snprintf(buf + n, cap - n, ",\"arc_count\":%d,\"arc_head\":%d,\"arc_v2\":%s,\"arc_hdr\":%d",
                  h.count, h.head, h.v2 ? "true" : "false", h.hdr);
    hist_bucket_t rec;
    int lo = 0, hi = h.count;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int phys = (h.head + mid) % HIST_ARCHIVE_HOURS;
        if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) != 0 ||
            fread(&rec, sizeof(rec), 1, f) != 1) { lo = h.count; break; }
        if (rec.ts < d0) lo = mid + 1; else hi = mid;
    }
    n += snprintf(buf + n, cap - n, ",\"bin_search_lo\":%d", lo);
    n += snprintf(buf + n, cap - n, ",\"arc_w_dobie\":[");
    int cnt = 0;
    for (int i = lo; i < h.count && cnt < 26 && n < cap - 80; i++) {
        int phys = (h.head + i) % HIST_ARCHIVE_HOURS;
        if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) != 0) break;
        if (fread(&rec, sizeof(rec), 1, f) != 1) break;
        if (rec.ts >= d1) break;
        if (rec.ts >= d0) {
            n += snprintf(buf + n, cap - n, "%s{\"ts\":%u,\"v\":%.3f}", cnt ? "," : "",
                          (unsigned)rec.ts, (double)rec.total);
            cnt++;
        }
    }
    n += snprintf(buf + n, cap - n, "],\"arc_znalezionych\":%d", cnt);
    if (lo > 0) {
        int phys = (h.head + lo - 1) % HIST_ARCHIVE_HOURS;
        if (fseek(f, arc_rec_off2(h.hdr, phys), SEEK_SET) == 0 &&
            fread(&rec, sizeof(rec), 1, f) == 1)
            n += snprintf(buf + n, cap - n, ",\"prev_rekord\":{\"ts\":%u,\"v\":%.3f}",
                          (unsigned)rec.ts, (double)rec.total);
    } else {
        n += snprintf(buf + n, cap - n, ",\"prev_rekord\":null");
    }
    fclose(f);
    n += snprintf(buf + n, cap - n, "}");
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
}

int history_get_day_json(const char *id_hex, uint32_t day_ts, char *buf, int buf_cap) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    // Pobierz metadane (kind, cumulative) z RAM jesli licznik zaladowany.
    meter_hist_t *m = get_or_load(id_hex, false);
    int kind = m ? m->kind : 0;
    int cumulative = m ? m->cumulative : 1;

    uint32_t d0 = floor_day(day_ts);          // poczatek wybranej doby
    uint32_t d1 = d0 + 86400;                  // poczatek nastepnej doby

    // Pola CHWILOWE (moc, napiecie): krzywa 1-min TYLKO gdy dla zadanej doby
    // faktycznie mamy punkty krzywej (biezaca doba). Dla starszych dni
    // przechodzimy do sciezki godzinowej - te dane leza w 2-letnim archiwum
    // na flashu dla KAZDEGO sledzonego pola, takze mocy i napiec. Wczesniej
    // ta galaz przechwytywala pole chwilowe zawsze i dla dni starszych niz
    // doba zwracala pusta liste, co wygladalo jak brak historii napiec/mocy.
    // FAZA 5a: wybor zrodla krzywej. curve_yesterday ma priorytet gdy jego
    // day_ts pasuje do zadanej doby (symetryczny wykres wczoraj/dzis 5-min).
    // Inaczej curve biezaca (jak dotad). Dla dni starszych niz wczoraj -
    // fallback do godzinnego archiwum ha_*.bin (kod ponizej).
    bool use_curve = false;
    hist_bucket_t *curve_src = NULL;
    int curve_src_n = 0;
    if (m && !cumulative && m->curve_yesterday && m->n_curve_yesterday > 0 &&
        m->curve_yesterday_day_ts == d0) {
        // Wczoraj z 5-min curve
        curve_src = m->curve_yesterday;
        curve_src_n = m->n_curve_yesterday;
        use_curve = true;
    } else if (m && !cumulative && m->curve && m->n_curve > 0) {
        bool has_day = false;
        for (int i = 0; i < m->n_curve; i++) {
            if (m->curve[i].ts >= d0 && m->curve[i].ts < d1) { has_day = true; break; }
        }
        if (has_day) {
            bool is_current_day = (m->last_ts >= d0 && m->last_ts < d1);
            bool fully_covered  = (m->curve[0].ts <= d0);   // ring zaczyna sie przed doba
            // Dzien historyczny tylko CZESCIOWO zahaczony przez ring (np. koncowka
            // wczorajszej doby) NIE moze przeslaniac pelnych 24 godzin z archiwum.
            use_curve = is_current_day || fully_covered;
            if (use_curve) {
                curve_src = m->curve;
                curve_src_n = m->n_curve;
            }
        }
    }
    // FAZA 9b: brak krzywej w RAM, ale pole jest chwilowe - sprobuj archiwum
    // 5-min na flashu, zanim spadniemy do slupkow godzinowych.
    static hist_bucket_t s_curve_day_buf[HIST_CURVE];
    if (!use_curve && m && !cumulative) {
        int na = curve_archive_read_day(id_hex, d0, d1, s_curve_day_buf, HIST_CURVE);
        if (na > 0) { curve_src = s_curve_day_buf; curve_src_n = na; use_curve = true; }
    }

    if (use_curve && curve_src) {
        int n2 = snprintf(buf, buf_cap,
                          "{\"id\":\"%s\",\"kind\":%d,\"cumulative\":0,\"curve\":1,\"points\":[",
                          id_hex ? id_hex : "", kind);
        bool first2 = true;
        for (int i = 0; i < curve_src_n && n2 < buf_cap - 48; i++) {
            uint32_t t = curve_src[i].ts;
            if (t < d0 || t >= d1) continue;
            n2 += snprintf(buf + n2, buf_cap - n2, "%s{\"t\":%u,\"v\":%.3f}",
                           first2 ? "" : ",", (unsigned)t, curve_src[i].total);
            first2 = false;
        }
        n2 += snprintf(buf + n2, buf_cap - n2, "]}");
        if (s_mutex) xSemaphoreGive(s_mutex);
        return n2;
    }

    int n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"points\":[",
                     id_hex ? id_hex : "", kind, cumulative);

    // Zbierz godziny wybranej doby (+1 poprzednia dla roznicy) do malego bufora.
    // ZRODLO 1: bufor hours[] w RAM (ostatnie 7 dni) - niezawodne, te same dane co
    // res=hour. Pokrywa Dzis/Wczoraj/caly tydzien.
    // ZRODLO 2 (fallback): archiwum ha_ na flash - dla dni starszych niz 7 dni.
    int nb = 0;
    hist_bucket_t prev; bool has_prev = false;

    // Z RAM czytamy TYLKO dzien w pelni pokryty oknem 168 godzin (RAM zaczyna
    // sie przed poczatkiem doby). Dzien GRANICZNY (dzis-7d) jest w RAM tylko
    // czesciowo i kurczy sie z kazda pelna godzina - wczesniej jedna znaleziona
    // godzina w RAM blokowala fallback do archiwum, wiec na tym dniu "znikaly"
    // slupki, mimo ze w archiwum na flashu lezaly nietkniete. Dzien niepokryty
    // od poczatku idzie w calosci z archiwum (kompletnego z dokladnoscia <=60 s).
    bool ram_covers = (m && m->n_hours > 0 && m->hours[0].ts <= d0);

    // Fallback do archiwum tylko gdy w RAM nie znaleziono godzin tej doby (starszy
    // lub pusty dzien). Rekordy w archiwum sa chronologiczne (rosnace ts) w
    // kolejnosci logicznej, wiec NIE skanujemy calego ringu (do 17520 rekordow po
    // jednym fseek+fread na SPIFFS = dziesiatki sekund z trzymanym s_mutex, co
    // blokowalo history_on_reading i zawieszalo modul przy przegladaniu dni bez
    // historii). Zamiast tego: wyszukiwanie binarne pierwszego rekordu doby
    // (~15 odczytow) + odczyt sekwencyjny do konca doby (max ~25 rekordow).
    if (!ram_covers) {
        char path[64]; archive_path(id_hex, path, sizeof(path));
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
    } else {
        for (int i = 0; i < m->n_hours && nb < ARC_DAY_BUF; i++) {
            if (m->hours[i].ts >= d0 && m->hours[i].ts < d1) {
                s_day_buf[nb++] = m->hours[i];
            } else if (m->hours[i].ts < d0) {
                prev = m->hours[i]; has_prev = true;
            }
        }
    }

    // SCALANIE RAM + archiwum: archiwum bywa NIEPELNE dla doby (np. 9 lipca -
    // zjedzony przez uszkodzony ring), a RAM ma te godziny. Wczesniej reguła
    // "albo RAM, albo archiwum" dawala PUSTY wykres mimo danych w pamieci.
    if (m) {
        uint32_t newest = (nb > 0) ? s_day_buf[nb-1].ts : 0;
        for (int i = 0; i < m->n_hours && nb < ARC_DAY_BUF; i++) {
            uint32_t t = m->hours[i].ts;
            if (t < d0) {
                if (!has_prev || t > prev.ts) { prev = m->hours[i]; has_prev = true; }
                continue;
            }
            if (t >= d1) break;
            if (t > newest || nb == 0) s_day_buf[nb++] = m->hours[i];
        }
    }

    // Wypisz punkty (zuzycie = roznica wzgledem poprzedniej godziny dla kumulacyjnych).
    // Luki NIE sa rozkladane na godziny: godziny bez ramek zostaja puste, a caly
    // przyrost z przerwy doliczany jest do pierwszego odczytu po niej. Dzieki temu
    // na wykresie widac, kiedy modul nie pracowal.
    bool first = true;
    for (int i = 0; i < nb && n < buf_cap - 60; i++) {
        if (cumulative) {
            hist_bucket_t *ref = NULL;
            if (i > 0) ref = &s_day_buf[i-1];
            else if (has_prev) ref = &prev;

            // Pierwszy kubelek doby bez odniesienia sprzed polnocy (np. modul
            // ruszyl dzis po resecie fabrycznym): uzyj stanu z pierwszego
            // odczytu tej doby - tego samego, ktorego uzywa e-ink. Bez tego
            // panel nie rysowal NIC, a e-ink pokazywal juz zuzycie.
            // Jeden slupek, bez rozkladania na godziny: zuzycie powstalo od
            // startu modulu, a nie od polnocy.
            // Punkt odniesienia doby ma PIERWSZENSTWO przed kubelkiem sprzed
            // polnocy. Jest dokladniejszy (stan dokladnie o polnocy, a nie o 23:00)
            // i po przywroceniu kopii to jedyna wiarygodna wartosc - wczorajszy
            // kubelek pochodzi sprzed kopii i odnosi sie do innego stanu licznika.
            // Punkt odniesienia doby tylko GDY BRAK kubelka sprzed polnocy.
            // Wczesniej mial pierwszenstwo zawsze - a po aktualizacji firmware
            // w srodku dnia jest ustawiany na biezacy odczyt, wiec pierwszy
            // slupek wychodzil ujemny i znikal (godzina 00:00 pokazywala zero).
            // Gdy kubelek sprzed polnocy istnieje, jest dokladniejszy.
            if (!ref && i == 0 && m && m->day_base_ts == d0) {
                float delta0 = s_day_buf[0].total - m->day_base_total;
                if (delta0 < 0) delta0 = 0;
                n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                              first ? "" : ",", (unsigned)s_day_buf[0].ts, delta0);
                first = false;
                continue;
            }

            if (ref && s_day_buf[i].ts > ref->ts) {
                uint32_t gap_h = (s_day_buf[i].ts - ref->ts) / 3600;
                float delta = s_day_buf[i].total - ref->total;
                if (delta < 0) delta = 0;
                (void)gap_h;
                // ZAWSZE jeden slupek w godzinie, w ktorej wrocil odczyt - takze
                // po przerwie. Rozkladanie przyrostu po godzinach luki zostalo
                // usuniete: godziny bez ramek maja pozostac puste, zeby bylo
                // widac, kiedy modul nie pracowal, a cale zuzycie z przerwy
                // doliczamy do pierwszego odczytu po niej.
                {
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
        if (!s_meters[i] || !s_meters[i]->used) continue;
        n += snprintf(buf + n, buf_cap - n, "%s{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"last\":%.3f,\"ts\":%u,\"tracked\":%s}",
                      first ? "" : ",", s_meters[i]->id, s_meters[i]->kind, s_meters[i]->cumulative,
                      s_meters[i]->last_total, (unsigned)s_meters[i]->last_ts,
                      history_is_tracked(s_meters[i]->id) ? "true" : "false");
        first = false;
    }
    n += snprintf(buf + n, buf_cap - n, "]");
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
}



// ----- API dla wyswietlacza e-ink -----

// Suma zuzycia doby [d0,d0+24h) liczona IDENTYCZNIE jak slupki wykresu
// (roznice kolejnych kubelkow godzinowych + rozklad luk <=6h proporcjonalnie,
// z przycieciem na granicy doby). Uzywana przez e-ink, zeby "dzis" na
// wyswietlaczu rownalo sie sumie slupkow w web UI.
// Dziala na godzinach w RAM; wymaga, by RAM pokrywal dobe od jej poczatku.
// need_cover=true  - wymagaj kubelka sprzed poczatku doby (pelne pokrycie).
// need_cover=false - policz z tego, co jest w dobie. Potrzebne dla doby
//   ZAMKNIETEJ na nowym module: gdy modul ruszyl np. o 8:00, kubelka sprzed
//   polnocy nie ma, ale zuzycie z tej doby jest znane i widac je w panelu.
static bool day_sum_from_hours_ex(const meter_hist_t *m, uint32_t d0,
                                  float *out_sum, bool need_cover) {
    if (!m || m->n_hours == 0) return false;
    if (need_cover && m->hours[0].ts > d0) return false;
    uint32_t d1 = d0 + 86400;
    float sum = 0;
    int counted = 0;
    const hist_bucket_t *ref = NULL;
    for (int i = 0; i < m->n_hours; i++) {
        const hist_bucket_t *b = &m->hours[i];
        if (b->ts >= d1) break;
        if (b->ts < d0) { ref = b; continue; }
        if (ref && b->ts > ref->ts) {
            counted++;
            uint32_t gap_h = (b->ts - ref->ts) / 3600;
            float delta = b->total - ref->total;
            if (delta < 0) delta = 0;
            // Bez rozkladania na godziny luki - caly przyrost liczy sie do
            // godziny, w ktorej wrocil odczyt (spojnie z wykresem).
            (void)gap_h;
            sum += delta;
        }
        ref = b;
    }
    if (!need_cover && counted == 0) return false;   // nic do policzenia
    *out_sum = sum;
    return true;
}

static bool day_sum_from_hours(const meter_hist_t *m, uint32_t d0, float *out_sum) {
    return day_sum_from_hours_ex(m, d0, out_sum, true);
}

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
        // ZUZYCIE DZIS - dokladnie tak, jak liczy to dashboard w panelu WWW.
        // Zrodlem sa kubelki godzinowe: series_update nadpisuje total biezacej
        // godziny przy KAZDEJ ramce, wiec wartosc rosnie na biezaco (e-ink
        // odswieza sie co 60 s i zawsze pokazuje aktualna liczbe).
        if (out->has_value) {
            float ds;
            if (day_sum_from_hours(m, day0, &ds)) {
                out->today = ds;                       // jak slupki na wykresie
            } else if (h1) {
                out->today = m->last_total - t_d1;     // stan teraz - koniec wczoraj
            } else if (m->day_base_ts == day0) {
                // Pierwsze godziny po instalacji - znamy stan z pierwszej ramki.
                out->today = m->last_total - m->day_base_total;
            } else {
                float base = 0;
                if (earliest_total_in(m, day0, m->last_total, &base) &&
                    m->last_total > base) {
                    out->today = m->last_total - base;
                } else {
                    out->today = 0;
                }
            }
            if (out->today < 0) out->today = 0;
            out->has_today = true;
        }
        // ZUZYCIE WCZORAJ - tak jak w panelu WWW, z kubelkow godzinowych.
        // Wczesniej wymagalismy wpisow dobowych z wczoraj I przedwczoraj (h1 && h2),
        // przez co NOWY modul nigdy nie pokazywal wczoraj - przedwczoraj jeszcze
        // nie istnialo, choc godzinowe dane z wczoraj byly (i widac je w UI).
        {
            float ys;
            if (day_sum_from_hours_ex(m, day_1, &ys, false)) {
                out->yesterday = ys;
                out->has_yesterday = true;
            } else if (h1 && h2) {
                out->yesterday = t_d1 - t_d2;
                out->has_yesterday = true;
            }
            if (out->yesterday < 0) out->yesterday = 0;
        }
        {
            float bs;
            if (day_sum_from_hours_ex(m, day_2, &bs, false)) {
                out->day_before = bs;
                out->has_day_before = true;
            } else if (h2 && h3) {
                out->day_before = t_d2 - t_d3;
                out->has_day_before = true;
            }
            if (out->day_before < 0) out->day_before = 0;
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

    bool cumul = m->cumulative;
    if (s_mutex) xSemaphoreGive(s_mutex);

    // JEDNO ZRODLO PRAWDY: dla licznikow kumulacyjnych zuzycie dzis/wczoraj
    // bierzemy z tej samej funkcji, ktora rysuje wykres w panelu. Wczesniej
    // e-ink liczyl po swojemu i obie strony potrafily pokazac co innego.
    // Wywolanie MUSI byc poza sekcja krytyczna - history_get_day_json samo
    // zaklada blokade.
    if (cumul) {
        uint32_t now_ts = (uint32_t)time(NULL);
        float v;
        if (history_day_sum(key, now_ts, &v)) {
            out->today = v; out->has_today = true;
        } else {
            out->today = 0; out->has_today = out->has_value;
        }
        if (history_day_sum(key, now_ts - 86400, &v)) {
            out->yesterday = v; out->has_yesterday = true;
        } else {
            out->has_yesterday = false;
        }
        if (history_day_sum(key, now_ts - 2 * 86400, &v)) {
            out->day_before = v; out->has_day_before = true;
        } else {
            out->has_day_before = false;
        }
    }
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

int history_keys_for_id(const char *id, char keys[][40], int max_keys) {
    if (!id) return 0;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    int count = 0;
    int idlen = (int)strlen(id);
    for (int i = 0; i < s_tracked_count && count < max_keys; i++) {
        // Klucz pasuje gdy zaczyna sie od "id" i dalej jest ':' lub koniec.
        if (strncasecmp(s_tracked[i], id, idlen) == 0 &&
            (s_tracked[i][idlen] == ':' || s_tracked[i][idlen] == '\0')) {
            strncpy(keys[count], s_tracked[i], 39);
            keys[count][39] = 0;
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

// Zrzuc na flash wszystkie zaladowane liczniki - wolane przed kontrolowanym
// restartem (przycisk restartu, start OTA), zeby krzywa dnia i biezaca godzina
// nie zaczynaly od zera po ponownym uruchomieniu.
bool history_fs_usage(size_t *used, size_t *total) {
    if (!s_fs_ok) return false;
    size_t u = 0, t = 0;
    if (esp_spiffs_info("littlefs", &t, &u) != ESP_OK) return false;
    if (used) *used = u;
    if (total) *total = t;
    return true;
}

void history_flush(void) {
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_HIST_METERS; i++) {
        if (s_meters[i] && s_meters[i]->used) save_meter(s_meters[i]);
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Historia zrzucona na flash (flush)");
}

int history_curve_day(const char *key, uint32_t day_ts, hist_bucket_t *out, int cap) {
    if (!key || !out || cap <= 0) return 0;
    int n = 0;
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    meter_hist_t *m = get_or_load(key, false);
    if (m && !m->cumulative) {
        uint32_t d0 = floor_day(day_ts), d1 = d0 + 86400;

        // FAZA 5a: jesli szukana doba pokrywa sie z curve_yesterday_day_ts,
        // zwrocic krzywa poprzedniego dnia (te same 5-min bucket'y co dzis,
        // symetryczny wykres). curve_yesterday jest zapisywana przy rotacji
        // doby i wczytywana z pliku h_*.bin v2 przy starcie modulu.
        if (m->curve_yesterday && m->n_curve_yesterday > 0 &&
            m->curve_yesterday_day_ts == d0) {
            for (int i = 0; i < m->n_curve_yesterday && n < cap; i++) {
                if (m->curve_yesterday[i].ts >= d0 && m->curve_yesterday[i].ts < d1)
                    out[n++] = m->curve_yesterday[i];
            }
        }
        // Fallback: krzywa dnia biezacego (jak dotad - lub gdy szukana doba to
        // "dzisiaj" wg last_ts i pokrywa sie z zakresem curve).
        else if (m->curve && m->n_curve > 0) {
            bool is_current_day = (m->last_ts >= d0 && m->last_ts < d1);
            bool fully_covered  = (m->curve[0].ts <= d0);
            if (is_current_day || fully_covered) {
                for (int i = 0; i < m->n_curve && n < cap; i++) {
                    if (m->curve[i].ts >= d0 && m->curve[i].ts < d1) out[n++] = m->curve[i];
                }
            }
            // FAZA 5a fix: nadpisz OSTATNI punkt biezacym last_total, zeby "teraz"
            // na wykresie historii bylo identyczne z dashboardem. Bez tego ostatni
            // punkt to running average jeszcze otwartego 5-min bucketu (curve_sum
            // /curve_count po kilku ramkach) - a dashboard pokazuje ostatnia
            // ramke wprost. Zamkniete buckety zostaja jako srednia z okna
            // (intencja FAZY 5a). Ruszamy tylko out[] zwracanego do JSON,
            // m->curve[] pozostaje z running average dla wewnetrznej logiki.
            if (n > 0 && m->curve_bucket_ts != 0 &&
                out[n-1].ts == m->curve_bucket_ts &&
                m->last_ts >= m->curve_bucket_ts) {
                out[n-1].total = m->last_total;
            }
        }

        // FAZA 9b: dni STARSZE niz wczoraj - z archiwum 5-min na flashu
        // (hc_*.bin). Wczesniej w tym miejscu zwracalismy 0 punktow i API
        // schodzilo do slupkow godzinowych; dlatego 5-min widac bylo tylko
        // przez dwie doby trzymane w RAM.
        if (n == 0) n = curve_archive_read_day(m->id, d0, d1, out, cap);
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return n;
}
