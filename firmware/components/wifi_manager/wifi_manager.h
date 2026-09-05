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

// Ilu klientow jest podlaczonych do naszego AP (0 = nikt).
int wifi_manager_ap_clients(void);
int          wifi_manager_get_rssi(void);
// Kanal AP, z ktorym jestesmy polaczeni (0 = brak polaczenia).
// Z niego panel wyprowadza pasmo: 1-14 to 2,4 GHz, 32 i wyzej to 5 GHz.
int          wifi_manager_get_channel(void);
void         wifi_manager_get_ip(char *buf, size_t len);
void         wifi_manager_reconnect(const char *ssid, const char *pass);

// Skanowanie dostepnych sieci WiFi. Wynik jako JSON do bufora:
// [{"ssid":"...","rssi":-60,"auth":3}, ...]. Dziala w trybie STA i AP (APSTA).
// Zwraca liczbe znalezionych sieci, lub -1 przy bledzie.
int          wifi_manager_scan(char *json_buf, int cap);

// Ostatni powod rozlaczenia STA (kod wifi_err_reason_t z ESP-IDF).
// 15 = AUTH_FAIL (zle haslo), 201 = NO_AP_FOUND. 0 = brak/OK.
// Pozwala UI wykryc zle haslo po restarcie (podejscie B - sprawdzenie po polaczeniu).
int          wifi_manager_last_disc_reason(void);
