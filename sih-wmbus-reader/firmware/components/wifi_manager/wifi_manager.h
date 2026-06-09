#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_MODE,
} wifi_state_t;

void         wifi_manager_init(void);
wifi_state_t wifi_manager_get_state(void);
int          wifi_manager_get_rssi(void);
void         wifi_manager_get_ip(char *buf, size_t len);
void         wifi_manager_reconnect(const char *ssid, const char *pass);
