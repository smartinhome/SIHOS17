#include "history.h"
// Dlugosci nazw z readdir sa sprawdzane przed skladaniem sciezek - GCC tego
// nie widzi i zglasza mozliwe obciecie przy kazdym snprintf z d_name.
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "hist_store.h"     // beta443: uklad plikow na LittleFS
#include "hist_migrate.h"   // beta443: montowanie + przeniesienie z SPIFFS
#include "esp_littlefs.h"
#include "esp_log.h"
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>       // round() - przyrosty jak w JSON doby
#include <unistd.h>       // fsync, unlink, fileno
#include <sys/stat.h>     // stat() dla recovery sierotek .tmp
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"    // pomiar zapisow (beta438/440)
#include "esp_system.h"   // esp_restart (beta434)
#include "esp_attr.h"     // RTC_NOINIT_ATTR (beta434)
#include "freertos/task.h"

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
static bool flush_to_flash(FILE *f) {
    if (!f) return false;
    bool ok = fflush(f) == 0;
    int fd = fileno(f);
    if (fd >= 0 && fsync(fd) != 0) ok = false;
    return ok;
}

static const char *TAG = "HISTORY";

// beta434: SPIFFS przy nieudanym zamknieciu pliku (blad zapisu bufora) NIE
// zwalnia uchwytu. Po max_files takich zdarzeniach kazde otwarcie konczy sie
// EIO az do restartu - tak modul stracil zapisy od 21.11 do restartu 23.11.
// Teraz: bledy zamkniecia trafiaja do logu, a seria EIO przy otwieraniu
// powoduje ponowne zamontowanie systemu plikow (zwalnia wszystkie uchwyty).
// beta443: LittleFS nie gubi uchwytow, wiec ponowne montowanie zniknelo.
// Licznik zostaje tylko do logu.
static int      s_fs_eio_streak = 0;
// beta434: straznik zakleszczen. Modul zawieszal sie w calosci (WWW, MQTT,
// e-ink) i wracal dopiero po odcieciu zasilania - watchdog zadan w tej
// konfiguracji tylko ostrzega. Wszystko, co czyta lub pisze historie, czeka na
// s_mutex, wiec zadanie, ktore utknelo z ta blokada (np. w operacji SPIFFS),
// zatrzymuje caly modul. Blokade bierzemy z nazwa funkcji; osobny task co 30 s
// sprawdza, czy da sie ja wziac w 2 min. Jesli nie - zapisuje w pamieci RTC,
// kto ja trzymal, i restartuje modul. Po starcie ten wpis trafia do logu.
#define HIST_WD_PERIOD_MS  30000
#define HIST_WD_WAIT_MS    120000
#define HIST_WD_MAGIC      0x48574431u   // "HWD1"
static const char *volatile s_lock_owner = NULL;
static volatile TickType_t  s_lock_since = 0;
RTC_NOINIT_ATTR static uint32_t s_wd_magic;
RTC_NOINIT_ATTR static uint32_t s_wd_held_s;
RTC_NOINIT_ATTR static char     s_wd_owner[40];
static SemaphoreHandle_t s_mutex;
static void hist_lock(const char *who) {
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_lock_owner = who;
    s_lock_since = xTaskGetTickCount();
}
static void hist_unlock(void) {
    if (!s_mutex) return;
    s_lock_owner = NULL;
    xSemaphoreGive(s_mutex);
}
static bool fs_close(FILE *f, const char *path) {
    if (!f) return false;
    errno = 0;
    if (fclose(f) == 0) return true;
    ESP_LOGE(TAG, "Zamkniecie %s nieudane (errno=%d)", path ? path : "?", errno);
    s_fs_eio_streak++;
    return false;
}
static void fs_write_ok(void) { s_fs_eio_streak = 0; }
// beta443: przestoj zapisu - od kiedy archiwum godzinowe nie przyjmuje danych.
// Panel pokazuje ostrzezenie, zanim w historii zrobi sie dziura (21.11 zapisy
// staly 44 godziny i nikt tego nie widzial).
static uint32_t s_arc_fail_since = 0;     // 0 = zapisy dzialaja
static uint32_t s_arc_last_ok = 0;

// beta440: SPIFFS przy ciaglym przepisywaniu plikow z czasem przestaje nadazac
// ze sprzataniem - zapis trwa sekundy, a zalozenie nowego pliku potrafi zajac
// kilkanascie sekund i sie nie udac (modul wygladal na zawieszony). Mierzymy
// czas zapisow; gdy kilka z rzedu jest wolnych albo nieudanych, przechodzimy w
// tryb awaryjny: bez plikow stanu h_ (stan i tak odtwarza sie z archiwum) i z
// rzadszym zapisem archiwow. Stan trwa po restarcie (NVS) az do odbudowy.
// beta442: prog i okno. Dotad wystarczyly 3 zapisy dluzsze niz 2 s (licznik
// zerowal sie dopiero po 50 szybkich), a SPIFFS zapelniony w polowie robi takie
// przestoje przy porzadkowaniu miejsca regularnie - tryb wlaczal sie bez awarii
// i trwal do odbudowy, ktora kasowala krzywe 5-min. Teraz: 5 zlych (dluzszych
// niz 5 s albo nieudanych) w ostatnich 30 zapisach, a 200 dobrych z rzedu
// wylacza tryb samo.
#define FS_SLOW_MS        5000
#define FS_WIN            30     // okno ostatnich zapisow (bity s_fs_win)
#define FS_BAD_TRIP       5      // tyle zlych w oknie -> tryb awaryjny
#define FS_GOOD_EXIT      200    // tyle dobrych z rzedu -> koniec trybu awaryjnego
static bool     s_fs_degraded = false;
static uint32_t s_fs_win = 0;        // 1 = zly zapis, najmlodszy na bicie 0
static int      s_fs_good_run = 0;
static uint32_t s_fs_worst_ms = 0;
// beta442: ile zadan czyta teraz pliki z /spiffs poza blokada historii (kopia
// zapasowa). Ponowne montowanie systemu plikow w tym czasie wyrwaloby im pliki.
static volatile int s_fs_readers = 0;

static void fs_set_degraded(const char *why) {
    if (s_fs_degraded) return;
    s_fs_degraded = true;
    nvs_handle_t nh;
    if (nvs_open("sih_fs", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, "degraded", 1);
        nvs_commit(nh);
        nvs_close(nh);
    }
    ESP_LOGE(TAG, "System plikow nie nadaza (%s, najdluzszy zapis %u ms) - tryb awaryjny: "
                  "bez plikow stanu, archiwa rzadziej. Potrzebna odbudowa systemu plikow.",
             why, (unsigned)s_fs_worst_ms);
}

static void fs_clear_degraded(void) {
    if (!s_fs_degraded) return;
    s_fs_degraded = false;
    nvs_handle_t nh;
    if (nvs_open("sih_fs", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, "degraded", 0);
        nvs_commit(nh);
        nvs_close(nh);
    }
    ESP_LOGW(TAG, "System plikow znow nadaza (%d sprawnych zapisow z rzedu) - koniec trybu awaryjnego",
             FS_GOOD_EXIT);
}

static void fs_note_write(int64_t t0, bool ok, const char *what) {
    uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (ms > s_fs_worst_ms) s_fs_worst_ms = ms;
    bool bad = !ok || ms > FS_SLOW_MS;
    s_fs_win = ((s_fs_win << 1) | (bad ? 1u : 0u)) & ((1u << FS_WIN) - 1u);
    if (bad) {
        s_fs_good_run = 0;
        int n = __builtin_popcount(s_fs_win);
        ESP_LOGW(TAG, "Zapis %s %s (%u ms) - zlych w ostatnich %d: %d",
                 what ? what : "?", ok ? "wolny" : "nieudany", (unsigned)ms, FS_WIN, n);
        if (n >= FS_BAD_TRIP) fs_set_degraded(ok ? "wolne zapisy" : "nieudane zapisy");
    } else if (++s_fs_good_run >= FS_GOOD_EXIT) {
        s_fs_good_run = 0;
        if (s_fs_degraded) { s_fs_win = 0; fs_clear_degraded(); }
    }
}

void history_fs_hold(void)    { __atomic_add_fetch(&s_fs_readers, 1, __ATOMIC_SEQ_CST); }
void history_fs_release(void) { __atomic_sub_fetch(&s_fs_readers, 1, __ATOMIC_SEQ_CST); }

bool history_fs_degraded(void) { return s_fs_degraded; }
uint32_t history_fs_worst_ms(void) { return s_fs_worst_ms; }
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
    // beta361: czas najstarszego rekordu (pierwsza ramka). TYLKO W RAM - nie
    // trafia do h_*.bin, wiec format pliku sie nie zmienia. Liczony leniwie
    // przy pierwszym zapytaniu i odswiezany po restarcie; po zawinieciu ringu
    // (po latach) najstarszy rekord przesuwa sie do przodu, ale roznica jest
    // widoczna dopiero po ponownym uruchomieniu modulu.
    uint32_t first_ts;
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
    // beta380: kopia krzywej "wczoraj" (curve_yesterday + curve_yesterday_max,
    // 3,4 KB na kazde pole chwilowe) zniknela z RAM. Kazdy zamkniety kubelek
    // 5-min i tak laduje od razu w hc_/hm_ na flashu (curve_archive_append),
    // wiec po polnocy wczorajsza doba jest tam w komplecie - czytamy ja ta
    // sama droga co kazdy starszy dzien. Przy 7 polach to ok. 24 KB sterty.
    // Format pliku h_ (v2) zostaje: pole "wczoraj" zapisujemy jako puste,
    // a przy wczytywaniu starszego pliku przeskakujemy je bez alokacji.
    // beta370: obwiednia maksimow. curve_max[i] to najwyzsza probka kubelka
    // curve[i] - te same indeksy, ta sama dlugosc n_curve. Osobna tablica
    // float zamiast trzeciego pola w hist_bucket_t, bo ten typ opisuje takze
    // godziny, doby i lata (pliki h_ i ha_), gdzie maksimum nie ma sensu.
    // Umowa: max < srednia oznacza "nieznane" (maksimum okna nie moze byc
    // mniejsze od jego sredniej), wiec kubelki sprzed aktualizacji - zera w
    // swiezo zaalokowanej tablicy - same sie odsiewaja przy rysowaniu.
    float   *curve_max;                // NULL gdy pole spoza curve_max_wanted()
    float    curve_run_max;            // maksimum OTWARTEGO kubelka
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
// beta390: lista sledzonych pol wczytana z flasha. Wczesniej (Wi-Fi startuje
// przed historia) kazde pole wyglada na niesledzone.
static volatile bool s_tracked_ready = false;
// Po przywroceniu kopii stan licznika zdazyl sie zmienic, a zuzycie z tego
// okresu NIE zostalo zarejestrowane. Bez tego firmware probowal je dopisac do
// dzisiejszych godzin (jeden ogromny slupek albo kilka identycznych po rozlozeniu
// luki). Znacznik mowi: przy pierwszej ramce po restarcie zacznij dobe od nowa.
#define REBASE_FLAG "/spiffs/rebase.flg"
static bool s_rebase_pending = false;

// ---------- Pomocnicze: zaokraglenia czasu ----------
static uint32_t floor_hour(uint32_t t)  { return t - (t % 3600); }
// Alokacja bufora krzywej na zadanie; NULL-safe (bez krzywej gdy brak RAM).
// Zapas sterty, ktory musi zostac PO alokacji krzywej. Krzywa to
// HIST_CURVE(288) x hist_bucket_t(8 B) = 2304 B, a z bliznaczym buforem
// "wczoraj" 4608 B (4,6 KB) na pole. Amiplus ma ~10 pol chwilowych, wiec
// zaznaczenie ich wszystkich to ~45 KB - stad ten prog. Liczba 11,2 KB
// z poprzedniej wersji komentarza pochodzila z krzywej 1-minutowej
// (1440 punktow), sprzed przejscia na kubelek 5-minutowy w FAZIE 5a.
// Brak krzywej nie psuje wykresu -
// dzien rysuje sie wtedy z kubelkow godzinowych (grubsza rozdzielczosc).
// Bufor na JSON doby przy liczeniu sumy dla e-ink: max ~48 slupkow po ~30 B.
#define DAY_SUM_BUF 3072

#define CURVE_HEAP_RESERVE (60 * 1024)

// beta370: obwiednia maksimow tylko tam, gdzie szczyt niesie informacje.
// Moc czynna i bierna - bo interesuje nas, kiedy pobor naprawde skoczyl.
// Napiecia - bo skok i zapad to zdarzenia, ktore srednia z 5 minut zaciera.
// Prady i reszta zostaja przy samej sredniej: nie placimy pamiecia za dane,
// ktorych nikt nie oglada.
static bool curve_max_wanted(const char *key) {
    if (!key) return false;
    const char *f = strchr(key, ':');
    f = f ? f + 1 : key;
    return strcmp(f, "moc_kw") == 0 ||
           strncmp(f, "moc_bierna", 10) == 0 ||
           strncmp(f, "napiecie", 8) == 0;
}

static bool ensure_curve(meter_hist_t *m) {
    if (m->curve) return true;
    size_t need = HIST_CURVE * sizeof(hist_bucket_t);
    if (curve_max_wanted(m->id)) need += HIST_CURVE * sizeof(float);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (freeb < need + CURVE_HEAP_RESERVE) {
        ESP_LOGW(TAG, "%s: brak RAM na krzywa minutowa (wolne %u B) - dane godzinowe",
                 m->id, (unsigned)freeb);
        m->n_curve = 0;
        return false;
    }
    m->curve = calloc(HIST_CURVE, sizeof(hist_bucket_t));
    if (!m->curve) { m->n_curve = 0; return false; }
    // beta370: brak RAM na obwiednie nie jest bledem - krzywa srednich dziala
    // dalej, po prostu bez tla maksimow.
    if (!m->curve_max && curve_max_wanted(m->id)) {
        m->curve_max = calloc(HIST_CURVE, sizeof(float));
        if (!m->curve_max)
            ESP_LOGW(TAG, "%s: brak RAM na obwiednie maksimow", m->id);
    }
    return true;
}
static void free_curve(meter_hist_t *m) {
    if (m->curve) { free(m->curve); m->curve = NULL; }
    m->n_curve = 0;
    // beta370: obwiednie ida razem z krzywymi, ktore opisuja.
    if (m->curve_max) { free(m->curve_max); m->curve_max = NULL; }
    m->curve_run_max = 0.0f;
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
// beta370: maxfile=false -> hc_*.bin (srednie, jak dotad), true -> hm_*.bin
// (maksima, plik zakladany dopiero po tej aktualizacji).
static bool curve_archive_append(const char *id, uint32_t bucket_ts, float value,
                                 bool maxfile);

// beta370: series_update dla krzywej - utrzymuje curve[] i curve_max[] w
// idealnej zgodnosci indeksow. Osobne wywolanie series_update dla kazdej
// tablicy rozjechaloby je przy przewijaniu ringu (memmove w jednej, brak
// w drugiej), a caly odczyt opiera sie na tym, ze indeks i znaczy w obu to samo.
static void curve_update(meter_hist_t *m, uint32_t bts, float avg, float mx) {
    int *n = &m->n_curve;
    if (*n > 0 && m->curve[*n - 1].ts == bts) {
        m->curve[*n - 1].total = avg;
        if (m->curve_max) m->curve_max[*n - 1] = mx;
        return;
    }
    if (*n < HIST_CURVE) {
        m->curve[*n].ts = bts; m->curve[*n].total = avg;
        if (m->curve_max) m->curve_max[*n] = mx;
        (*n)++;
        return;
    }
    memmove(m->curve, m->curve + 1, (HIST_CURVE - 1) * sizeof(hist_bucket_t));
    m->curve[HIST_CURVE - 1].ts = bts;
    m->curve[HIST_CURVE - 1].total = avg;
    if (m->curve_max) {
        memmove(m->curve_max, m->curve_max + 1, (HIST_CURVE - 1) * sizeof(float));
        m->curve_max[HIST_CURVE - 1] = mx;
    }
}

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
                curve_update(m, m->curve_bucket_ts, avg, m->curve_run_max);
                // FAZA 9b: ostatni kubelek doby trafia na flash zanim curve[]
                // zostanie wyczyszczona pod nowa dobe.
                curve_archive_append(m->id, m->curve_bucket_ts, avg, false);
                if (m->curve_max)
                    curve_archive_append(m->id, m->curve_bucket_ts, m->curve_run_max, true);
            }
            // beta380: bez kopiowania do RAM - miniona doba jest juz w hc_/hm_.
            ESP_LOGI(TAG, "%s: nowa doba, %d kubelkow 5-min zostaje w archiwum",
                     m->id, m->n_curve);
            // Reset curve na nowa dobe.
            m->n_curve = 0;
            m->curve_sum = 0.0f;
            m->curve_count = 0;
            m->curve_bucket_ts = 0;
            m->curve_run_max = 0.0f;
            memset(m->curve, 0, HIST_CURVE * sizeof(hist_bucket_t));
            if (m->curve_max) memset(m->curve_max, 0, HIST_CURVE * sizeof(float));
        }
    }

    // 2. Detekcja przejscia bucketu 5-min - zamknij poprzedni (final srednia).
    if (m->curve_bucket_ts != bucket_ts) {
        if (m->curve_count > 0 && m->curve_bucket_ts != 0) {
            float avg = m->curve_sum / (float)m->curve_count;
            curve_update(m, m->curve_bucket_ts, avg, m->curve_run_max);
            // FAZA 9b: kubelek jest domkniety - zapisz go do archiwum 5-min.
            // Jeden zapis na 5 min na pole (60x rzadziej niz archiwum godzinowe,
            // ktore pisze co ramke) - zuzycie flasha praktycznie bez zmian.
            curve_archive_append(m->id, m->curve_bucket_ts, avg, false);
            // beta370: drugi plik, ten sam rytm zapisu.
            if (m->curve_max)
                curve_archive_append(m->id, m->curve_bucket_ts, m->curve_run_max, true);
        }
        m->curve_bucket_ts = bucket_ts;
        m->curve_sum = 0.0f;
        m->curve_count = 0;
        m->curve_run_max = value;      // beta370: nowe okno zaczyna sie od tej probki
    }

    // 3. Dodaj probke do biezacej sredniej + zapisz partial average do curve
    //    (dzieki temu bufor pokazuje aktualna srednia BIEZACEGO bucketu, nie
    //    zamknieta poprzednia).
    m->curve_sum += value;
    m->curve_count++;
    // beta370: maksimum biezacego okna rosnie razem ze srednia, wiec otwarty
    // kubelek ma juz sensowna obwiednie - nie czeka na domkniecie.
    if (m->curve_count == 1 || value > m->curve_run_max) m->curve_run_max = value;
    curve_update(m, bucket_ts, m->curve_sum / (float)m->curve_count, m->curve_run_max);
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
// beta419: poczatek NASTEPNEJ doby. Doba ma 23, 24 albo 25 godzin (zmiana
// czasu), wiec d0 + 86400 w dobie przestawienia zegara na zimowy gubilo
// godzine 23:00 (a w marcu dokladalo 00:00 kolejnego dnia). d0 + 26 h zawsze
// wypada w nastepnej dobie, przed jej 03:00.
static uint32_t day_end(uint32_t d0) {
    return floor_day(d0 + 26 * 3600);
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

// ================= beta443: archiwa na LittleFS (hist_store.c) =================
// Do beta442 archiwum godzinowe (ha_) i krzywe 5-min (hc_/hm_) byly
// pierscieniami w jednym pliku z naglowkiem przepisywanym przy kazdym zapisie.
// Teraz kazdy klucz ma katalog z plikiem na miesiac (godziny) i na tydzien
// (krzywe), a pliki rosna tylko na koncu - opis ukladu w hist_store.h.
// Funkcje ponizej to cienka warstwa: klucz -> safe_key -> hs_*.
#define ARC_DAY_BUF 64   // rekordow do bufora dnia (doba=24, zapas)
static hist_bucket_t s_day_buf[ARC_DAY_BUF];
#define ARC_TS_MIN 1700000000u
#define ARC_TS_MAX 2100000000u

// beta361: czas NAJSTARSZEGO rekordu archiwum godzinowego. Panel wyszarza po
// nim dni sprzed pierwszej ramki.
static uint32_t arc_first_ts(const char *id) {
    char safe[SAFE_KEY_MAX + 1];
    safe_key(id, safe);
    hist_bucket_t r;
    return hs_arc_first(safe, &r) ? r.ts : 0;
}

static void arc_note(bool ok, const char *id) {
    uint32_t now = (uint32_t)time(NULL);
    if (ok) {
        if (s_arc_fail_since)
            ESP_LOGW(TAG, "Zapis archiwum znow dziala (przerwa %u s)",
                     (unsigned)(now - s_arc_fail_since));
        s_arc_fail_since = 0;
        s_arc_last_ok = now;
        fs_write_ok();
        return;
    }
    s_fs_eio_streak++;
    if (!s_arc_fail_since) {
        s_arc_fail_since = now;
        ESP_LOGE(TAG, "Zapis archiwum %s nieudany - historia nie jest zapisywana", id ? id : "?");
    }
}

// Zapis godziny do archiwum: nowa godzina dopisywana, ta sama nadpisywana.
// ts_exact = dokladny czas ostatniego odczytu (last.bin, dla dashboardu).
static bool archive_append_hour(const char *id, uint32_t hour_ts, float total,
                                uint32_t ts_exact) {
    if (!s_fs_ok) return false;
    if (hour_ts < ARC_TS_MIN || hour_ts > ARC_TS_MAX) return true;
    char safe[SAFE_KEY_MAX + 1];
    safe_key(id, safe);
    bool ok = hs_arc_put(safe, hour_ts, total, ts_exact);
    arc_note(ok, id);
    return ok;
}

// FAZA 9b / beta370: zamkniety kubelek 5-min (srednia albo maksimum).
static bool curve_archive_append(const char *id, uint32_t bucket_ts, float value,
                                 bool maxfile) {
    if (!s_fs_ok || !id) return false;
    if (bucket_ts < ARC_TS_MIN || bucket_ts > ARC_TS_MAX) return true;
    char safe[SAFE_KEY_MAX + 1];
    safe_key(id, safe);
    return hs_crv_put(safe, bucket_ts, value, maxfile);
}

// Punkty 5-min doby [d0, d1) z archiwum. Zwraca liczbe punktow.
static int curve_archive_read_day(const char *id, uint32_t d0, uint32_t d1,
                                  hist_bucket_t *out, int cap, bool maxfile) {
    if (!s_fs_ok || !id || !out || cap <= 0) return 0;
    char safe[SAFE_KEY_MAX + 1];
    safe_key(id, safe);
    return hs_crv_read(safe, d0, d1, out, cap, maxfile);
}

// beta370: wspolny bufor na dobe maksimow. Uzywany przez odtwarzanie po
// restarcie i przez budowanie JSON-a - nigdy jednoczesnie (oba pod mutexem
// historii, a odtwarzanie konczy sie zanim ruszy czytanie doby).
static hist_bucket_t s_curve_max_buf[HIST_CURVE];

// beta370: po restarcie curve_max[] jest puste, a curve[] wraca z pliku h_ juz
// z kubelkami sprzed restartu - obwiednia mialaby dziure od polnocy do chwili
// wlaczenia modulu. Maksima tych kubelkow leza w archiwum, wiec wystarczy jedno
// czytanie przy wczytywaniu licznika (nie przy kazdym zapytaniu o wykres).
static void curve_max_seed_today(meter_hist_t *m) {
    if (!m || !m->curve || !m->curve_max || m->n_curve <= 0) return;
    uint32_t now = (uint32_t)time(NULL);
    if (now < ARC_TS_MIN) return;                 // czas niezsynchronizowany
    uint32_t d0 = floor_day(now);
    int n = curve_archive_read_day(m->id, d0, day_end(d0),
                                   s_curve_max_buf, HIST_CURVE, true);
    if (n <= 0) return;
    // Obie listy sa posortowane rosnaco po ts - jedno przejscie scalajace.
    int j = 0, hit = 0;
    for (int i = 0; i < m->n_curve && j < n; i++) {
        while (j < n && s_curve_max_buf[j].ts < m->curve[i].ts) j++;
        if (j < n && s_curve_max_buf[j].ts == m->curve[i].ts) {
            m->curve_max[i] = s_curve_max_buf[j].total;
            j++; hit++;
        }
    }
    if (hit > 0)
        ESP_LOGI(TAG, "%s: odtworzono %d maksimow 5-min z archiwum", m->id, hit);
}

// Krzywa dnia z RAM (z pliku h_) dopisana do archiwum 5-min, jesli archiwum
// jej nie ma (np. kopia zapasowa bez krzywych). Rekordy, ktore archiwum juz
// zna, sa pomijane - przy normalnej pracy nic sie nie zapisuje.
static void curve_archive_seed(meter_hist_t *m) {
    if (!s_fs_ok || !m || m->cumulative || !m->curve || m->n_curve <= 0) return;
    char safe[SAFE_KEY_MAX + 1];
    safe_key(m->id, safe);
    // Ostatni kubelek to srednia sprzed restartu - lepsza niz dziura; gdy ten
    // kubelek jeszcze trwa, jego domkniecie nadpisze rekord (ten sam ts).
    int n = m->n_curve;
    int k = 0;
    while (k < n && (m->curve[k].ts < ARC_TS_MIN || m->curve[k].ts > ARC_TS_MAX)) k++;
    if (k < n) hs_crv_put_many(safe, m->curve + k, n - k, false);
}

// Uzupelnij archiwum godzinami z RAM nowszymi niz jego ostatni rekord (plik h_
// bywa swiezszy, np. po przywroceniu kopii z innego okresu). Nic nie nadpisuje.
static void archive_rebuild_from_hours(meter_hist_t *m) {
    if (!s_fs_ok || m->n_hours == 0) return;
    char safe[SAFE_KEY_MAX + 1];
    safe_key(m->id, safe);
    hist_bucket_t last;
    uint32_t lts = hs_arc_last(safe, &last) ? last.ts : 0;
    int added = 0;
    for (int i = 0; i < m->n_hours; i++) {
        const hist_bucket_t *b = &m->hours[i];
        if (b->ts <= lts || b->ts < ARC_TS_MIN || b->ts > ARC_TS_MAX) continue;
        uint32_t ex = (i == m->n_hours - 1 && m->last_ts >= b->ts) ? m->last_ts : 0;
        if (!hs_arc_put(safe, b->ts, b->total, ex)) break;
        added++;
    }
    if (added) ESP_LOGI(TAG, "Archiwum %s uzupelnione o %d godz z RAM", m->id, added);
}

// Odtworz z archiwum najswiezszy stan po restarcie. Plik h_ zapisywany jest raz
// na godzine, archiwum co kilka minut - godziny nowsze niz ostatnia z h_ (do 7
// dni) trafiaja do serii w RAM, a last.bin daje dokladny czas i stan ostatniego
// odczytu sprzed restartu.
static void restore_from_archive(meter_hist_t *m) {
    if (!s_fs_ok) return;
    char safe[SAFE_KEY_MAX + 1];
    safe_key(m->id, safe);
    uint32_t last_bh = (m->n_hours > 0) ? m->hours[m->n_hours - 1].ts : 0;
    hist_bucket_t newest;
    int added = 0;
    if (hs_arc_last(safe, &newest) && newest.ts >= last_bh) {
        uint32_t from = last_bh;
        uint32_t win = (HIST_HOURS - 1) * 3600u;
        if (newest.ts > win && from < newest.ts - win) from = newest.ts - win;
        hs_iter_t it;
        hs_iter_open(&it, safe, from, newest.ts + 1);
        hist_bucket_t chunk[32], rec = { 0 };
        bool any = false;
        int r;
        while ((r = hs_iter_next(&it, chunk, 32)) > 0) {
            for (int j = 0; j < r; j++) {
                const hist_bucket_t *b = &chunk[j];
                if (b->ts < from || b->ts < ARC_TS_MIN) continue;
                if (any && b->ts <= rec.ts) continue;
                series_update(m->hours,  &m->n_hours,  HIST_HOURS,  b->ts, b->total);
                series_update(m->days,   &m->n_days,   HIST_DAYS,   floor_day(b->ts),   b->total);
                series_update(m->months, &m->n_months, HIST_MONTHS, floor_month(b->ts), b->total);
                series_update(m->years,  &m->n_years,  HIST_YEARS,  floor_year(b->ts),  b->total);
                if (b->ts > last_bh) added++;
                rec = *b; any = true;
            }
        }
        hs_iter_close(&it);
        if (added > 1)
            ESP_LOGI(TAG, "%s: dolozono %d godz z archiwum (plik stanu byl starszy)", m->id, added);
        if (any && rec.ts >= floor_hour(m->last_ts)) {
            m->last_total = rec.total;
            if (rec.ts > m->last_ts) m->last_ts = rec.ts;
        }
    }
    uint32_t lts; float ltot;
    if (hs_arc_last_exact(safe, &lts, &ltot) && lts >= m->last_ts) {
        m->last_ts = lts;
        m->last_total = ltot;
    }
    ESP_LOGI(TAG, "Odtworzono %s: last_ts=%u total=%.3f", m->id,
             (unsigned)m->last_ts, (double)m->last_total);
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

static bool save_meter_raw(meter_hist_t *m) {
    if (!s_fs_ok) return false;
    char path[64]; meter_path(m->id, path, sizeof(path));
    // Atomic write: zapis do .tmp, potem fsync + rename. Reset zasilania w
    // srodku fwrite() zostawia .tmp jako smiec (zignorowany przy starcie),
    // ale oryginalny .bin jest nadal caly - historia sie nie traci.
    char tmp[64]; path_to_tmp(path, tmp, sizeof(tmp));
    // beta438: pierwsze zapisy po starcie w logu - modul stawal tuz po
    // pierwszej ramce, czyli przy pierwszym zapisie pliku stanu. Ostatnia linia
    // przed zawieszeniem pokaze, ktory zapis go zatrzymal.
    static int s_save_log_n = 0;
    bool lg = s_save_log_n < 30;
    if (lg) { s_save_log_n++; ESP_LOGI(TAG, "zapis stanu %s - start", m->id); }
    errno = 0;
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        int e = errno;
        ESP_LOGW(TAG, "nie moge zapisac %s (errno=%d)", tmp, e);
        s_fs_eio_streak++;
        return false;
    }
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
    // beta380: "wczoraj" nie siedzi juz w RAM (jest w hc_/hm_), wiec pole
    // zapisujemy zawsze jako puste. Uklad pliku v2 bez zmian.
    uint32_t cy_day = 0;
    int      cy_n   = 0;
    W(&cy_day, sizeof(uint32_t));
    W(&cy_n,   sizeof(int));
    #undef W
    #undef WARR
    // Wpisz obliczone CRC w miejscu placeholdera (offset 8 = po magic+ver).
    fseek(f, 8, SEEK_SET);
    fwrite(&crc, sizeof(uint32_t), 1, f);
    bool wr_ok = !ferror(f);
    if (!flush_to_flash(f)) wr_ok = false;   // wymuszona synchronizacja przed rename
    if (!fs_close(f, tmp)) wr_ok = false;
    // beta434: niepelny .tmp NIE moze zastapic dobrego pliku. Dotad blad zapisu
    // konczyl sie usunieciem .bin i podstawieniem ucietej kopii (CRC mismatch
    // przy starcie). Teraz poprzedni stan zostaje na flashu.
    if (!wr_ok) {
        ESP_LOGW(TAG, "zapis %s niepelny - zostaje poprzedni %s", tmp, path);
        unlink(tmp);
        return false;
    }
    // beta443: LittleFS podmienia plik docelowy w jednym kroku (SPIFFS wymagal
    // usuniecia go przed rename - stad dawne okno bez pliku stanu).
    if (rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "atomic rename %s -> %s nieudany (errno=%d)", tmp, path, errno);
        unlink(tmp);   // historia zostaje w poprzednim .bin
        return false;
    }
    m->last_save_ts = m->last_ts;
    fs_write_ok();
    if (lg) ESP_LOGI(TAG, "zapis stanu %s - OK", m->id);
    return true;
}

static void save_meter(meter_hist_t *m) {
    if (s_fs_degraded) {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            ESP_LOGW(TAG, "Tryb awaryjny: pliki stanu nie sa zapisywane - historia idzie do archiwow");
        }
        return;
    }
    int64_t t0 = esp_timer_get_time();
    bool ok = save_meter_raw(m);
    fs_note_write(t0, ok, m->id);
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
                // beta380: plik sprzed aktualizacji ma jeszcze kopie "wczoraj".
                // Przeskakujemy ja bez alokacji (CRC liczymy po drodze); przy
                // nastepnym zapisie pole bedzie juz puste.
                hist_bucket_t tmp;
                for (int k = 0; k < cy_n; k++) {
                    if (fread(&tmp, sizeof(tmp), 1, f) != 1) break;
                    if (v1) crc = hist_crc32_update(crc, &tmp, sizeof(tmp));
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

// beta441: odbudowa systemu plikow z kopii. Zamiast kasowac pliki jeden po
// drugim (na zapchanym SPIFFS to tez trwa i sie nie udaje) formatujemy cala
// partycje; potem handle_backup_post wgrywa pliki z kopii na czysty system
// plikow i restartuje modul. RAM historii zwalniamy pod blokada, a samo
// formatowanie (do minuty) idzie juz bez niej - inaczej straznik historii
// uznalby to za zawieszenie. Do restartu historia nie dotyka plikow.
bool history_fs_rebuild(void) {
    hist_lock(__func__);
    for (int i = 0; i < MAX_HIST_METERS; i++) {
        if (s_meters[i]) {
            free_curve(s_meters[i]);
            free(s_meters[i]);
            s_meters[i] = NULL;
        }
    }
    s_tracked_count = 0;
    memset(s_tracked, 0, sizeof(s_tracked));
    s_fs_ok = false;
    hist_unlock();

    ESP_LOGW(TAG, "Odbudowa: formatowanie pamieci historii...");
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = esp_littlefs_format("littlefs");   // beta443: sekunda, nie minuta
    ESP_LOGW(TAG, "Odbudowa: formatowanie %s po %u s", esp_err_to_name(r),
             (unsigned)((esp_timer_get_time() - t0) / 1000000));
    if (r != ESP_OK) return false;

    // Czysty system plikow - koniec trybu awaryjnego.
    s_fs_degraded = false;
    s_fs_win = 0;
    s_fs_good_run = 0;
    nvs_handle_t nh;
    if (nvs_open("sih_fs", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_u8(nh, "degraded", 0);
        nvs_commit(nh);
        nvs_close(nh);
    }
    return true;
}

// Usuwa zawartosc katalogu (do 3 poziomow w glab: a/<klucz>/plik). Kasowanie
// po zamknieciu katalogu - usuwanie w trakcie readdir potrafi pominac wpisy.
static int rm_tree(const char *dir, int depth) {
    int removed = 0;
    for (int pass = 0; pass < 20; pass++) {
        DIR *d = opendir(dir);
        if (!d) break;
        char names[16][48];
        int nv = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL && nv < 16) {
            if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(names[0])) continue;
            snprintf(names[nv++], sizeof(names[0]), "%s", de->d_name);
        }
        closedir(d);
        if (nv == 0) break;
        int gone = 0;
        for (int k = 0; k < nv; k++) {
            char p[128];
            snprintf(p, sizeof(p), "%s/%s", dir, names[k]);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (depth < 3) removed += rm_tree(p, depth + 1);
                if (rmdir(p) == 0) gone++;
            } else if (remove(p) == 0) {
                removed++; gone++;
            }
        }
        if (gone == 0) break;
    }
    return removed;
}

void history_erase_all(void) {
    // Reset fabryczny: kasuje CALA historie z flasha i liste sledzonych.
    // Konfiguracja siedzi w NVS (nvs_config_reset), ale historia to osobny
    // system plikow - bez tego po resecie zostawaly wykresy i zaznaczone pola.
    // beta442: formatowanie partycji zamiast kasowania pliku po pliku. Kasowanie
    // trzymalo blokade historii, a na zapchanym SPIFFS trwalo dluzej niz 2 min -
    // straznik historii restartowal modul w polowie (przy przywracaniu kopii:
    // konfiguracja juz z kopii, historia skasowana tylko czesciowo, kopia niewgrana).
    if (history_fs_rebuild()) {
        ESP_LOGW(TAG, "Reset: pamiec historii sformatowana");
        return;
    }
    // Formatowanie nieudane - kasujemy pojedynczo, bez blokady: RAM historii jest
    // juz zwolniony, a s_fs_ok = false, wiec nikt inny tych plikow nie dotknie.
    ESP_LOGW(TAG, "Reset: formatowanie nieudane - kasuje pliki historii pojedynczo");
    int removed = rm_tree("/spiffs", 0);
    ESP_LOGW(TAG, "Reset: skasowano %d plikow historii", removed);
}

size_t history_free_curves(void) {
    // Krzywe 5-minutowe (2,3 KB, z buforem "wczoraj" 4,6 KB na pole) sa
    // odtwarzalne - po restarcie i tak buduja sie od nowa, a dane godzinowe
    // zostaja. Zwalniamy je przed OTA, bo TLS do GitHuba
    // potrzebuje sporego, spojnego kawalka sterty.
    size_t freed = 0;
    hist_lock(__func__);
    for (int i = 0; i < MAX_HIST_METERS; i++) {
        if (!s_meters[i]) continue;
        if (s_meters[i]->curve)     freed += HIST_CURVE * sizeof(hist_bucket_t);
        // beta370: obwiednie tez sa odtwarzalne - siedza w hm_ na flashu.
        if (s_meters[i]->curve_max) freed += HIST_CURVE * sizeof(float);
        if (s_meters[i]->curve) free_curve(s_meters[i]);
    }
    hist_unlock();
    ESP_LOGI(TAG, "Zwolniono %u B buforow krzywych (OTA)", (unsigned)freed);
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
bool history_tracked_ready(void) { return s_tracked_ready; }

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
    hist_lock(__func__);
    int n = snprintf(buf, buf_cap, "[");
    for (int i = 0; i < s_tracked_count; i++) {
        n += snprintf(buf + n, buf_cap - n, "%s\"%s\"", i ? "," : "", s_tracked[i]);
    }
    n += snprintf(buf + n, buf_cap - n, "]");
    hist_unlock();
    return n;
}

void history_set_tracked(const char *id_hex, bool tracked) {
    if (!id_hex) return;
    hist_lock(__func__);
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
        // Zwolnij slot RAM (2,9 KB + ewentualne krzywe 2 x 2,3 KB) - inaczej wisialby do
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
    hist_unlock();
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

static void hist_wd_task(void *arg) {
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HIST_WD_PERIOD_MS));
        if (!s_mutex) continue;
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(HIST_WD_WAIT_MS)) == pdTRUE) {
            xSemaphoreGive(s_mutex);
            continue;
        }
        const char *who = s_lock_owner;
        uint32_t held = (uint32_t)((xTaskGetTickCount() - s_lock_since) * portTICK_PERIOD_MS / 1000);
        snprintf(s_wd_owner, sizeof(s_wd_owner), "%s", who ? who : "?");
        s_wd_held_s = held;
        s_wd_magic = HIST_WD_MAGIC;
        ESP_LOGE(TAG, "Blokada historii trzymana przez %s od %u s - restart modulu",
                 s_wd_owner, (unsigned)held);
        vTaskDelay(pdMS_TO_TICKS(200));   // log zdazy wyjsc na UART
        esp_restart();
    }
}

void history_init(void) {
    s_mutex = xSemaphoreCreateMutex();
    if (s_wd_magic == HIST_WD_MAGIC) {
        s_wd_owner[sizeof(s_wd_owner) - 1] = 0;
        ESP_LOGE(TAG, "Poprzedni restart: straznik historii - blokada trzymana przez %s od %u s",
                 s_wd_owner, (unsigned)s_wd_held_s);
    }
    s_wd_magic = 0;
    // Priorytet 10: powyzej wmbus_work (6) i HTTP - straznik musi dostac CPU
    // takze wtedy, gdy zadanie z blokada krazy w petli.
    xTaskCreate(hist_wd_task, "hist_wd", 3072, NULL, 10, NULL);
    // beta443: LittleFS zamiast SPIFFS. Pierwszy start po aktualizacji przenosi
    // historie (hist_migrate.c) - kilkanascie sekund do minuty.
    if (!hist_fs_mount()) {
        ESP_LOGE(TAG, "Pamiec historii niedostepna - historia wylaczona");
        s_fs_ok = false;
        return;
    }
    hs_root = "/spiffs";
    hs_free_fn = hist_fs_free;
    s_fs_ok = true;
    {
        nvs_handle_t nh;
        if (nvs_open("sih_fs", NVS_READONLY, &nh) == ESP_OK) {
            uint8_t dg = 0;
            if (nvs_get_u8(nh, "degraded", &dg) == ESP_OK && dg) s_fs_degraded = true;
            nvs_close(nh);
        }
        if (s_fs_degraded)
            ESP_LOGW(TAG, "Tryb awaryjny systemu plikow (z poprzedniej pracy)");
    }
    // FAZA 3a hotfix: przed jakimkolwiek load_meter posprzataj sierotki .tmp.
    hist_recover_stale_tmp();
    remove("/spiffs/imp.part");      // niedokonczone wgrywanie pliku z kopii
    {   // znacznik po przywroceniu kopii - kasujemy od razu, dziala jednorazowo
        FILE *rf = fopen(REBASE_FLAG, "rb");
        if (rf) { fclose(rf); remove(REBASE_FLAG); s_rebase_pending = true;
            ESP_LOGW(TAG, "Po przywroceniu kopii: biezaca doba zostanie zaczeta od nowa"); }
    }
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
    esp_littlefs_info("littlefs", &total, &used);
    ESP_LOGI(TAG, "LittleFS zamontowany: %u/%u B", (unsigned)used, (unsigned)total);
    hs_prune(false);                 // za malo miejsca? najpierw stare krzywe
    tracked_load();
    s_tracked_ready = true;
    ESP_LOGI(TAG, "Sledzonych licznikow: %d", s_tracked_count);

    // Wczytaj od razu z flash wszystkie sledzone liczniki (eager load), zeby
    // dane historyczne i ostatni stan byly dostepne natychmiast po restarcie -
    // bez czekania na pierwsza ramke.
    int loaded = 0;
    for (int i = 0; i < s_tracked_count; i++) {
        meter_hist_t *m = get_or_load(s_tracked[i], false);
        if (m) {
            loaded++;
            archive_rebuild_from_hours(m);  // uzupelnij archiwum z godzin w RAM
            curve_archive_seed(m);
            // Diagnostyka retencji: zasieg archiwum w logu przy kazdym starcie.
            char safe[SAFE_KEY_MAX + 1];
            safe_key(m->id, safe);
            hist_bucket_t r0, r1;
            int months = 0;
            int cnt = hs_arc_count(safe, &months);
            if (hs_arc_first(safe, &r0) && hs_arc_last(safe, &r1))
                ESP_LOGI(TAG, "Archiwum %s: %d godz w %d mies., od ts=%u do ts=%u",
                         m->id, cnt, months, (unsigned)r0.ts, (unsigned)r1.ts);
        }
    }
    s_arc_last_ok = (uint32_t)time(NULL);
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
        curve_max_seed_today(slot);   // beta370: obwiednia biezacej doby z hm_
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
// beta440: archiwum godzinowe co 5 min (w trybie awaryjnym co 15 min) zamiast
// co minute. Przy zmianie godziny najpierw domykamy poprzednia godzine jej
// ostatnim stanem z RAM, zeby rekord godziny nie byl o te minuty nieaktualny.
#define HIST_SAVE_S      3600
#define ARC_WRITE_S      300
#define ARC_WRITE_SLOW_S 900
static void arc_maybe_write(meter_hist_t *m, uint32_t bh, float ft, uint32_t ts_unix) {
    bool new_hour = floor_hour(ts_unix) != floor_hour(m->last_arc_ts);
    uint32_t every = s_fs_degraded ? ARC_WRITE_SLOW_S : ARC_WRITE_S;
    if (!new_hour && ts_unix - m->last_arc_ts < every) return;
    int64_t t0 = esp_timer_get_time();
    bool ok = true;   // beta442: dotad zawsze true - nieudany zapis liczyl sie jak udany
    if (new_hour && m->last_arc_ts && m->n_hours >= 2) {
        const hist_bucket_t *pv = &m->hours[m->n_hours - 2];
        if (pv->ts == floor_hour(m->last_arc_ts) &&
            !archive_append_hour(m->id, pv->ts, pv->total, ts_unix)) ok = false;
    }
    if (!archive_append_hour(m->id, bh, ft, ts_unix)) ok = false;
    m->last_arc_ts = ts_unix;
    fs_note_write(t0, ok, m->id);
}

void history_on_reading(const char *id_hex, double total, int kind, uint32_t ts_unix) {
    if (!id_hex || ts_unix < 1700000000) return;  // wymaga zsynchronizowanego czasu
    // Historie zbieramy WYLACZNIE dla sledzonych licznikow. Zaslyszani sasiedzi
    // (kazdy nadajnik w zasiegu) zajmowali wczesniej slot 2.9KB na stercie i plik
    // na flashu, mimo ze nikt ich nie ogladal. Podglad ramek ("Lap licznik") i
    // dekodowanie dzialaja niezaleznie - to rezygnacja wylacznie z historii.
    if (!history_is_tracked(id_hex)) return;
    hist_lock(__func__);

    meter_hist_t *m = get_or_load(id_hex, true);
    if (!m) { hist_unlock(); return; }

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
    arc_maybe_write(m, bh, ft, ts_unix);

    // Zapis: przy zamknieciu godziny LUB co >=15 min (krzywa dnia + biezaca
    // godzina nie przepadaja przy utracie zasilania; kontrolowany restart
    // dodatkowo robi pelny history_flush()).
    // beta440: plik stanu raz na godzine, nie co 15 min i nie przy kazdej pelnej
    // godzinie. Stan po restarcie odtwarza sie z archiwum (beta434), a kazdy
    // zapis h_ to nowy plik na SPIFFS - najwiekszy powod jego zapychania sie.
    // Po starcie pierwszy zapis dopiero po godzinie. Czas ustawiamy PRZED
    // zapisem, zeby nieudany zapis nie byl powtarzany przy kazdej ramce.
    (void)hour_closed;
    if (!m->last_save_ts) m->last_save_ts = ts_unix;
    if (ts_unix - m->last_save_ts >= HIST_SAVE_S) {
        m->last_save_ts = ts_unix;
        save_meter(m);
    }

    hist_unlock();
}

// Per-pole: klucz "id:pole". Zbiera TYLKO gdy pole sledzone (Etap A).
void history_on_field(const char *id_hex, const char *field, double value,
                      int kind, int cumulative, uint32_t ts_unix) {
    if (!id_hex || !field || ts_unix < 1700000000) return;
    char key[40];
    snprintf(key, sizeof(key), "%s:%s", id_hex, field);

    // ETAP A: zbieramy tylko sledzone pola (sasiedzi i niewybrane ignorowane)
    if (!history_is_tracked(key)) return;

    hist_lock(__func__);
    meter_hist_t *m = get_or_load(key, true);
    if (!m) { hist_unlock(); return; }

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
    arc_maybe_write(m, bh, ft, ts_unix);

    // beta440: plik stanu raz na godzine, nie co 15 min i nie przy kazdej pelnej
    // godzinie. Stan po restarcie odtwarza sie z archiwum (beta434), a kazdy
    // zapis h_ to nowy plik na SPIFFS - najwiekszy powod jego zapychania sie.
    // Po starcie pierwszy zapis dopiero po godzinie. Czas ustawiamy PRZED
    // zapisem, zeby nieudany zapis nie byl powtarzany przy kazdej ramce.
    (void)hour_closed;
    if (!m->last_save_ts) m->last_save_ts = ts_unix;
    if (ts_unix - m->last_save_ts >= HIST_SAVE_S) {
        m->last_save_ts = ts_unix;
        save_meter(m);
    }
    hist_unlock();
}

// ---------- JSON historii (zuzycie = roznice) ----------
// Najwczesniejszy znany stan licznika w danym okresie (lub tuz przed nim) - baza
// do policzenia przyrostu pierwszego/biezacego okresu, ktory nie ma poprzednika.
// Szuka w buforach od najdrobniejszego (godziny->dni->miesiace) pierwszego punktu
// o ts w [period_start, period_end). Zwraca true gdy znaleziono.
// beta419: gorna granica okresu. Dotad brane byly tez kubelki PO jego koncu -
// licznik, ktory pozniej sie cofnal (np. woda 244,241 -> 244,205 m3), dawal
// za niska baze i zawyzony pierwszy okres (36 l zamiast 6 l).
static bool earliest_total_in(meter_hist_t *m, uint32_t period_start,
                              uint32_t period_end, float cur_total, float *out) {
    // 1. Najmniejszy (najwczesniejszy) stan licznika w okresie [period_start, period_end).
    float best = 1e30f; bool found = false;
    hist_bucket_t *series[4] = { m->hours, m->days, m->months, m->years };
    int counts[4] = { m->n_hours, m->n_days, m->n_months, m->n_years };
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < counts[s]; i++) {
            if (series[s][i].ts >= period_start && series[s][i].ts < period_end &&
                series[s][i].total < best) {
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
                if (series[s][i].ts < period_end && series[s][i].total < gmin)
                    gmin = series[s][i].total;
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

// beta443: odczyt archiwum godzinowego od rekordu tuz przed from (baza
// roznic) do lim. Iterator otwiera kolejne pliki miesiecy po kolei.
static bool arc_iter_open(meter_hist_t *m, hs_iter_t *it, uint32_t from, uint32_t lim) {
    if (!s_fs_ok || from >= lim) return false;
    char safe[SAFE_KEY_MAX + 1];
    safe_key(m->id, safe);
    hs_iter_open(it, safe, from, lim);
    return true;
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

// beta395: pola chwilowe w widoku tygodnia/miesiaca/roku. Srednia z
// kubelkow godzinowych okresu jak dotad, ale godziny sprzed okna RAM (7 dni)
// czytamy z archiwum ha_ - wczesniej starsze dni byly po prostu puste.
#define RANGE_MAX_BUCKETS  400
#define RANGE_ARC_MAX_RECS 9000    // ok. rok godzin - gorna granica czasu odczytu

// Indeks okresu zawierajacego t (bts ma nb+1 granic) albo -1.
static int range_bucket_of(const uint32_t *bts, int nb, uint32_t t) {
    if (nb <= 0 || t < bts[0] || t >= bts[nb]) return -1;
    int lo = 0, hi = nb - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (bts[mid] <= t) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static int range_instant_points(meter_hist_t *m, uint32_t from, uint32_t to, char unit,
                                uint32_t now, char *buf, int buf_cap, int n) {
    uint32_t *bts = malloc((RANGE_MAX_BUCKETS + 1) * sizeof(uint32_t));
    float *sum = calloc(RANGE_MAX_BUCKETS, sizeof(float));
    int *cnt = calloc(RANGE_MAX_BUCKETS, sizeof(int));
    if (!bts || !sum || !cnt) { free(bts); free(sum); free(cnt); return n; }
    int nb = 0;
    for (uint32_t b = from; b < to && b <= now && nb < RANGE_MAX_BUCKETS; ) {
        bts[nb++] = b;
        b = next_bucket(b, unit);
        bts[nb] = b;                             // koniec ostatniego okresu
    }
    if (nb > 0) {
        // 1. godziny z RAM (ostatnie 7 dni) - dokladnie jak dotad
        uint32_t ram_start = (m->n_hours > 0) ? m->hours[0].ts : 0xFFFFFFFFu;
        for (int i = 0; i < m->n_hours; i++) {
            int k = range_bucket_of(bts, nb, m->hours[i].ts);
            if (k >= 0) { sum[k] += m->hours[i].total; cnt[k]++; }
        }
        // 2. godziny sprzed okna RAM - z archiwum (tylko odczyt)
        uint32_t lim = (ram_start < bts[nb]) ? ram_start : bts[nb];
        hs_iter_t it;
        if (arc_iter_open(m, &it, bts[0], lim)) {
            hist_bucket_t chunk[64];
            int r, done = 0;
            bool stop = false;
            while (!stop && done < RANGE_ARC_MAX_RECS && (r = hs_iter_next(&it, chunk, 64)) > 0) {
                for (int j = 0; j < r; j++) {
                    if (chunk[j].ts >= lim) { stop = true; break; }
                    int k = range_bucket_of(bts, nb, chunk[j].ts);
                    if (k >= 0) { sum[k] += chunk[j].total; cnt[k]++; }
                }
                done += r;
            }
            hs_iter_close(&it);
        }
        bool first = true;
        for (int k = 0; k < nb && n < buf_cap - 64; k++) {
            if (!cnt[k]) continue;
            n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                          first ? "" : ",", (unsigned)bts[k], sum[k] / cnt[k]);
            first = false;
        }
    }
    free(bts); free(sum); free(cnt);
    return n;
}

// beta419: pola kumulacyjne w widoku tygodnia/miesiaca/roku (i na wykresie
// 7 dni na dashboardzie) liczone TAK SAMO jak slupki doby w day_json: suma
// przyrostow kolejnych kubelkow godzinowych, ujemny przyrost jako zero,
// pierwszy kubelek bez poprzednika jako zero (albo od punktu odniesienia
// doby). Dotad okres byl roznica stanu licznika na jego granicach i dawal
// inne liczby niz widok doby:
//  - kubelek 00:00 trzyma stan z konca tej godziny, wiec doba liczyla sie
//    od 01:00 do 01:00 (prad: 13,87 zamiast 13,67 kWh),
//  - licznik, ktory sie cofnal, zerowal caly okres albo go zawyzal,
//  - pierwsza doba danych brala baze sprzed jej konca (36 l zamiast 6 l).
// Czytanie jak w range_instant_points: archiwum ha_ do poczatku okna RAM,
// dalej godziny z RAM.
typedef struct {
    hist_bucket_t ref, cur;          // ostatni zamkniety kubelek i biezacy
    bool has_ref, has_cur;
} range_cum_t;

static void range_cum_close(meter_hist_t *m, range_cum_t *st, const uint32_t *bts,
                            int nb, double *sum, int *cnt) {
    if (!st->has_cur) return;
    hist_bucket_t *b = &st->cur;
    int k = range_bucket_of(bts, nb, b->ts);
    if (k >= 0) {
        float d = 0;
        if (st->has_ref) d = b->total - st->ref.total;
        else if (m->day_base_ts != 0 && m->day_base_ts == floor_day(b->ts))
            d = b->total - m->day_base_total;   // jak pierwszy slupek w day_json
        if (d < 0) d = 0;
        // Zaokraglenie kazdej godziny do 0,001 jak w JSON doby - suma okresu
        // ma byc co do litra/Wh rowna sumie slupkow widoku doby.
        sum[k] += round((double)d * 1000.0) / 1000.0; cnt[k]++;
    }
    st->ref = *b; st->has_ref = true;
    st->has_cur = false;
}

// Ta sama godzina tylko raz (pozniejszy rekord wygrywa), cofniety ts pomijamy.
static void range_cum_feed(meter_hist_t *m, range_cum_t *st, const hist_bucket_t *r,
                           const uint32_t *bts, int nb, double *sum, int *cnt) {
    if (st->has_cur && r->ts == st->cur.ts) { st->cur = *r; return; }
    if (st->has_cur && r->ts < st->cur.ts) return;
    if (!st->has_cur && st->has_ref && r->ts <= st->ref.ts) return;
    range_cum_close(m, st, bts, nb, sum, cnt);
    st->cur = *r; st->has_cur = true;
}

static int range_cumulative_points(meter_hist_t *m, uint32_t from, uint32_t to, char unit,
                                   uint32_t now, char *buf, int buf_cap, int n) {
    uint32_t *bts = malloc((RANGE_MAX_BUCKETS + 1) * sizeof(uint32_t));
    double *sum = calloc(RANGE_MAX_BUCKETS, sizeof(double));
    int *cnt = calloc(RANGE_MAX_BUCKETS, sizeof(int));
    if (!bts || !sum || !cnt) { free(bts); free(sum); free(cnt); return n; }
    int nb = 0;
    for (uint32_t b = from; b < to && b <= now && nb < RANGE_MAX_BUCKETS; ) {
        bts[nb++] = b;
        b = next_bucket(b, unit);
        bts[nb] = b;
    }
    if (nb > 0) {
        range_cum_t st;
        memset(&st, 0, sizeof(st));
        uint32_t ram_start = (m->n_hours > 0) ? m->hours[0].ts : 0xFFFFFFFFu;
        // 1. archiwum: od rekordu tuz przed poczatkiem zakresu (baza pierwszej
        //    godziny, jak "prev" w day_json) do poczatku okna RAM
        uint32_t lim = (ram_start < bts[nb]) ? ram_start : bts[nb];
        hs_iter_t it;
        if (arc_iter_open(m, &it, bts[0], lim)) {
            hist_bucket_t chunk[64];
            int r, done = 0;
            bool stop = false;
            while (!stop && done < RANGE_ARC_MAX_RECS && (r = hs_iter_next(&it, chunk, 64)) > 0) {
                for (int j = 0; j < r; j++) {
                    if (chunk[j].ts >= lim) { stop = true; break; }
                    range_cum_feed(m, &st, &chunk[j], bts, nb, sum, cnt);
                }
                done += r;
            }
            hs_iter_close(&it);
        }
        // 2. godziny z RAM (ostatnie 7 dni) - swiezsze od archiwum
        for (int i = 0; i < m->n_hours; i++) {
            if (m->hours[i].ts >= bts[nb]) break;
            range_cum_feed(m, &st, &m->hours[i], bts, nb, sum, cnt);
        }
        range_cum_close(m, &st, bts, nb, sum, cnt);
        bool first = true;
        for (int k = 0; k < nb && n < buf_cap - 64; k++) {
            if (!cnt[k]) continue;
            n += snprintf(buf + n, buf_cap - n, "%s{\"t\":%u,\"v\":%.3f}",
                          first ? "" : ",", (unsigned)bts[k], sum[k]);
            first = false;
        }
    }
    free(bts); free(sum); free(cnt);
    return n;
}

int history_range_json(const char *id_hex, uint32_t from, uint32_t to,
                       char unit, char *buf, int buf_cap) {
    hist_lock(__func__);
    meter_hist_t *m = get_or_load(id_hex, false);
    if (!m) {
        int n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":0,\"points\":[]}",
                         id_hex ? id_hex : "");
        hist_unlock();
        return n;
    }
    int n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"points\":[",
                     m->id, m->kind, m->cumulative);
    uint32_t now = (uint32_t)time(NULL);
    if (!m->cumulative) {
        n = range_instant_points(m, from, to, unit, now, buf, buf_cap, n);
    } else {
        n = range_cumulative_points(m, from, to, unit, now, buf, buf_cap, n);
    }
    n += snprintf(buf + n, buf_cap - n, "]}");
    hist_unlock();
    return n;
}

int history_get_json(const char *id_hex, const char *res, char *buf, int buf_cap) {
    hist_lock(__func__);
    meter_hist_t *m = get_or_load(id_hex, false);
    int n = 0;
    if (!m) {
        n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":0,\"points\":[]}", id_hex ? id_hex : "");
        hist_unlock();
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
                    uint32_t pend = (i + 1 < cnt) ? arr[i + 1].ts : 0xFFFFFFFFu;
                    if (arr == m->hours) pend = pstart + 3600;
                    else if (arr == m->days) pend = next_bucket(pstart, 'd');
                    else if (arr == m->months) pend = next_bucket(pstart, 'm');
                    if (earliest_total_in(m, pstart, pend, arr[i].total, &base) && arr[i].total >= base) {
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
    hist_unlock();
    return n;
}

// Zwraca godzinowe zuzycie z konkretnego dnia z archiwum miesiecznego (flash).
// Czyta plik archiwum, filtruje punkty z wybranej doby, liczy zuzycie jako roznice
// kolejnych totali (dla kumulacyjnych) lub wartosc (dla chwilowych).
int history_debug_day(const char *key, uint32_t day_ts, char *buf, int cap) {
    hist_lock(__func__);
    meter_hist_t *m = get_or_load(key, false);
    uint32_t d0 = floor_day(day_ts), d1 = day_end(d0);
    int n = snprintf(buf, cap, "{\"key\":\"%s\",\"d0\":%u,\"d1\":%u", key ? key : "", (unsigned)d0, (unsigned)d1);
    if (!m) {
        n += snprintf(buf + n, cap - n, ",\"slot\":\"BRAK - licznik nie zaladowany\"}");
        hist_unlock();
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
    if (!s_fs_ok) {
        n += snprintf(buf + n, cap - n, ",\"arch\":\"BRAK PAMIECI HISTORII\"}");
        hist_unlock();
        return n;
    }
    char safe[SAFE_KEY_MAX + 1];
    safe_key(key, safe);
    int months = 0;
    int recs = hs_arc_count(safe, &months);
    n += snprintf(buf + n, cap - n, ",\"arc_klucz\":\"%s\",\"arc_rekordow\":%d,\"arc_miesiecy\":%d",
                  safe, recs, months);
    hs_iter_t it;
    hs_iter_open(&it, safe, d0, d1);
    hist_bucket_t chunk[32], prev = { 0 };
    bool has_prev = false, stop = false;
    int cnt = 0, r;
    n += snprintf(buf + n, cap - n, ",\"arc_w_dobie\":[");
    while (!stop && (r = hs_iter_next(&it, chunk, 32)) > 0) {
        for (int j = 0; j < r; j++) {
            if (chunk[j].ts >= d1) { stop = true; break; }
            if (chunk[j].ts < d0) { prev = chunk[j]; has_prev = true; continue; }
            if (cnt >= 26 || n >= cap - 80) { stop = true; break; }
            n += snprintf(buf + n, cap - n, "%s{\"ts\":%u,\"v\":%.3f}", cnt ? "," : "",
                          (unsigned)chunk[j].ts, (double)chunk[j].total);
            cnt++;
        }
    }
    hs_iter_close(&it);
    n += snprintf(buf + n, cap - n, "],\"arc_znalezionych\":%d", cnt);
    if (has_prev)
        n += snprintf(buf + n, cap - n, ",\"prev_rekord\":{\"ts\":%u,\"v\":%.3f}",
                      (unsigned)prev.ts, (double)prev.total);
    else
        n += snprintf(buf + n, cap - n, ",\"prev_rekord\":null");
    n += snprintf(buf + n, cap - n, "}");
    hist_unlock();
    return n;
}

// beta395: try_curve_arc=false pomija probe archiwum 5-min. API wola te
// wersje po history_curve_day(), ktore to archiwum juz przeszukalo - dla
// doby spoza niego (np. starszej niz 90 dni) plik byl przeszukiwany drugi raz.
static int day_json(const char *id_hex, uint32_t day_ts, char *buf, int buf_cap,
                    bool try_curve_arc);

int history_get_day_json(const char *id_hex, uint32_t day_ts, char *buf, int buf_cap) {
    return day_json(id_hex, day_ts, buf, buf_cap, true);
}

int history_get_day_json_hours(const char *id_hex, uint32_t day_ts, char *buf, int buf_cap) {
    return day_json(id_hex, day_ts, buf, buf_cap, false);
}

static int day_json(const char *id_hex, uint32_t day_ts, char *buf, int buf_cap,
                    bool try_curve_arc) {
    hist_lock(__func__);
    // Pobierz metadane (kind, cumulative) z RAM jesli licznik zaladowany.
    meter_hist_t *m = get_or_load(id_hex, false);
    int kind = m ? m->kind : 0;
    int cumulative = m ? m->cumulative : 1;

    uint32_t d0 = floor_day(day_ts);          // poczatek wybranej doby
    uint32_t d1 = day_end(d0);                 // poczatek nastepnej doby (23-25 h)

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
    // beta370: zrodlo obwiedni maksimow. Z RAM idzie tablica rownolegla do
    // curve_src (te same indeksy); z flasha - osobna lista z wlasnymi ts,
    // scalana po znaczniku czasu.
    const float *curve_src_max = NULL;
    int max_flash_n = 0;
    if (m && !cumulative && m->curve && m->n_curve > 0) {
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
                curve_src_max = m->curve_max;     // beta370
            }
        }
    }
    // FAZA 9b: brak krzywej w RAM, ale pole jest chwilowe - sprobuj archiwum
    // 5-min na flashu, zanim spadniemy do slupkow godzinowych.
    static hist_bucket_t s_curve_day_buf[HIST_CURVE];
    if (!use_curve && m && !cumulative && try_curve_arc) {
        int na = curve_archive_read_day(id_hex, d0, d1, s_curve_day_buf, HIST_CURVE, false);
        if (na > 0) {
            curve_src = s_curve_day_buf; curve_src_n = na; use_curve = true;
            // beta370: maksima dla tej samej doby, osobny plik.
            max_flash_n = curve_archive_read_day(id_hex, d0, d1,
                                                 s_curve_max_buf, HIST_CURVE, true);
        }
    }

    if (use_curve && curve_src) {
        int n2 = snprintf(buf, buf_cap,
                          "{\"id\":\"%s\",\"kind\":%d,\"cumulative\":0,\"curve\":1,\"points\":[",
                          id_hex ? id_hex : "", kind);
        bool first2 = true;
        int jmx = 0;   // beta370: kursor po liscie maksimow z flasha
        for (int i = 0; i < curve_src_n && n2 < buf_cap - 72; i++) {
            uint32_t t = curve_src[i].ts;
            if (t < d0 || t >= d1) continue;
            n2 += snprintf(buf + n2, buf_cap - n2, "%s{\"t\":%u,\"v\":%.3f",
                           first2 ? "" : ",", (unsigned)t, curve_src[i].total);
            // beta370: maksimum kubelka jako pole "m". Umowa: maksimum okna nie
            // moze byc MNIEJSZE od jego sredniej, wiec wartosc mniejsza znaczy
            // "nie mam tych danych" - kubelek sprzed aktualizacji albo pole
            // spoza curve_max_wanted(). Wtedy pola po prostu nie wypisujemy
            // i panel rysuje sama linie, dokladnie jak przed ta wersja.
            float mv = 0.0f;
            if (curve_src_max) {
                mv = curve_src_max[i];
            } else if (max_flash_n > 0) {
                while (jmx < max_flash_n && s_curve_max_buf[jmx].ts < t) jmx++;
                if (jmx < max_flash_n && s_curve_max_buf[jmx].ts == t)
                    mv = s_curve_max_buf[jmx].total;
            }
            if (mv >= curve_src[i].total)
                n2 += snprintf(buf + n2, buf_cap - n2, ",\"m\":%.3f", mv);
            n2 += snprintf(buf + n2, buf_cap - n2, "}");
            first2 = false;
        }
        n2 += snprintf(buf + n2, buf_cap - n2, "]}");
        hist_unlock();
        return n2;
    }

    int n = snprintf(buf, buf_cap, "{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"points\":[",
                     id_hex ? id_hex : "", kind, cumulative);

    // Zbierz godziny wybranej doby (+1 poprzednia dla roznicy) do malego bufora.
    // ZRODLO 1: bufor hours[] w RAM (ostatnie 7 dni) - niezawodne, te same dane co
    // res=hour. Pokrywa Dzis/Wczoraj/caly tydzien.
    // ZRODLO 2 (fallback): archiwum ha_ na flash - dla dni starszych niz 7 dni.
    int nb = 0;
    int dup = 0;                 // beta350: ile godzin odsiano jako powtorzone
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
        // beta443: plik miesiaca doby (i rekord sprzed niej z poprzedniego
        // miesiaca, gdy doba jest pierwsza w miesiacu).
        if (s_fs_ok) {
            char safe[SAFE_KEY_MAX + 1];
            safe_key(id_hex, safe);
            hs_iter_t it;
            hs_iter_open(&it, safe, d0, d1);
            hist_bucket_t chunk[32];
            bool stop = false;
            int r;
            while (!stop && (r = hs_iter_next(&it, chunk, 32)) > 0) {
                for (int j = 0; j < r; j++) {
                    hist_bucket_t rec = chunk[j];
                    if (rec.ts >= d1) { stop = true; break; }
                    if (rec.ts < d0) { prev = rec; has_prev = true; continue; }
                    if (nb >= ARC_DAY_BUF) { stop = true; break; }
                    if (nb > 0 && rec.ts == s_day_buf[nb - 1].ts) {
                        s_day_buf[nb - 1] = rec;   // aktualizacja tej samej godziny
                        dup++;
                    } else if (nb > 0 && rec.ts < s_day_buf[nb - 1].ts) {
                        dup++;                     // ts nierosnacy - pomijamy
                    } else {
                        s_day_buf[nb++] = rec;
                    }
                }
            }
            hs_iter_close(&it);
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
        for (int i = 0; i < m->n_hours && nb < ARC_DAY_BUF; i++) {
            uint32_t t = m->hours[i].ts;
            if (t < d0) {
                if (!has_prev || t > prev.ts) { prev = m->hours[i]; has_prev = true; }
                continue;
            }
            if (t >= d1) break;
            // beta351: sprostowanie do beta350. Straznik "t > newest" NIE byl
            // martwy - nb bywa tu niezerowe, bo archiwum wypelnilo juz bufor.
            // Co wazniejsze: godziny z RAM, ktore archiwum wlasnie dostarczylo,
            // to normalna praca tego bloku, a nie powtorzenia w danych. Zliczanie
            // ich do "dup" zawyzalo diagnostyke. Rekord z RAM jest swiezszy, wiec
            // przy rownym ts nadpisuje ten z archiwum.
            if (nb > 0 && t == s_day_buf[nb - 1].ts) {
                s_day_buf[nb - 1] = m->hours[i];
            } else if (nb > 0 && t < s_day_buf[nb - 1].ts) {
                /* ta godzina jest juz w buforze z archiwum */
            } else {
                s_day_buf[nb++] = m->hours[i];
            }
        }
    }

    // beta351: dlawik 60 s. Panel odpytuje o dobe co kilka sekund, wiec ten wpis
    // zalewal konsole. Pliki uszkodzone przed ta wersja nie naprawia sie same,
    // wiec komunikat bedzie sie jeszcze pojawial - ale juz nie przy kazdym
    // odswiezeniu.
    if (dup > 0) {
        static uint32_t s_dup_log_ts = 0;
        uint32_t now_s = (uint32_t)time(NULL);
        if (now_s - s_dup_log_ts >= 60) {
            s_dup_log_ts = now_s;
            ESP_LOGW(TAG, "%s: doba %u - w archiwum %d powtorzonych godzin (zostalo %d)",
                     id_hex ? id_hex : "?", (unsigned)d0, dup, nb);
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
    n += snprintf(buf + n, buf_cap - n, "]");
    // beta428: stan licznika na poczatku i koncu doby (np. odczyt rozliczeniowy
    // operatora o 24:00). Nic nowego nie zapisujemy - kubelek godzinowy trzyma
    // stan licznika z konca swojej godziny. sNt to poczatek godziny kubelka
    // (odczyt lezy w [sNt, sNt+3600)), dla biezacej doby s1t to czas ostatniej
    // ramki. Klucze celowo bez "v": history_day_sum sumuje kazde "v".
    if (cumulative && n < buf_cap - 100) {
        if (has_prev)
            n += snprintf(buf + n, buf_cap - n, ",\"s0\":%.3f,\"s0t\":%u",
                          prev.total, (unsigned)prev.ts);
        if (m && m->last_ts >= d0 && m->last_ts < d1)
            n += snprintf(buf + n, buf_cap - n, ",\"s1\":%.3f,\"s1t\":%u,\"s1now\":1",
                          m->last_total, (unsigned)m->last_ts);
        else if (nb > 0)
            n += snprintf(buf + n, buf_cap - n, ",\"s1\":%.3f,\"s1t\":%u",
                          s_day_buf[nb - 1].total, (unsigned)s_day_buf[nb - 1].ts);
    }
    n += snprintf(buf + n, buf_cap - n, "}");
    hist_unlock();
    return n;
}

int history_list_json(char *buf, int buf_cap) {
    hist_lock(__func__);
    int n = snprintf(buf, buf_cap, "[");
    bool first = true;
    for (int i = 0; i < MAX_HIST_METERS; i++) {
        if (!s_meters[i] || !s_meters[i]->used) continue;
        // Czas pierwszej ramki - liczony raz na licznik i pamietany w RAM.
        if (!s_meters[i]->first_ts) {
            uint32_t ft = arc_first_ts(s_meters[i]->id);
            if (!ft) {   // brak archiwum (swiezy licznik) - najstarszy kubelek z RAM
                if (s_meters[i]->n_days > 0)       ft = s_meters[i]->days[0].ts;
                else if (s_meters[i]->n_hours > 0) ft = s_meters[i]->hours[0].ts;
                else                               ft = s_meters[i]->last_ts;
            }
            s_meters[i]->first_ts = ft;
        }
        // snprintf zwraca dlugosc, ktora BY sie zapisala - bez clampowania n
        // moglby przekroczyc bufor, a buf_cap-n podwinaloby sie do ogromnego
        // size_t. Przy 24 kluczach wpis ma ~120 B, wiec bufor 1 kB byl za maly.
        if (n >= buf_cap - 2) break;
        int w = snprintf(buf + n, buf_cap - n,
                      "%s{\"id\":\"%s\",\"kind\":%d,\"cumulative\":%d,\"last\":%.3f,\"ts\":%u,\"first\":%u,\"tracked\":%s}",
                      first ? "" : ",", s_meters[i]->id, s_meters[i]->kind, s_meters[i]->cumulative,
                      s_meters[i]->last_total, (unsigned)s_meters[i]->last_ts,
                      (unsigned)s_meters[i]->first_ts,
                      history_is_tracked(s_meters[i]->id) ? "true" : "false");
        if (w < 0 || w >= buf_cap - n) break;   // nie miesci sie - urwij czysto
        n += w;
        first = false;
    }
    n += snprintf(buf + n, buf_cap - n, "]");
    hist_unlock();
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
    uint32_t d1 = day_end(d0);
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

    hist_lock(__func__);
    meter_hist_t *m = get_or_load(key, false);
    if (!m) {
        hist_unlock();
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
                if (earliest_total_in(m, day0, 0xFFFFFFFFu, m->last_total, &base) &&
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
    hist_unlock();

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
        // Poludnie poprzednich dob, a nie "teraz - 24 h": tuz po polnocy w dobie
        // 23-godzinnej (marzec) teraz - 24 h wypadalo jeszcze w przedwczoraj.
        uint32_t d0_now = floor_day(now_ts);
        uint32_t d1_ago = floor_day(d0_now - 1);
        if (history_day_sum(key, d1_ago + 43200, &v)) {
            out->yesterday = v; out->has_yesterday = true;
        } else {
            out->has_yesterday = false;
        }
        if (history_day_sum(key, floor_day(d1_ago - 1) + 43200, &v)) {
            out->day_before = v; out->has_day_before = true;
        } else {
            out->has_day_before = false;
        }
    }
    return true;
}

int history_tracked_meter_ids(char ids[][12], int max_ids) {
    hist_lock(__func__);
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
    hist_unlock();
    return count;
}

int history_keys_for_id(const char *id, char keys[][40], int max_keys) {
    if (!id) return 0;
    hist_lock(__func__);
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
    hist_unlock();
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
    if (esp_littlefs_info("littlefs", &t, &u) != ESP_OK) return false;
    if (used) *used = u;
    if (total) *total = t;
    return true;
}

uint32_t history_write_stall_s(void) {
    if (!s_fs_ok || !s_arc_fail_since) return 0;
    uint32_t now = (uint32_t)time(NULL);
    return now > s_arc_fail_since ? now - s_arc_fail_since : 1;
}

// ---------- beta443: kopia zapasowa ----------
#define IMP_PART "/spiffs/imp.part"

static void export_dir(const char *rel_dir, history_file_fn fn, void *ctx) {
    char dpath[64];
    snprintf(dpath, sizeof(dpath), "/spiffs/%s", rel_dir);
    DIR *top = opendir(dpath);
    if (!top) return;
    struct dirent *k;
    while ((k = readdir(top)) != NULL) {
        if (k->d_name[0] == '.' || strlen(k->d_name) > HS_SAFE_MAX) continue;
        char kpath[112];
        snprintf(kpath, sizeof(kpath), "%s/%s", dpath, k->d_name);
        DIR *sub = opendir(kpath);
        if (!sub) continue;
        struct dirent *e;
        while ((e = readdir(sub)) != NULL) {
            if (e->d_name[0] == '.' || strlen(e->d_name) > 16) continue;
            char rel[80], path[136];
            snprintf(rel, sizeof(rel), "%s/%s/%s", rel_dir, k->d_name, e->d_name);
            snprintf(path, sizeof(path), "%s/%s", kpath, e->d_name);
            if (hs_rel_ok(rel)) fn(ctx, rel, path);
        }
        closedir(sub);
    }
    closedir(top);
}

void history_export_each(int pass, history_file_fn fn, void *ctx) {
    if (!s_fs_ok || !fn) return;
    if (pass == 0) {
        DIR *d = opendir("/spiffs");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                const char *nm = de->d_name;
                if (strlen(nm) > 60 || !hs_rel_ok(nm)) continue;   // h_*.bin, tracked.txt
                char path[80];
                snprintf(path, sizeof(path), "/spiffs/%s", nm);
                fn(ctx, nm, path);
            }
            closedir(d);
        }
        export_dir("a", fn, ctx);
    } else {
        export_dir("c", fn, ctx);
        export_dir("m", fn, ctx);
    }
}

// Stare nazwy (kopie do beta442): ha_/hc_/hm_<klucz>.bin - przeliczane na nowy uklad.
static bool old_flat_key(const char *nm, char *key) {
    size_t n = strlen(nm);
    if (n < 8 || strcmp(nm + n - 4, ".bin") != 0) return false;
    if (strncmp(nm, "ha_", 3) && strncmp(nm, "hc_", 3) && strncmp(nm, "hm_", 3)) return false;
    size_t kl = n - 7;
    if (kl == 0 || kl > HS_SAFE_MAX) return false;
    memcpy(key, nm + 3, kl);
    key[kl] = 0;
    for (size_t i = 0; i < kl; i++) {
        char c = key[i];
        if (!(c == '_' || c == '-' || (c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    }
    return true;
}

bool history_import_name_ok(const char *name) {
    char key[HS_SAFE_MAX + 1];
    return name && (hs_rel_ok(name) || old_flat_key(name, key));
}

FILE *history_import_open(void) {
    return fopen(IMP_PART, "wb");
}

void history_import_abort(void) {
    remove(IMP_PART);
}

bool history_import_commit(const char *name) {
    char key[HS_SAFE_MAX + 1];
    bool ok;
    if (old_flat_key(name, key)) {
        int recs = 0;
        if (name[1] == 'a') ok = hs_import_old_arc(IMP_PART, key, &recs);
        else ok = hs_import_old_crv(IMP_PART, key, name[1] == 'm', &recs);
        if (ok) ESP_LOGI(TAG, "Kopia: %s przeliczony (%d rekordow)", name, recs);
        remove(IMP_PART);
        return ok;
    }
    if (!hs_rel_ok(name)) { remove(IMP_PART); return false; }
    char path[96];
    snprintf(path, sizeof(path), "/spiffs/%s", name);
    ok = hs_mkdirs(path) && rename(IMP_PART, path) == 0;
    if (!ok) remove(IMP_PART);
    return ok;
}

void history_flush(void) {
    hist_lock(__func__);
    for (int i = 0; i < MAX_HIST_METERS; i++) {
        if (s_meters[i] && s_meters[i]->used) save_meter(s_meters[i]);
    }
    hist_unlock();
    ESP_LOGI(TAG, "Historia zrzucona na flash (flush)");
}

int history_curve_day(const char *key, uint32_t day_ts, hist_bucket_t *out,
                      float *out_max, int cap) {
    if (!key || !out || cap <= 0) return 0;
    int n = 0;
    hist_lock(__func__);
    meter_hist_t *m = get_or_load(key, false);
    if (m && !m->cumulative) {
        uint32_t d0 = floor_day(day_ts), d1 = day_end(d0);

        // Krzywa dnia biezacego (gdy szukana doba to "dzisiaj" wg last_ts
        // albo ring w calosci ja pokrywa). Wczoraj i starsze - z hc_/hm_ nizej.
        if (m->curve && m->n_curve > 0) {
            bool is_current_day = (m->last_ts >= d0 && m->last_ts < d1);
            bool fully_covered  = (m->curve[0].ts <= d0);
            if (is_current_day || fully_covered) {
                for (int i = 0; i < m->n_curve && n < cap; i++) {
                    if (m->curve[i].ts < d0 || m->curve[i].ts >= d1) continue;
                    if (out_max) out_max[n] = m->curve_max ? m->curve_max[i] : 0.0f;
                    out[n++] = m->curve[i];
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
                // beta370: maksimum otwartego kubelka obejmuje takze te ostatnia
                // ramke, wiec po podmianie sredniej na last_total relacja
                // "maksimum >= wartosc" nadal sie trzyma.
                if (out_max) out_max[n-1] = m->curve_run_max;
            }
        }

        // FAZA 9b: dni STARSZE niz wczoraj - z archiwum 5-min na flashu
        // (hc_*.bin). Wczesniej w tym miejscu zwracalismy 0 punktow i API
        // schodzilo do slupkow godzinowych; dlatego 5-min widac bylo tylko
        // przez dwie doby trzymane w RAM.
        if (n == 0) {
            n = curve_archive_read_day(m->id, d0, d1, out, cap, false);
            // beta370: maksima tej samej doby leza w osobnym pliku i maja wlasne
            // znaczniki czasu - scalamy je z krzywa jednym przejsciem po obu
            // listach (obie rosnaco po ts).
            if (n > 0 && out_max) {
                for (int i = 0; i < n; i++) out_max[i] = 0.0f;
                int nm = curve_archive_read_day(m->id, d0, d1, s_curve_max_buf,
                                                HIST_CURVE, true);
                int j = 0;
                for (int i = 0; i < n && j < nm; i++) {
                    while (j < nm && s_curve_max_buf[j].ts < out[i].ts) j++;
                    if (j < nm && s_curve_max_buf[j].ts == out[i].ts)
                        out_max[i] = s_curve_max_buf[j++].total;
                }
            }
        }
    }
    hist_unlock();
    return n;
}
