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
