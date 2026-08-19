#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Wyciaga skumulowana wartosc "total" (m3 lub kWh) z surowej ramki wMbus.
// Obsluguje: IZAR (LFSR), Apator woda (AES+rejestry), Amiplus/Unismart (AES+DIF/VIF).
// data/len - surowa ramka (z eteru, z CRC blokow).
// key_hex  - klucz AES 32 znaki hex (NULL lub "" jesli brak/niepotrzebny).
// out_total - wynik (m3 dla wody/gazu, kWh dla energii).
// out_kind  - 0=nieznany, 1=woda/gaz (m3), 2=energia (kWh).
// Zwraca true gdy udalo sie wyciagnac total.
bool meter_total_extract(const uint8_t *data, size_t len,
                         const char *key_hex,
                         double *out_total, int *out_kind);

// Krotka nazwa sterownika rozpoznana z naglowka ramki (do logow).
// Znak zapytania na koncu = medium rozpoznane, ale bez dedykowanego sterownika.
const char *meter_total_driver_name(const uint8_t *data, size_t len);

// Czy ramka (surowa, z CRC blokow) jest zaszyfrowana (tryb != 0) i wymaga klucza?
// Zwraca false dla ramek jawnych lub juz odszyfrowanych (payload 2F2F).
bool meter_total_needs_key(const uint8_t *data, size_t len);

// --- Wielopolowa ekstrakcja (dla licznika energii: energia, moc, napiecia) ---
#define MTF_MAX_FIELDS 24   // Amiplus: energia+taryfy, moc, napiecia,
                             // prady, moc/energia bierna = do 20 pol
typedef struct {
    char   field[24];   // nazwa pola, np. "energia_kwh", "napiecie_l1_v"
    double value;
    char   unit[8];     // "kWh","kW","V","m3"
    int    cumulative;  // 1=kumulacyjne (roznica/zuzycie), 0=chwilowe (wartosc)
} mtf_field_t;

// Wyciaga wszystkie istotne pola z ramki. Zwraca liczbe pol (0 gdy brak).
// kind: 1=woda,2=prad,3=gaz (jak w meter_total_extract).
int meter_total_extract_fields(const uint8_t *data, size_t len,
                               const char *key_hex,
                               mtf_field_t *out, int max_fields, int *out_kind);
