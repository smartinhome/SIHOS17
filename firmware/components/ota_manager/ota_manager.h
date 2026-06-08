#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_WRITING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    int         progress_pct;   // 0-100
    char        error[64];
} ota_status_t;

void       ota_manager_init(void);
void       ota_start_from_url(const char *url);
void       ota_start_from_buffer(const uint8_t *data, size_t len);
ota_status_t ota_get_status(void);
const char  *ota_get_running_version(void);
