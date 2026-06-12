#include "ota_manager.h"
#include "cc1101.h"
#include <stdlib.h>
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "OTA";
static ota_status_t s_status = { .state = OTA_STATE_IDLE };

// Task OTA z URL (GitHub Releases)
static void ota_url_task(void *arg) {
    char *url = (char *)arg;
    s_status.state        = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;

    // Zatrzymaj radio jesli jeszcze dziala (gdy wolane bezposrednio z URL)
    cc1101_stop();

    esp_http_client_config_t http_cfg = {
        .url                         = url,
        .timeout_ms                  = 60000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .max_redirection_count       = 10,
        .buffer_size                 = 16384,
        .buffer_size_tx              = 4096,
        .user_agent                  = "SIH-wMbus-Reader",
        .keep_alive_enable           = true,
    };

    // Wylacz logi walidacji certyfikatu przy kazdym chunku (zasmiecaja logi)
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_https_ota_config_t ota_cfg = {
        .http_config           = &http_cfg,
        .bulk_flash_erase      = false,
        .partial_http_download = false,
    };

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

    s_status.state = OTA_STATE_WRITING;
    int image_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "OTA: rozmiar obrazu = %d B (%.1f KB)", image_size, image_size/1024.0);

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
static void ota_buffer_task(void *arg) {
    typedef struct { uint8_t *data; size_t len; } buf_arg_t;
    buf_arg_t *ba = (buf_arg_t *)arg;

    s_status.state = OTA_STATE_WRITING;

    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        strlcpy(s_status.error, "Brak partycji OTA", sizeof(s_status.error));
        s_status.state = OTA_STATE_FAILED;
        goto cleanup;
    }

    ESP_ERROR_CHECK(esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES,
                                  &ota_handle));

    size_t chunk = 4096;
    size_t written = 0;
    while (written < ba->len) {
        size_t to_write = (ba->len - written < chunk) ?
                          ba->len - written : chunk;
        esp_err_t err = esp_ota_write(ota_handle, ba->data + written, to_write);
        if (err != ESP_OK) {
            snprintf(s_status.error, sizeof(s_status.error),
                     "esp_ota_write: %s", esp_err_to_name(err));
            s_status.state = OTA_STATE_FAILED;
            esp_ota_abort(ota_handle);
            goto cleanup;
        }
        written += to_write;
        s_status.progress_pct = (written * 100) / ba->len;
    }

    if (esp_ota_end(ota_handle) == ESP_OK &&
        esp_ota_set_boot_partition(update_part) == ESP_OK) {
        s_status.state        = OTA_STATE_SUCCESS;
        s_status.progress_pct = 100;
        ESP_LOGI(TAG, "OTA buffer OK — restart za 3s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        strlcpy(s_status.error, "OTA end/set_boot failed",
                sizeof(s_status.error));
        s_status.state = OTA_STATE_FAILED;
    }

cleanup:
    free(ba->data);
    free(ba);
    vTaskDelete(NULL);
}


// ── Pobierz URL najnowszego firmware z GitHub API (na module) ──
// Odpytuje api.github.com, parsuje JSON, znajduje browser_download_url
// dla sih-wmbus-reader.bin. Zwraca true + wypelnia out_url.
#define GITHUB_API_URL "https://api.github.com/repos/smartinhome/SIHOS17/releases?per_page=1"

static bool github_get_latest_bin_url(char *out_url, size_t out_max) {
    ESP_LOGI(TAG, "GitHub: wolny heap = %lu B, najwiekszy blok = %lu B",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    ESP_LOGI(TAG, "GitHub: laczenie z %s", GITHUB_API_URL);

    esp_http_client_config_t cfg = {
        .url                         = GITHUB_API_URL,
        .timeout_ms                  = 10000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
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
    esp_err_t err = esp_http_client_open(client, 0);
    ESP_LOGI(TAG, "GitHub: open zwrocil %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GitHub: open failed: %s", esp_err_to_name(err));
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

    // Znajdz wszystkie browser_download_url, zaloguj kazdy
    const char *key = "\"browser_download_url\":\"";
    char *p = buf;
    int found_count = 0;
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
                strlcpy(out_url, tmp, out_max);
                ESP_LOGI(TAG, "GitHub: WYBRANO %s", out_url);
                result = true;
                // nie przerywamy — chcemy zalogowac wszystkie assety
            }
        }
        p = end;
    }

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
    vTaskDelay(pdMS_TO_TICKS(100));

    char url[480] = {0};
    if (!github_get_latest_bin_url(url, sizeof(url))) {
        strlcpy(s_status.error, "Nie znaleziono firmware na GitHub",
                sizeof(s_status.error));
        s_status.state = OTA_STATE_FAILED;
        vTaskDelete(NULL);
        return;
    }

    // Mamy URL — uruchom standardowy OTA z URL
    char *url_copy = strdup(url);
    ota_url_task(url_copy);  // wykona sie w tym samym tasku
    vTaskDelete(NULL);
}

void ota_start_from_github(void) {
    if (s_status.state == OTA_STATE_DOWNLOADING ||
        s_status.state == OTA_STATE_WRITING) {
        ESP_LOGW(TAG, "OTA juz w toku");
        return;
    }
    s_status.state    = OTA_STATE_IDLE;
    s_status.error[0] = 0;
    xTaskCreate(ota_github_task, "ota_github", 24576, NULL, 6, NULL);
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

void ota_start_from_buffer(const uint8_t *data, size_t len) {
    typedef struct { uint8_t *data; size_t len; } buf_arg_t;
    buf_arg_t *ba = malloc(sizeof(buf_arg_t));
    ba->data = malloc(len);
    ba->len  = len;
    memcpy(ba->data, data, len);
    s_status.state    = OTA_STATE_IDLE;
    s_status.error[0] = 0;
    xTaskCreate(ota_buffer_task, "ota_buf", 8192, ba, 5, NULL);
}

ota_status_t ota_get_status(void) { return s_status; }

const char *ota_get_running_version(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc->version;
}
