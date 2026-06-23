#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NVS_NAMESPACE "sih_cfg"
#define MAX_METERS    8

typedef struct {
    char     id_hex[12];   // np. "56989134"
    char     type[16];     // "amiplus", "izar", "apator162", "unismart"
    char     key[33];      // klucz AES 32 hex + null
    char     name[32];     // nazwa przyjazna
    bool     enabled;
} meter_config_t;

typedef struct {
    char           wifi_ssid[64];
    char           wifi_pass[64];
    char           ap_ssid[32];
    char           ap_pass[32];
    float          freq_mhz;
    uint8_t        meter_count;
    meter_config_t meters[MAX_METERS];
    bool           led_enabled;     // dioda RX wlaczona
    uint8_t        led_brightness;  // jasnosc 0-100 (%)
    char           dashboard_ids[MAX_METERS][12]; // ID licznikow przypietych do dashboard (niezalezne od meters[])
    bool           led_status_enabled;     // dioda RGB statusu WiFi (GPIO8)
    uint8_t        led_status_brightness;  // jasnosc 0-100 (%)
    // Wlasne nazwy licznikow (niezalezne od meters[]) - ID -> nazwa.
    // Pozwala nazwac dowolny wykryty licznik bez dodawania go do meters[].
    struct {
        char id_hex[12];
        char name[32];
    } meter_names[MAX_METERS];
} sih_config_t;

// Pobierz/ustaw wlasna nazwe licznika po ID. Zwraca "" gdy brak.
const char *nvs_config_meter_name(const char *id_hex);
void        nvs_config_set_meter_name(const char *id_hex, const char *name);

void         nvs_config_init(void);
sih_config_t nvs_config_get(void);
void         nvs_config_save(const sih_config_t *cfg);
void         nvs_config_reset(void);
