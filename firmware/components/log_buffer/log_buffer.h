#pragma once
#include <stddef.h>

// Inicjalizuje przechwytywanie logow ESP-IDF do bufora kolowego.
// Po wywolaniu wszystkie ESP_LOGx trafiaja rownolegle do UART i bufora.
void log_buffer_init(void);

// Kopiuje zawartosc bufora logow do podanego bufora (jako tekst).
// Zwraca liczbe zapisanych bajtow.
size_t log_buffer_dump(char *out, size_t out_max);

// Czysci bufor logow.
void log_buffer_clear(void);
