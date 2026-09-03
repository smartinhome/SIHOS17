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
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// Szerokosc pojedynczego zakresu Range przy pobieraniu firmware z sieci.
// Im wiekszy, tym mniej handshake'ow TLS - patrz komentarz przy
// max_http_request_size ponizej.
#define OTA_RANGE_CHUNK (256 * 1024)

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
        // Pelna lista dozwolonych w logu (ma miejsce), s_status.error ma tylko 64 B
        // wiec krotki komunikat dla UI (uzytkownik zajrzy do logow po szczegoly).
        ESP_LOGE(TAG, "OTA: URL '%s' nie na whitelist (dozwolone: github.com, "
                      "objects.githubusercontent.com, api.github.com, www.smartinhome.pl)", url);
        snprintf(s_status.error, sizeof(s_status.error),
                 "URL poza whitelist (szczegoly w logach)");
        s_status.state = OTA_STATE_FAILED;
        if (url) free(url);
        vTaskDelete(NULL);
        return;
    }

    // Zatrzymaj radio jesli jeszcze dziala (gdy wolane bezposrednio z URL)
    cc1101_stop();
    display_eink_pause();  // zwolnij SPI i RAM przed OTA

    esp_http_client_config_t http_cfg = {
        .url                         = url,
        .timeout_ms                  = 60000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        // skip_cert_common_name_check = false: wymuszamy walidacje CN certyfikatu
        // wzgledem hosta URL. Wczesniej true pozwalalo kazdemu certowi z bundle
        // CA na podszycie sie za dowolna domene - MITM przez uzyskanie ANY cert
        // od zaufanego CA (Let's Encrypt, DigiCert...) mogl podac wlasny firmware.
        .skip_cert_common_name_check = false,
        .max_redirection_count       = 10,
        .buffer_size                 = 8192,
        .buffer_size_tx              = 4096,
        .user_agent                  = "SIH-wMbus-Reader",
        .keep_alive_enable           = true,
    };

    // Wylacz logi walidacji certyfikatu przy kazdym chunku (zasmiecaja logi)
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_https_ota_config_t ota_cfg = {
        .http_config           = &http_cfg,
        .bulk_flash_erase      = false,
        // FAZA 5a: partial_http_download=true - esp_https_ota pobiera firmware
        // w mniejszych chunk'ach zamiast trzymac calosc w buforach TLS. Mniejsze
        // spójne alokacje w heap podczas OTA = mniejsze ryzyko OOM przy TLS
        // handshake i wieksze prawdopodobienstwo powodzenia auto-OTA z GitHub
        // (wczesniej user musial recznie wgrywac bin przez /api/ota/upload).
        .partial_http_download = true,
        // beta338: bylo 8192. esp_https_ota dla KAZDEGO zakresu robi
        // esp_http_client_close() + open(), czyli pelny handshake TLS i
        // ponowna walidacje lancucha certyfikatow. Przy 8 KB i obrazie
        // ~1,19 MB dawalo to 153 handshake'i (~0,9-1,2 s kazdy) = ok. 3 minut.
        // 256 KB daje 5 zadan zamiast 153. RAM bez zmian - dane i tak plyna
        // przez bufor esp_http_client (buffer_size wyzej), a ta wartosc
        // steruje wylacznie szerokoscia naglowka Range.
        .max_http_request_size = OTA_RANGE_CHUNK,
    };

    // Zwolnij bufory krzywych minutowych - TLS do GitHuba potrzebuje duzego,
    // spojnego kawalka sterty, a kazde sledzone pole chwilowe trzyma 11.2 KB.
    history_free_curves();
    ESP_LOGI(TAG, "OTA: heap przed pobieraniem = %u B, najwiekszy blok = %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "OTA: rozpoczynam pobieranie firmware z URL");
    ESP_LOGI(TAG, "OTA: %s", url);

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        snprintf(s_status.error, sizeof(s_status.error),
                 "OTA begin failed: %s", esp_err_to_name(err));
        s_status.state = OTA_STATE_FAILED;
        free(url);
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t desc;
    esp_https_ota_get_img_desc(handle, &desc);
    ESP_LOGI(TAG, "Aktualizacja do wersji: %s", desc.version);

    // Diagnostyka partycji: z ktorej bootujemy, do ktorej zapiszemy
    const esp_partition_t *run_p = esp_ota_get_running_partition();
    const esp_partition_t *next_p = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "OTA: biezaca partycja=%s @0x%lx, zapis do=%s @0x%lx",
             run_p ? run_p->label : "?", (unsigned long)(run_p ? run_p->address : 0),
             next_p ? next_p->label : "?", (unsigned long)(next_p ? next_p->address : 0));

    // Gdy pobierana wersja == zainstalowana - nie pobieraj, poinformuj ze aktualne.
    // esp_https_ota_begin pobiera tylko naglowek z wersja, wiec przerywamy zanim
    // sciagniemy caly firmware.
    const esp_app_desc_t *cur = esp_app_get_description();
    if (cur && strcmp(cur->version, desc.version) == 0) {
        ESP_LOGI(TAG, "OTA: zainstalowana wersja (%s) jest aktualna - pomijam pobieranie",
                 desc.version);
        snprintf(s_status.error, sizeof(s_status.error),
                 "Wersja %s jest aktualna", desc.version);
        s_status.state = OTA_STATE_UPTODATE;
        esp_https_ota_abort(handle);
        cc1101_start_receive(wmbus_decoder_on_frame);  // wznow radio
        display_eink_resume();                          // wznow e-ink
        free(url);
        vTaskDelete(NULL);
        return;
    }

    s_status.state = OTA_STATE_WRITING;
    int image_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "OTA: rozmiar obrazu = %d B (%.1f KB)", image_size, image_size/1024.0);
    if (image_size > 0) {
        int ranges = (image_size + OTA_RANGE_CHUNK - 1) / OTA_RANGE_CHUNK;
        ESP_LOGI(TAG, "OTA: zakres Range = %d KB -> %d zadan HTTPS",
                 OTA_RANGE_CHUNK / 1024, ranges);
    }

    int last_logged = -1;
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int written = esp_https_ota_get_image_len_read(handle);
        if (image_size > 0) {
            s_status.progress_pct = (written * 100) / image_size;
            int decile = s_status.progress_pct / 10;
            if (decile != last_logged) {
                last_logged = decile;
                ESP_LOGI(TAG, "OTA: postep %d%% (%d/%d B)",
                         s_status.progress_pct, written, image_size);
            }
        }
    }
    ESP_LOGI(TAG, "OTA: petla zakonczona, err=%s", esp_err_to_name(err));

    if (err == ESP_OK && esp_https_ota_is_complete_data_received(handle)) {
        esp_err_t finish = esp_https_ota_finish(handle);
        if (finish == ESP_OK) {
            s_status.state        = OTA_STATE_SUCCESS;
            s_status.progress_pct = 100;
            ESP_LOGI(TAG, "OTA OK — restart za 3s");
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        } else {
            snprintf(s_status.error, sizeof(s_status.error),
                     "OTA finish: %s", esp_err_to_name(finish));
            s_status.state = OTA_STATE_FAILED;
        }
    } else {
        esp_https_ota_abort(handle);
        snprintf(s_status.error, sizeof(s_status.error),
                 "OTA perform: %s", esp_err_to_name(err));
        s_status.state = OTA_STATE_FAILED;
    }

    free(url);
    vTaskDelete(NULL);
}

// OTA z bufora (upload przez przeglądarkę)



// ── Pobierz URL najnowszego firmware z GitHub API (na module) ──
// Odpytuje api.github.com, parsuje JSON, znajduje browser_download_url
// dla sih-wmbus-reader.bin. Zwraca true + wypelnia out_url.
#define GITHUB_API_URL "https://api.github.com/repos/smartinhome/SIHOS17/releases?per_page=15"

static bool github_get_latest_bin_url(char *out_url, size_t out_max, bool beta_channel) {
    ESP_LOGI(TAG, "GitHub: wolny heap = %lu B, najwiekszy blok = %lu B",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
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
        return false;
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
        return false;
    }

    ESP_LOGI(TAG, "GitHub: pobieranie naglowkow...");
    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "GitHub: HTTP status=%d, content_len=%d", status, content_len);

    if (status != 200) {
        ESP_LOGE(TAG, "GitHub: zly status HTTP %d (403=rate limit, 404=brak repo)", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // Wiekszy bufor — odpowiedz GitHub API bywa >16KB
    const size_t BUFSZ = 24576;
    char *buf = malloc(BUFSZ);
    if (!buf) {
        ESP_LOGE(TAG, "GitHub: brak pamieci na bufor %d B", (int)BUFSZ);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int total = 0, r;
    while ((r = esp_http_client_read(client, buf + total,
                                     BUFSZ - 1 - total)) > 0) {
        total += r;
        if (total >= (int)BUFSZ - 1) {
            ESP_LOGW(TAG, "GitHub: odpowiedz obcieta do %d B", (int)BUFSZ);
            break;
        }
    }
    buf[total > 0 ? total : 0] = 0;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "GitHub: odebrano %d B JSON", total);
    if (total <= 0) {
        ESP_LOGE(TAG, "GitHub: pusta odpowiedz");
        free(buf);
        return false;
    }

    // Znajdz wszystkie browser_download_url. Lista releasow z API moze NIE byc
    // posortowana wg numeru wersji (data tagu/dwa rownolegle buildy), wiec
    // wybieramy URL z NAJWYZSZYM numerem beta, nie pierwszy z brzegu.
    const char *key = "\"browser_download_url\":\"";
    char *p = buf;
    int found_count = 0;
    int best_beta = -1;
    bool result = false;
    while ((p = strstr(p, key)) != NULL) {
        p += strlen(key);
        char *end = strchr(p, '"');
        if (!end) break;
        size_t ulen = end - p;
        if (ulen > 0 && ulen < 470) {
            char tmp[480];
            strncpy(tmp, p, ulen);
            tmp[ulen] = 0;
            found_count++;
            ESP_LOGI(TAG, "GitHub: asset[%d]: %s", found_count, tmp);
            if (strstr(tmp, "sih-wmbus-reader.bin") && ulen < out_max) {
                // wyciagnij numer beta z URL ".../v1.0.0-betaN/..."
                char *bp = strstr(tmp, "beta");
                bool is_beta = (bp != NULL);
                // Filtruj wg kanalu: oficjalna = tylko stabilne (bez beta),
                // beta = tylko beta. Kanaly sie nie mieszaja.
                if (beta_channel != is_beta) { p = end; continue; }
                int ver;
                if (is_beta) {
                    ver = atoi(bp + 4);            // numer beta
                } else {
                    // stabilne: numer z "vX.Y.Z" -> X*10000+Y*100+Z dla porownania
                    char *vp = strstr(tmp, "/v");
                    int a = 0, b = 0, cc = 0;
                    if (vp) sscanf(vp + 2, "%d.%d.%d", &a, &b, &cc);
                    ver = a * 10000 + b * 100 + cc;
                }
                if (ver > best_beta) {
                    best_beta = ver;
                    strlcpy(out_url, tmp, out_max);
                    result = true;
                    ESP_LOGI(TAG, "GitHub: kandydat %s (%s) -> %s",
                             is_beta ? "beta" : "stable",
                             is_beta ? "beta" : "oficjalna", out_url);
                }
            }
        }
        p = end;
    }
    if (result) ESP_LOGI(TAG, "GitHub: WYBRANO %s, wersja=%d: %s",
                         beta_channel ? "beta" : "oficjalna", best_beta, out_url);

    ESP_LOGI(TAG, "GitHub: znaleziono %d assetow, firmware %s",
             found_count, result ? "OK" : "NIE ZNALEZIONO");
    free(buf);
    return result;
}

static void ota_github_task(void *arg) {
    (void)arg;
    s_status.state        = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;
    s_status.error[0]     = 0;

    // Zatrzymaj radio — task RX glodzi siec (wyzszy priorytet, ciagly SPI)
    ESP_LOGI(TAG, "OTA: zatrzymuje radio CC1101 na czas aktualizacji");
    cc1101_stop();
    display_eink_pause();  // zwolnij SPI i RAM przed OTA
    vTaskDelay(pdMS_TO_TICKS(100));

    char url[480] = {0};
    bool beta_channel = (bool)(intptr_t)arg;   // kanal przekazany przez arg taska
    if (!github_get_latest_bin_url(url, sizeof(url), beta_channel)) {
        snprintf(s_status.error, sizeof(s_status.error),
                 "Nie znaleziono firmware (%s) na GitHub",
                 beta_channel ? "beta" : "oficjalny");
        s_status.state = OTA_STATE_FAILED;
        vTaskDelete(NULL);
        return;
    }

    // Mamy URL — uruchom standardowy OTA z URL
    char *url_copy = strdup(url);
    ota_url_task(url_copy);  // wykona sie w tym samym tasku
    vTaskDelete(NULL);
}

void ota_start_from_github(bool beta_channel) {
    if (s_status.state == OTA_STATE_DOWNLOADING ||
        s_status.state == OTA_STATE_WRITING) {
        ESP_LOGW(TAG, "OTA juz w toku");
        return;
    }
    s_status.state    = OTA_STATE_IDLE;
    s_status.error[0] = 0;
    xTaskCreate(ota_github_task, "ota_github", 24576,
                (void *)(intptr_t)beta_channel, 6, NULL);
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
