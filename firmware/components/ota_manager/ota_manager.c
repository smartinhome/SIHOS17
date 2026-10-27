#include "esphome_api.h"
#include "ota_manager.h"
#include <stdint.h>
#include "cc1101.h"
#include "wmbus_decoder.h"
#include "display_eink.h"
#include <stdlib.h>
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "history.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include <limits.h>
#include <string.h>

static const char *TAG = "OTA";
static ota_status_t s_status = { .state = OTA_STATE_IDLE };

// Whitelist dozwolonych zrodel OTA. Bez tego /api/ota/url przyjmowal DOWOLNY
// URL - atakujacy w LAN mogl wskazac obraz z wlasnego serwera i przejac modul
// (SSRF + RCE). Kazde URL musi zaczynac sie od jednego z tych prefiksow:
// - https://github.com/                    (releases attachments, redirect)
// - https://objects.githubusercontent.com/  (rzeczywisty CDN GitHub po redirect)
// - https://api.github.com/                 (metadata dla ota_start_from_github)
// - https://www.smartinhome.pl/             (autorski serwer usera)
// Wymuszamy https:// - plain HTTP jest zaslepione (MITM = podmiana firmware).
static const char * const OTA_URL_WHITELIST[] = {
    "https://github.com/",
    "https://objects.githubusercontent.com/",
    "https://api.github.com/",
    "https://www.smartinhome.pl/",
    NULL
};

bool ota_url_allowed(const char *url) {
    if (!url || url[0] == 0) return false;
    for (int i = 0; OTA_URL_WHITELIST[i] != NULL; i++) {
        size_t plen = strlen(OTA_URL_WHITELIST[i]);
        if (strncmp(url, OTA_URL_WHITELIST[i], plen) == 0) return true;
    }
    return false;
}

// beta410: radio, e-ink i API ESPHome zatrzymujemy na czas pobierania. Po
// nieudanej probie trzeba je wznowic - wczesniej wracalo tylko API ESPHome,
// a modul do restartu nie odbieral licznikow (radio stalo). Flaga, bo
// cc1101_start_receive nie jest idempotentne (drugi task odbioru).
static bool s_svc_stopped;

static void ota_stop_services(void) {
    cc1101_stop();
    display_eink_pause();  // zwolnij SPI i RAM przed OTA
    esphome_api_stop();    // beta390: ok. 8 KB sterty na TLS do GitHuba
    s_svc_stopped = true;
}

static void ota_resume_services(void) {
    if (!s_svc_stopped) return;
    s_svc_stopped = false;
    cc1101_start_receive(wmbus_decoder_on_frame);
    display_eink_resume();
    esphome_api_start();
}

// beta410: kanal GitHub nie instaluje obrazu zbudowanego wczesniej niz
// zainstalowany. Numery wersji tego nie rozstrzygna - bety maja "1.0.0-betaN"
// takze po v1.0.4, wiec porownujemy date i godzine kompilacji z opisu obrazu.
static bool s_no_downgrade;

// Parametry biezacego zlecenia z panelu (jedno OTA naraz)
static bool s_gh_beta;
static char s_gh_tag[48];

// "Oct 27 2026" + "11:29:00" -> liczba rosnaca z czasem (0 = nie rozpoznano)
static int64_t ota_build_stamp(const char *date, const char *time) {
    static const char M[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int d = 0, y = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(date, "%3s %d %d", mon, &d, &y) != 3) return 0;
    const char *p = strstr(M, mon);
    if (strlen(mon) != 3 || !p || (p - M) % 3) return 0;
    sscanf(time, "%d:%d:%d", &hh, &mm, &ss);
    int mi = (int)(p - M) / 3 + 1;
    return ((((int64_t)y * 100 + mi) * 100 + d) * 100 + hh) * 10000 + mm * 100 + ss;
}

// ── Pobieranie obrazu z wznawianiem ──
// beta410: wlasna petla zamiast esp_https_ota. Z partial_http_download
// esp_https_ota co 256 KB laczyl sie od nowa pod adres PO przekierowaniu -
// podpisany link GitHuba z krotka waznoscia. Przy wolnym lub rwacym sie
// laczu kolejny zakres dostawal 403 albo zerwanie i cala aktualizacja konczyla
// sie "OTA perform: ESP_FAIL" w 30-50%. Teraz jedno ciagle pobieranie, a po
// zerwaniu ponowne polaczenie od oryginalnego URL (swiezy link) z naglowkiem
// Range od ostatniego zapisanego bajtu. Poprawnosc i tak sprawdza esp_ota_end
// (suma SHA-256 obrazu), wiec sklejony obraz nie ma szans przejsc z bledem.
#define OTA_DL_BUF       4096
#define OTA_HDR_NEED     ((int)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)))
#define OTA_MAX_NOPROG   6     // kolejne proby bez zadnego postepu
#define OTA_MAX_TRIES    30    // twardy limit polaczen na jedna aktualizacje
#define OTA_MAX_STALL    2     // ile razy z rzedu read moze zwrocic timeout

// Otworz URL (z przekierowaniami) od bajtu "from". Zwraca klienta po
// odebraniu naglowkow ostatecznej odpowiedzi; *status = kod HTTP albo -1.
static esp_http_client_handle_t ota_dl_open(const char *url, int64_t from,
                                            int *status, int64_t *clen) {
    *status = -1;
    *clen   = -1;
    esp_http_client_config_t cfg = {
        .url                         = url,
        .timeout_ms                  = 20000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        // skip_cert_common_name_check = false: wymuszamy walidacje CN certyfikatu
        // wzgledem hosta URL. Wczesniej true pozwalalo kazdemu certowi z bundle
        // CA na podszycie sie za dowolna domene - MITM przez uzyskanie ANY cert
        // od zaufanego CA (Let's Encrypt, DigiCert...) mogl podac wlasny firmware.
        .skip_cert_common_name_check = false,
        .buffer_size                 = 8192,
        .buffer_size_tx              = 4096,
        .user_agent                  = "SIH-wMbus-Reader",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (from > 0) {
        char rng[32];
        snprintf(rng, sizeof(rng), "bytes=%lld-", (long long)from);
        esp_http_client_set_header(c, "Range", rng);   // zostaje po przekierowaniu
    }
    for (int hop = 0; hop < 8; hop++) {
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "OTA: polaczenie nieudane: %s", esp_err_to_name(e));
            break;
        }
        int64_t cl = esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (cl < 0 || st <= 0) {
            ESP_LOGW(TAG, "OTA: brak naglowkow odpowiedzi");
            break;
        }
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            char tmp[128];   // oproznij tresc przekierowania przed kolejnym zadaniem
            while (esp_http_client_read(c, tmp, sizeof(tmp)) > 0) {}
            if (esp_http_client_set_redirection(c) != ESP_OK) break;
            esp_http_client_close(c);
            continue;
        }
        *status = st;
        *clen   = esp_http_client_get_content_length(c);
        return c;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

typedef enum { DL_OK, DL_UPTODATE, DL_FAIL } dl_result_t;

// Pobierz obraz spod URL i zapisz do nastepnej partycji OTA.
// DL_OK = obraz zapisany, sprawdzony i ustawiony do startu.
static dl_result_t ota_download(const char *url) {
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        snprintf(s_status.error, sizeof(s_status.error), "Brak partycji OTA");
        return DL_FAIL;
    }
    char *buf = malloc(OTA_DL_BUF);
    if (!buf) {
        snprintf(s_status.error, sizeof(s_status.error), "Brak pamieci na bufor OTA");
        return DL_FAIL;
    }

    esp_ota_handle_t oh = 0;
    bool    ota_open = false, complete = false, fatal = false;
    int64_t total = -1, done = 0;     // done = bajty juz zapisane na flash
    int     noprog = 0, tries = 0, last_decile = -1;
    s_status.error[0] = 0;

    while (!complete && !fatal && tries < OTA_MAX_TRIES && noprog < OTA_MAX_NOPROG) {
        if (tries > 0) {
            int wait_s = 2 * noprog + 1;
            if (wait_s > 10) wait_s = 10;
            ESP_LOGW(TAG, "OTA: wznawiam od %lld B za %d s (proba %d)",
                     (long long)done, wait_s, tries + 1);
            vTaskDelay(pdMS_TO_TICKS(wait_s * 1000));
        }
        tries++;
        int64_t done_before = done;
        int st; int64_t cl;
        esp_http_client_handle_t c = ota_dl_open(url, done, &st, &cl);
        if (!c) { noprog++; continue; }

        if (st == 404) {
            // kanal oficjalny (latest/download) daje 404, gdy nie ma zadnego wydania bez bety
            if (s_no_downgrade && !s_gh_beta)
                snprintf(s_status.error, sizeof(s_status.error), "Brak oficjalnego wydania na GitHub");
            else
                snprintf(s_status.error, sizeof(s_status.error), "Brak pliku firmware na GitHub (404)");
            fatal = true;
        } else if (st != 200 && st != 206) {
            ESP_LOGW(TAG, "OTA: HTTP %d", st);
            snprintf(s_status.error, sizeof(s_status.error), "Serwer odpowiedzial HTTP %d", st);
        } else {
            // 200 przy wznowieniu = serwer zignorowal Range: pomin to, co juz jest
            int64_t skip = (st == 200) ? done : 0;
            if (cl > 0) {
                int64_t t = (st == 206) ? done + cl : cl;
                if (total < 0) total = t;
                else if (t != total) {
                    // inny rozmiar = inny plik pod tym samym adresem (nowe wydanie)
                    snprintf(s_status.error, sizeof(s_status.error), "Plik zmienil sie w trakcie pobierania");
                    fatal = true;
                }
            }
            if (!fatal && total > (int64_t)part->size) {
                snprintf(s_status.error, sizeof(s_status.error), "Obraz wiekszy od partycji OTA");
                fatal = true;
            }
            int fill = 0, stall = 0;
            while (!fatal) {
                int r = esp_http_client_read(c, buf + fill, OTA_DL_BUF - fill);
                if (r == -ESP_ERR_HTTP_EAGAIN) {
                    if (++stall > OTA_MAX_STALL) break;   // lacze stoi - polacz od nowa
                    continue;
                }
                if (r < 0) break;                          // zerwane
                if (r == 0) {
                    if (esp_http_client_is_complete_data_received(c) && ota_open)
                        complete = true;
                    break;
                }
                stall = 0;
                if (skip > 0) {
                    int k = (r > skip) ? (int)skip : r;
                    skip -= k;
                    if (k < r) memmove(buf + fill, buf + fill + k, r - k);
                    r -= k;
                    if (r == 0) continue;
                }
                fill += r;

                if (!ota_open) {
                    if (fill < OTA_HDR_NEED) continue;     // naglowek jeszcze niepelny
                    const esp_image_header_t *ih = (const esp_image_header_t *)buf;
                    const esp_app_desc_t *nd = (const esp_app_desc_t *)
                        (buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
                    if (ih->magic != ESP_IMAGE_HEADER_MAGIC || nd->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                        snprintf(s_status.error, sizeof(s_status.error), "Pobrany plik nie jest firmware");
                        fatal = true;
                        break;
                    }
                    if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                        snprintf(s_status.error, sizeof(s_status.error), "Firmware dla innego ukladu ESP32");
                        fatal = true;
                        break;
                    }
                    char ver[sizeof(nd->version) + 1];
                    memcpy(ver, nd->version, sizeof(nd->version));
                    ver[sizeof(nd->version)] = 0;
                    ESP_LOGI(TAG, "Aktualizacja do wersji: %s (build %.16s %.16s)", ver, nd->date, nd->time);
                    const esp_app_desc_t *cur = esp_app_get_description();
                    // Ta sama wersja - nie pobieraj, poinformuj ze aktualne.
                    // Mamy dopiero naglowek, wiec przerywamy zanim sciagniemy calosc.
                    if (strncmp(cur->version, nd->version, sizeof(nd->version)) == 0) {
                        ESP_LOGI(TAG, "OTA: zainstalowana wersja (%s) jest aktualna - pomijam pobieranie", ver);
                        snprintf(s_status.error, sizeof(s_status.error), "Wersja %s jest aktualna", ver);
                        esp_http_client_cleanup(c);
                        free(buf);
                        return DL_UPTODATE;
                    }
                    if (s_no_downgrade) {
                        int64_t sn = ota_build_stamp(nd->date, nd->time);
                        int64_t sc = ota_build_stamp(cur->date, cur->time);
                        if (sn && sc && sn < sc) {
                            ESP_LOGW(TAG, "OTA: %s zbudowana wczesniej niz zainstalowana %s - pomijam",
                                     ver, cur->version);
                            snprintf(s_status.error, sizeof(s_status.error),
                                     "Zainstalowana jest nowsza niz %s", ver);
                            esp_http_client_cleanup(c);
                            free(buf);
                            return DL_UPTODATE;
                        }
                    }
                    const esp_partition_t *run_p = esp_ota_get_running_partition();
                    ESP_LOGI(TAG, "OTA: biezaca partycja=%s, zapis do=%s @0x%lx, rozmiar obrazu=%lld B",
                             run_p ? run_p->label : "?", part->label,
                             (unsigned long)part->address, (long long)total);
                    esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &oh);
                    if (e != ESP_OK) {
                        snprintf(s_status.error, sizeof(s_status.error), "esp_ota_begin: %s", esp_err_to_name(e));
                        fatal = true;
                        break;
                    }
                    ota_open = true;
                    s_status.state = OTA_STATE_WRITING;
                }

                esp_err_t e = esp_ota_write(oh, buf, fill);
                if (e != ESP_OK) {
                    snprintf(s_status.error, sizeof(s_status.error), "Zapis flash: %s", esp_err_to_name(e));
                    fatal = true;
                    break;
                }
                done += fill;
                fill = 0;
                if (total > 0) {
                    s_status.progress_pct = (int)(done * 100 / total);
                    if (s_status.progress_pct / 10 != last_decile) {
                        last_decile = s_status.progress_pct / 10;
                        ESP_LOGI(TAG, "OTA: postep %d%% (%lld/%lld B)", s_status.progress_pct,
                                 (long long)done, (long long)total);
                    }
                    if (done >= total) { complete = true; break; }
                }
            }
            // bajty naglowka sprzed esp_ota_begin przepadaja - nastepna proba
            // i tak zaczyna od "done", czyli od zera
        }
        esp_http_client_cleanup(c);
        if (done > done_before) noprog = 0;
        else if (!complete) noprog++;
    }
    free(buf);

    if (!complete) {
        if (ota_open) esp_ota_abort(oh);
        if (!fatal && (noprog >= OTA_MAX_NOPROG || tries >= OTA_MAX_TRIES)) {
            if (total > 0)
                snprintf(s_status.error, sizeof(s_status.error), "Pobieranie przerwane przy %d%% - slabe lacze?",
                         s_status.progress_pct);
            else if (!s_status.error[0])
                snprintf(s_status.error, sizeof(s_status.error), "Brak polaczenia z serwerem firmware");
        }
        ESP_LOGE(TAG, "OTA: niepowodzenie po %d polaczeniach: %s", tries, s_status.error);
        return DL_FAIL;
    }
    ESP_LOGI(TAG, "OTA: pobrano %lld B w %d polaczeniach, sprawdzam obraz", (long long)done, tries);
    esp_err_t e = esp_ota_end(oh);
    if (e != ESP_OK) {
        snprintf(s_status.error, sizeof(s_status.error), "Obraz niepoprawny: %s", esp_err_to_name(e));
        return DL_FAIL;
    }
    e = esp_ota_set_boot_partition(part);
    if (e != ESP_OK) {
        snprintf(s_status.error, sizeof(s_status.error), "Ustawienie partycji: %s", esp_err_to_name(e));
        return DL_FAIL;
    }
    return DL_OK;
}

// Task OTA z URL (GitHub Releases)
static void ota_url_task(void *arg) {
    char *url = (char *)arg;
    s_status.state        = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;

    // Defence-in-depth: whitelist juz sprawdzona w handle_ota_url, ale
    // ota_url_task wolany jest tez z ota_github_task (URL z API GitHub) - tam
    // odpowiedz API teoretycznie moze zostac zmanipulowana MITM. Ponowna
    // walidacja tutaj gwarantuje ze zaden nieznany host nie zostanie odpytany.
    if (!ota_url_allowed(url)) {
        // Pelna lista dozwolonych w logu (ma miejsce), s_status.error jest krotki
        // wiec krotki komunikat dla UI (uzytkownik zajrzy do logow po szczegoly).
        ESP_LOGE(TAG, "OTA: URL '%s' nie na whitelist (dozwolone: github.com, "
                      "objects.githubusercontent.com, api.github.com, www.smartinhome.pl)", url);
        snprintf(s_status.error, sizeof(s_status.error),
                 "URL poza whitelist (szczegoly w logach)");
        s_status.state = OTA_STATE_FAILED;
        ota_resume_services();   // wolane tez po zatrzymaniu w ota_github_task
        if (url) free(url);
        vTaskDelete(NULL);
        return;
    }

    // Zatrzymaj radio jesli jeszcze dziala (gdy wolane bezposrednio z URL)
    if (!s_svc_stopped) ota_stop_services();

    // Wylacz logi walidacji certyfikatu przy kazdym polaczeniu (zasmiecaja logi)
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    // Zwolnij bufory krzywych minutowych - TLS do GitHuba potrzebuje duzego,
    // spojnego kawalka sterty, a kazde sledzone pole chwilowe trzyma 11.2 KB.
    history_free_curves();
    ESP_LOGI(TAG, "OTA: heap przed pobieraniem = %u B, najwiekszy blok = %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "OTA: rozpoczynam pobieranie firmware z URL");
    ESP_LOGI(TAG, "OTA: %s", url);

    dl_result_t r = ota_download(url);
    if (r == DL_OK) {
        s_status.state        = OTA_STATE_SUCCESS;
        s_status.progress_pct = 100;
        ESP_LOGI(TAG, "OTA OK — restart za 3s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    s_status.state = (r == DL_UPTODATE) ? OTA_STATE_UPTODATE : OTA_STATE_FAILED;
    ota_resume_services();   // tu dochodza tylko nieudane - udana restartuje
    free(url);
    vTaskDelete(NULL);
}

// OTA z bufora (upload przez przeglądarkę)



// ── Pobierz URL najnowszego firmware z GitHub API (na module) ──
// Odpytuje api.github.com, parsuje JSON, znajduje browser_download_url
// dla sih-wmbus-reader.bin. Zwraca GH_OK + wypelnia out_url.
// beta410: tylko kanal beta i tylko gdy panel nie podal tagu. Kanal oficjalny
// idzie przez /releases/latest/download - lista 15 ostatnich wydan od dawna
// zawierala same bety, wiec wersji oficjalnej nie dalo sie znalezc.
#define GITHUB_API_URL "https://api.github.com/repos/smartinhome/SIHOS17/releases?per_page=15"
#define GITHUB_DL_BASE "https://github.com/smartinhome/SIHOS17/releases/"
#define GITHUB_BIN     "sih-wmbus-reader.bin"

typedef enum { GH_OK, GH_NOT_FOUND, GH_RATE_LIMIT, GH_NO_CONN, GH_HTTP, GH_NO_MEM } gh_result_t;

// Stan wyboru najlepszego assetu - parser ogląda teraz kolejne fragmenty
// odpowiedzi, wiec wynik musi przezyc miedzy nimi.
typedef struct {
    int  best;      // najwyzszy dotad numer wersji
    int  count;     // ile assetow widzielismy (do logu)
    bool found;
} gh_pick_t;

// Jeden "browser_download_url" z odpowiedzi API. Filtruje po kanale
// (beta/oficjalna) i zapamietuje URL z najwyzszym numerem wersji - lista
// releasow z API NIE musi byc posortowana wg wersji.
static void github_consider_asset(const char *url, bool beta_channel,
                                  char *out_url, size_t out_max, gh_pick_t *st) {
    st->count++;
    ESP_LOGI(TAG, "GitHub: asset[%d]: %s", st->count, url);
    if (!strstr(url, "sih-wmbus-reader.bin")) return;
    if (strlen(url) >= out_max) return;

    const char *bp = strstr(url, "beta");
    bool is_beta = (bp != NULL);
    if (beta_channel != is_beta) return;      // kanaly sie nie mieszaja

    int ver;
    if (is_beta) {
        ver = atoi(bp + 4);                   // numer beta
    } else {
        // stabilne: "vX.Y.Z" -> X*10000+Y*100+Z do porownania
        const char *vp = strstr(url, "/v");
        int a = 0, b = 0, c = 0;
        if (vp) sscanf(vp + 2, "%d.%d.%d", &a, &b, &c);
        ver = a * 10000 + b * 100 + c;
    }
    if (ver > st->best) {
        st->best = ver;
        strlcpy(out_url, url, out_max);
        st->found = true;
        ESP_LOGI(TAG, "GitHub: kandydat %s, wersja=%d -> %s",
                 is_beta ? "beta" : "oficjalna", ver, out_url);
    }
}

static gh_result_t github_get_latest_bin_url(char *out_url, size_t out_max, bool beta_channel) {
    ESP_LOGI(TAG, "GitHub: wolny heap = %lu B, najwiekszy blok = %lu B",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    // beta373: bufor bierzemy PRZED polaczeniem. mbedTLS zajmuje na handshake
    // kilkadziesiat KB, wiec najwiekszy wolny blok potrafil spasc ponizej 24 KB
    // i malloc konczyl sie bledem "brak pamieci na bufor". Przy okazji nie
    // trzymamy juz calej odpowiedzi (ponad 100 KB) w RAM - parsujemy w locie
    // przez przesuwane okno, wiec nic sie tez nie obcina.
    const size_t CAP  = 4096;     // okno odczytu
    const int    KEEP = 560;      // ogon na rekord przeciety granica bloku
    char *buf = malloc(CAP);
    if (!buf) {
        ESP_LOGE(TAG, "GitHub: brak pamieci na bufor %d B", (int)CAP);
        return GH_NO_MEM;
    }

    ESP_LOGI(TAG, "GitHub: laczenie z %s", GITHUB_API_URL);

    esp_http_client_config_t cfg = {
        .url                         = GITHUB_API_URL,
        .timeout_ms                  = 10000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        // false: wymuszaj walidacje CN cert. api.github.com ma prawidlowy cert
        // dla tego dokladnie hosta. Poprzednie true bylo mimowolnym MITM-holem.
        .skip_cert_common_name_check = false,
        .max_redirection_count       = 10,
        .buffer_size                 = 4096,
        .buffer_size_tx              = 1024,
        .user_agent                  = "SIH-wMbus-Reader",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "GitHub: esp_http_client_init zwrocil NULL");
        free(buf);
        return GH_NO_MEM;
    }

    // Naglowek Accept dla GitHub API
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    ESP_LOGI(TAG, "GitHub: otwieranie polaczenia (TLS handshake)...");
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        err = esp_http_client_open(client, 0);
        ESP_LOGI(TAG, "GitHub: open (proba %d) zwrocil %s", attempt, esp_err_to_name(err));
        if (err == ESP_OK) break;
        // Czesta przyczyna: brak wolnego gniazda (webserver je trzyma).
        // Poczekaj — lru_purge/timeout zwolni gniazda — i sprobuj ponownie.
        ESP_LOGW(TAG, "GitHub: polaczenie nieudane, ponawiam za 2s...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GitHub: open failed po 3 probach: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buf);
        return GH_NO_CONN;
    }

    ESP_LOGI(TAG, "GitHub: pobieranie naglowkow...");
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "GitHub: HTTP status=%d, content_len=%d", status, content_len);

    if (status != 200) {
        ESP_LOGE(TAG, "GitHub: zly status HTTP %d (403=rate limit, 404=brak repo)", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(buf);
        return (status == 403 || status == 429) ? GH_RATE_LIMIT : GH_HTTP;
    }

    // Skanujemy odpowiedz w locie: czytamy blok, wyciagamy z niego komplety
    // "browser_download_url":"...", a niedokonczony rekord z konca przenosimy
    // na poczatek bufora i doklejamy nastepny blok.
    const char *key = "\"browser_download_url\":\"";
    gh_pick_t st = { .best = -1, .count = 0, .found = false };
    int fill = 0, total = 0, r;

    while ((r = esp_http_client_read(client, buf + fill, (int)(CAP - 1 - fill))) > 0) {
        fill  += r;
        total += r;
        buf[fill] = 0;

        char *p = buf, *done = buf;
        while ((p = strstr(p, key)) != NULL) {
            char *v   = p + strlen(key);
            char *end = strchr(v, '"');
            if (!end) break;               // rekord uciety - dokonczymy po doczytaniu
            size_t ulen = end - v;
            if (ulen > 0 && ulen < 470) {
                char tmp[480];
                memcpy(tmp, v, ulen);
                tmp[ulen] = 0;
                github_consider_asset(tmp, beta_channel, out_url, out_max, &st);
            }
            p    = end + 1;
            done = p;
        }

        // Ogon: albo niedokonczony rekord (od "done"), albo ostatnie KEEP bajtow,
        // zeby klucz przeciety granica bloku dalo sie dopasowac.
        int keep = fill - (int)(done - buf);
        if (keep > KEEP) { done = buf + fill - KEEP; keep = KEEP; }
        if (keep < 0) keep = 0;
        if (keep > 0 && done != buf) memmove(buf, done, keep);
        fill = keep;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buf);

    ESP_LOGI(TAG, "GitHub: przeskanowano %d B JSON", total);
    if (total <= 0) {
        ESP_LOGE(TAG, "GitHub: pusta odpowiedz");
        return GH_HTTP;
    }

    bool result     = st.found;
    int  found_count = st.count;
    int  best_beta   = st.best;

    if (result) ESP_LOGI(TAG, "GitHub: WYBRANO %s, wersja=%d: %s",
                         beta_channel ? "beta" : "oficjalna", best_beta, out_url);

    ESP_LOGI(TAG, "GitHub: znaleziono %d assetow, firmware %s",
             found_count, result ? "OK" : "NIE ZNALEZIONO");
    return result ? GH_OK : GH_NOT_FOUND;
}

static void ota_github_task(void *arg) {
    (void)arg;
    s_status.state        = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;
    s_status.error[0]     = 0;

    // Zatrzymaj radio — task RX glodzi siec (wyzszy priorytet, ciagly SPI)
    ESP_LOGI(TAG, "OTA: zatrzymuje radio CC1101 na czas aktualizacji");
    ota_stop_services();
    vTaskDelay(pdMS_TO_TICKS(100));

    char url[480] = {0};
    bool beta_channel = s_gh_beta;
    if (!beta_channel) {
        // GitHub sam przekierowuje na plik z najnowszego wydania nie-beta
        // (bety maja flage prerelease) - bez API i jego limitu zapytan.
        strlcpy(url, GITHUB_DL_BASE "latest/download/" GITHUB_BIN, sizeof(url));
    } else if (s_gh_tag[0]) {
        // Panel zna juz najnowszy tag bety - pobieramy wprost, bez API
        snprintf(url, sizeof(url), GITHUB_DL_BASE "download/%s/" GITHUB_BIN, s_gh_tag);
    } else {
        gh_result_t g = github_get_latest_bin_url(url, sizeof(url), true);
        if (g != GH_OK) {
            const char *m =
                g == GH_RATE_LIMIT ? "GitHub: limit zapytan, sprobuj za godzine" :
                g == GH_NO_CONN    ? "Brak polaczenia z GitHub (internet/DNS)" :
                g == GH_HTTP       ? "GitHub odpowiedzial bledem, sprobuj pozniej" :
                g == GH_NO_MEM     ? "Za malo pamieci na polaczenie z GitHub" :
                                     "Nie znaleziono firmware (beta) na GitHub";
            strlcpy(s_status.error, m, sizeof(s_status.error));
            s_status.state = OTA_STATE_FAILED;
            ota_resume_services();
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGI(TAG, "OTA: kanal %s -> %s", beta_channel ? "beta" : "oficjalny", url);

    // Mamy URL — uruchom standardowy OTA z URL
    s_no_downgrade = true;
    char *url_copy = strdup(url);
    ota_url_task(url_copy);  // wykona sie w tym samym tasku
    vTaskDelete(NULL);
}

// Tag z panelu trafia do URL - tylko bezpieczne znaki nazwy tagu
static bool gh_tag_ok(const char *t) {
    size_t n = strlen(t);
    if (n == 0 || n >= sizeof(s_gh_tag)) return false;
    for (size_t i = 0; i < n; i++) {
        char ch = t[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'))
            return false;
    }
    return strstr(t, "beta") != NULL;   // kanaly sie nie mieszaja
}

void ota_start_from_github(bool beta_channel, const char *tag) {
    if (s_status.state == OTA_STATE_DOWNLOADING ||
        s_status.state == OTA_STATE_WRITING) {
        ESP_LOGW(TAG, "OTA juz w toku");
        return;
    }
    s_status.state    = OTA_STATE_IDLE;
    s_status.error[0] = 0;
    s_gh_beta = beta_channel;
    s_gh_tag[0] = 0;
    if (beta_channel && tag && tag[0]) {
        if (gh_tag_ok(tag)) strlcpy(s_gh_tag, tag, sizeof(s_gh_tag));
        else ESP_LOGW(TAG, "OTA: odrzucony tag '%s' - szukam przez API", tag);
    }
    // beta380: 16 KB jak ota_url - to ta sama sciezka (pobieranie z bundlem
    // CA), tylko poprzedzona krotkim zapytaniem do API. 24 KB to bylo 8 KB
    // zamrozone dokladnie wtedy, gdy pamieci jest najmniej.
    xTaskCreate(ota_github_task, "ota_github", 16384, NULL, 6, NULL);
}

// ── beta411: sprawdzanie nowej wersji oficjalnej ──
// Modul pyta github.com o adres /releases/latest: 5 min po starcie, potem co
// 12 h i na przycisk w panelu. GitHub odpowiada przekierowaniem na
// .../releases/tag/vX.Y.Z najnowszego wydania bez flagi pre-release (bety ja
// maja, wiec nigdy tu nie trafia), a gdy wydania oficjalnego nie ma - na sama
// liste .../releases. To zwykla strona, nie API, wiec nie zuzywa limitu
// zapytan. Niczego nie instaluje: panel pokazuje plakietke przy "System".
#define UPD_URL       "https://github.com/smartinhome/SIHOS17/releases/latest"
#define UPD_FIRST_S   (5 * 60)
#define UPD_PERIOD_S  (12 * 60 * 60)
#define UPD_RETRY_S   (30 * 60)

typedef enum { UPD_UNKNOWN, UPD_OK, UPD_NONE } upd_state_t;
static upd_state_t        s_upd_state;
static bool               s_upd_err;        // ostatnia proba sie nie udala
static char               s_upd_latest[32];
static volatile bool      s_upd_busy;
static esp_timer_handle_t s_upd_timer;

// "v1.0.1" -> {1,0,1,INT_MAX}, "v1.0.1-beta3" -> {1,0,1,3}: wydanie oficjalne
// jest nowsze od kazdej swojej bety, a beta nastepnej wersji od poprzedniej
// oficjalnej (v1.0.0-beta410 < v1.0.0 < v1.0.1-beta1 < v1.0.1).
static bool ver_parse(const char *s, int v[4]) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    if (sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) != 3) return false;
    const char *bp = strstr(s, "-beta");
    v[3] = bp ? atoi(bp + 5) : INT_MAX;
    return true;
}

// >0 gdy a nowsza od b, 0 gdy rowne, <0 gdy starsza. Nierozpoznana = najstarsza.
int ota_version_cmp(const char *a, const char *b) {
    int va[4], vb[4];
    bool oa = ver_parse(a, va), ob = ver_parse(b, vb);
    if (!oa || !ob) return (int)oa - (int)ob;
    for (int i = 0; i < 4; i++)
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

// Adres po przekierowaniu -> tag. 1 = wydanie oficjalne (tag w out),
// 0 = brak wydania oficjalnego, -1 = nierozpoznany adres.
static int upd_tag_from_url(const char *loc, char *out, size_t cap) {
    const char *t = strstr(loc, "/releases/tag/");
    if (t) {
        t += strlen("/releases/tag/");
        size_t n = 0;
        while (t[n] && t[n] != '/' && t[n] != '?' && t[n] != '#') {
            char ch = t[n];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_'))
                return -1;
            n++;
        }
        if (n == 0 || n >= cap) return -1;
        memcpy(out, t, n);
        out[n] = 0;
        return strstr(out, "beta") ? -1 : 1;   // beta nie moze byc "latest"
    }
    size_t l = strlen(loc);
    if (l >= 9 && strcmp(loc + l - 9, "/releases") == 0) return 0;
    return -1;
}

static void upd_schedule(int sec) {
    if (!s_upd_timer) return;
    esp_timer_stop(s_upd_timer);   // gdy nieuzbrojony - zwraca blad, bez znaczenia
    esp_timer_start_once(s_upd_timer, (uint64_t)sec * 1000000ULL);
}

static bool upd_fetch(void) {
    // TLS do GitHuba potrzebuje spojnego bloku sterty. Gdy go nie ma, nie
    // probujemy - sprobujemy za 30 min, zamiast dusic modul w trakcie pracy.
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t fr  = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (big < 24 * 1024 || fr < 48 * 1024) {
        ESP_LOGW(TAG, "Wersja: za malo pamieci na TLS (wolne %u B, blok %u B) - pozniej",
                 (unsigned)fr, (unsigned)big);
        return false;
    }
    esp_http_client_config_t cfg = {
        .url                         = UPD_URL,
        .timeout_ms                  = 15000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = false,
        .buffer_size                 = 4096,
        .buffer_size_tx              = 1024,
        .user_agent                  = "SIH-wMbus-Reader",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        int st = esp_http_client_get_status_code(c);
        char loc[160];
        if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) &&
            esp_http_client_set_redirection(c) == ESP_OK &&
            esp_http_client_get_url(c, loc, sizeof(loc)) == ESP_OK) {
            char tag[sizeof(s_upd_latest)];
            int r = upd_tag_from_url(loc, tag, sizeof(tag));
            if (r == 1) {
                strlcpy(s_upd_latest, tag, sizeof(s_upd_latest));
                s_upd_state = UPD_OK;
                ok = true;
            } else if (r == 0) {
                s_upd_latest[0] = 0;
                s_upd_state = UPD_NONE;
                ok = true;
            } else {
                ESP_LOGW(TAG, "Wersja: nieznany adres %s", loc);
            }
        } else {
            ESP_LOGW(TAG, "Wersja: GitHub odpowiedzial HTTP %d", st);
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

static void upd_task(void *arg) {
    (void)arg;
    bool ok = false;
    if (wifi_manager_get_state() == WIFI_STATE_CONNECTED &&
        s_status.state != OTA_STATE_DOWNLOADING && s_status.state != OTA_STATE_WRITING) {
        esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
        ok = upd_fetch();
        if (ok && s_upd_state == UPD_OK)
            ESP_LOGI(TAG, "Wersja: najnowsza oficjalna %s, zainstalowana %s%s", s_upd_latest,
                     ota_get_running_version(),
                     ota_version_cmp(s_upd_latest, ota_get_running_version()) > 0 ? " - dostepna aktualizacja" : "");
        else if (ok)
            ESP_LOGI(TAG, "Wersja: brak wydania oficjalnego na GitHub");
    }
    s_upd_err  = !ok;
    s_upd_busy = false;
    upd_schedule(ok ? UPD_PERIOD_S : UPD_RETRY_S);
    vTaskDelete(NULL);
}

static void upd_start_task(void) {
    if (s_upd_busy) return;
    s_upd_busy = true;
    // Task tylko na czas sprawdzenia - stos na handshake TLS nie wisi caly czas
    if (xTaskCreate(upd_task, "upd_chk", 10240, NULL, 3, NULL) != pdPASS) {
        s_upd_busy = false;
        upd_schedule(UPD_RETRY_S);
    }
}

static void upd_timer_cb(void *arg) {
    (void)arg;
    if (!nvs_config_upd_check()) { upd_schedule(UPD_PERIOD_S); return; }
    upd_start_task();
}

void ota_update_check_now(void) {
    upd_start_task();
}

void ota_update_set_auto(bool on) {
    if (on) {
        upd_schedule(10);
    } else {
        // Wylaczone - plakietka znika; "Sprawdz teraz" nadal dziala
        s_upd_state = UPD_UNKNOWN;
        s_upd_err   = false;
        s_upd_latest[0] = 0;
    }
}

int ota_update_json(char *buf, size_t cap) {
    const char *st = s_upd_state == UPD_OK ? "ok" : (s_upd_state == UPD_NONE ? "none" : "unknown");
    char latest[sizeof(s_upd_latest)];
    strlcpy(latest, s_upd_latest, sizeof(latest));
    bool nw = s_upd_state == UPD_OK && ota_version_cmp(latest, ota_get_running_version()) > 0;
    return snprintf(buf, cap,
        "\"upd\":{\"auto\":%s,\"busy\":%s,\"state\":\"%s\",\"err\":%s,\"latest\":\"%s\",\"new\":%s}",
        nvs_config_upd_check() ? "true" : "false", s_upd_busy ? "true" : "false", st,
        s_upd_err ? "true" : "false", latest, nw ? "true" : "false");
}

static void upd_init(void) {
    const esp_timer_create_args_t ta = { .callback = upd_timer_cb, .name = "upd_chk" };
    if (esp_timer_create(&ta, &s_upd_timer) == ESP_OK) upd_schedule(UPD_FIRST_S);
}

void ota_manager_init(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Biezaca partycja: %s", running->label);

    // Potwierdz ze firmware dziala (anuluj rollback).
    // Bez tego kolejny OTA zwroci ESP_ERR_OTA_ROLLBACK_INVALID_STATE.
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "OTA: firmware w stanie PENDING_VERIFY - potwierdzam jako sprawny");
            esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
            if (e == ESP_OK)
                ESP_LOGI(TAG, "OTA: firmware potwierdzony, rollback anulowany");
            else
                ESP_LOGW(TAG, "OTA: mark_app_valid blad: %s", esp_err_to_name(e));
        } else {
            ESP_LOGI(TAG, "OTA: stan partycji = %d (juz potwierdzony)", state);
        }
    }
    upd_init();   // beta411: sprawdzanie wersji oficjalnej (pierwsze za 5 min)
}

void ota_start_from_url(const char *url) {
    if (s_status.state == OTA_STATE_DOWNLOADING ||
        s_status.state == OTA_STATE_WRITING) {
        ESP_LOGW(TAG, "OTA juz w toku");
        return;
    }
    char *url_copy = strdup(url);
    s_status.state = OTA_STATE_IDLE;
    s_status.error[0] = 0;
    s_no_downgrade = false;   // wlasny URL: uzytkownik wie, co wgrywa
    xTaskCreate(ota_url_task, "ota_url", 16384, url_copy, 5, NULL);
}



ota_status_t ota_get_status(void) { return s_status; }

const char *ota_get_running_version(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc->version;
}

const char *ota_get_partition_label(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    return running ? running->label : "?";
}
