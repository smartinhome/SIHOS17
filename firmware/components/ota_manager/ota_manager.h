#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_WRITING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
    OTA_STATE_UPTODATE,
} ota_state_t;

typedef struct {
    ota_state_t state;
    int         progress_pct;
    char        error[64];
} ota_status_t;

void          ota_manager_init(void);
void          ota_start_from_url(const char *url);
void          ota_start_from_buffer(const uint8_t *data, size_t len);
void          ota_start_from_github(bool beta_channel);
ota_status_t  ota_get_status(void);
const char   *ota_get_running_version(void);
const char   *ota_get_partition_label(void);

// Whitelist source-of-truth dla OTA. Zwraca true tylko dla https:// URL-i
// zaczynajacych sie od dozwolonego prefiksu (github.com, objects.githubusercontent.com,
// api.github.com, www.smartinhome.pl). Wolane rowniez z api_handlers.c
// przed uruchomieniem OTA - defence-in-depth (fail-fast 400 zanim task startuje).
bool          ota_url_allowed(const char *url);
