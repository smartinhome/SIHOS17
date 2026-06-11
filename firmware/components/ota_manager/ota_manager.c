#include "ota_manager.h"
#include <stdlib.h>
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_log.h"
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

    esp_http_client_config_t http_cfg = {
        .url                         = url,
        .timeout_ms                  = 60000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .max_redirection_count       = 10,
        .buffer_size                 = 4096,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config          = &http_cfg,
        .bulk_flash_erase     = false,
        .partial_http_download = true,
        .max_http_request_size = 4096,
    };

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

    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int written = esp_https_ota_get_image_len_read(handle);
        if (image_size > 0)
            s_status.progress_pct = (written * 100) / image_size;
    }

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
    esp_http_client_config_t cfg = {
        .url                         = GITHUB_API_URL,
        .timeout_ms                  = 15000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .max_redirection_count       = 10,
        .buffer_size                 = 2048,
        .user_agent                  = "SIH-wMbus-Reader",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GitHub API open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int content_len = esp_http_client_fetch_headers(client);
    (void)content_len;

    // Czytaj odpowiedz w kawalkach, szukaj browser_download_url konczacego sie .bin
    // JSON moze byc duzy — czytamy strumieniowo do bufora obrotowego
    static char buf[8192];
    int total = 0, r;
    while ((r = esp_http_client_read(client, buf + total,
                                     sizeof(buf) - 1 - total)) > 0) {
        total += r;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    buf[total > 0 ? total : 0] = 0;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total <= 0) {
        ESP_LOGE(TAG, "GitHub API: pusta odpowiedz");
        return false;
    }

    // Znajdz "browser_download_url":"...sih-wmbus-reader.bin"
    const char *key = "\"browser_download_url\":\"";
    char *p = buf;
    while ((p = strstr(p, key)) != NULL) {
        p += strlen(key);
        char *end = strchr(p, '"');
        if (!end) break;
        size_t ulen = end - p;
        if (ulen < out_max && ulen > 4 &&
            strncmp(end - 4, ".bin", 4) == 0) {
            // Sprawdz czy to nasz plik
            char tmp[480];
            strncpy(tmp, p, ulen);
            tmp[ulen] = 0;
            if (strstr(tmp, "sih-wmbus-reader.bin")) {
                strlcpy(out_url, tmp, out_max);
                ESP_LOGI(TAG, "GitHub: znaleziono %s", out_url);
                return true;
            }
        }
        p = end;
    }
    ESP_LOGE(TAG, "GitHub API: nie znaleziono sih-wmbus-reader.bin w odpowiedzi");
    return false;
}

static void ota_github_task(void *arg) {
    (void)arg;
    s_status.state        = OTA_STATE_DOWNLOADING;
    s_status.progress_pct = 0;
    s_status.error[0]     = 0;

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
    xTaskCreate(ota_github_task, "ota_github", 16384, NULL, 5, NULL);
}

void ota_manager_init(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Biezaca partycja: %s", running->label);
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
