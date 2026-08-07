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
    // Nowe pola ZAWSZE na koncu (zgodnosc blobu NVS przy aktualizacji OTA).
    bool     led_only_pinned;   // mrugaj tylko dla licznikow z dashboardu
    uint16_t led_blink_ms;      // czas swiecenia po ramce (20-5000 ms, domyslnie 60)
    bool     logs_enabled;      // zakladka Logi widoczna (domyslnie false)
} sih_config_t;

// Pobierz/ustaw wlasna nazwe licznika po ID. Zwraca "" gdy brak.
const char *nvs_config_meter_name(const char *id_hex);
void        nvs_config_set_meter_name(const char *id_hex, const char *name);

void         nvs_config_init(void);
sih_config_t nvs_config_get(void);
void         nvs_config_save(const sih_config_t *cfg);
void         nvs_config_reset(void);

// Czy licznik o danym ID jest przypiety do dashboardu (case-insensitive).
// Lekki odczyt z cache w RAM - bezpieczny do wolania per ramka.
bool nvs_config_is_pinned(const char *id_hex);
// Lekki getter trybu diody RX (bez kopiowania calej konfiguracji).
bool nvs_config_led_only_pinned(void);
