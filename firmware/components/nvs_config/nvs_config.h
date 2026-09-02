#pragma once
#include <stdint.h>
#include <stdbool.h>

#define NVS_NAMESPACE "sih_cfg"
#define MAX_METERS    8
// Przypiecia i nazwy: osobny, wiekszy limit niz meters[] (klucze AES).
// Scenariusz inkasenta/dozorcy: 20-30 licznikow w budynku, kazdy z wlasna
// nazwa typu "Kwiatowa 5 m. 3", odczyt biezacego stanu z dashboardu.
#define MAX_PINS      32

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
    // --- Rozszerzone przypiecia i nazwy (MAX_PINS) ---
    // Dopisane NA KONCU, a stare tablice zostaja nietkniete: konfiguracja jest
    // zapisywana jako jeden blok, wiec powiekszenie tablic w srodku przesuneloby
    // wszystkie kolejne pola i po aktualizacji OTA przepadlyby ustawienia.
    // Przy pierwszym uruchomieniu stare wpisy sa tu przepisywane (patrz
    // migrate_pins w nvs_config.c) i od tej pory uzywane sa juz tylko te.
    bool     pins_migrated;
    char     pins[MAX_PINS][12];               // ID przypietych do dashboardu
    struct {
        char id_hex[12];
        char name[32];
    } names[MAX_PINS];                         // wlasne nazwy licznikow
    // --- MQTT ---
    // Dopisane NA KONCU struktury (zgodnosc blobu NVS przy aktualizacji OTA).
    bool     mqtt_enabled;
    char     mqtt_host[64];      // adres brokera (IP lub nazwa)
    uint16_t mqtt_port;          // domyslnie 1883
    char     mqtt_user[32];
    char     mqtt_pass[64];
    char     mqtt_prefix[24];    // prefiks tematow, domyslnie "sihos17"
    bool     mqtt_ha_discovery;  // ogloszenia dla Home Assistant
} sih_config_t;

// Pobierz/ustaw wlasna nazwe licznika po ID. Zwraca "" gdy brak.
const char *nvs_config_meter_name(const char *id_hex);
void        nvs_config_set_meter_name(const char *id_hex, const char *name);

void         nvs_config_init(void);
sih_config_t nvs_config_get(void);
// Wskaznik do konfiguracji BEZ kopiowania. nvs_config_get() zwraca cala
// strukture przez wartosc (ponad 3 kB po rozszerzeniu przypiec do 32), co
// przepelnialo stos zadania startowego i zadania odbioru ramek. Do odczytu
// uzywaj tego akcesora; kopii potrzebuja tylko miejsca, ktore modyfikuja
// konfiguracje przed nvs_config_save().
const sih_config_t *nvs_config_ptr(void);
void         nvs_config_save(const sih_config_t *cfg);
void         nvs_config_reset(void);

// Czy licznik o danym ID jest przypiety do dashboardu (case-insensitive).
// Lekki odczyt z cache w RAM - bezpieczny do wolania per ramka.
bool nvs_config_is_pinned(const char *id_hex);
// Lekki getter trybu diody RX (bez kopiowania calej konfiguracji).
bool nvs_config_led_only_pinned(void);

// --- Pola przypiete do dashboardu (klucz "id:pole", np. "56989134:moc_kw") ---
// Trzymane w OSOBNYM blobie NVS ("dashflds"), a NIE w sih_config_t. Powod:
// uklad glownej struktury zostaje nietkniety, wiec aktualizacja OTA nie ma
// szansy przesunac zadnego istniejacego pola i zgubic ustawien.
// Pusta lista dla danego licznika = zachowanie sprzed beta334, czyli na
// dashboardzie pokazuja sie pola sledzone w historii (patrz meterCard w
// webui/index.html). Pierwsze recznie zaznaczone pole przejmuje kontrole.
#define MAX_DASH_FIELDS 32
#define DASH_FIELD_LEN  40

// Czy dane pole jest przypiete do dashboardu (case-insensitive).
bool nvs_config_dash_field_is_set(const char *key);
// Dodaje/usuwa pole. Zwraca false gdy brak wolnego miejsca przy dodawaniu.
bool nvs_config_dash_fields_set(const char *key, bool on);
// Lista przypietych pol jako tablica JSON. Zwraca dlugosc zapisana do buf.
int  nvs_config_dash_fields_json(char *buf, int cap);
// Kasuje cala liste (reset fabryczny).
void nvs_config_dash_fields_clear(void);
