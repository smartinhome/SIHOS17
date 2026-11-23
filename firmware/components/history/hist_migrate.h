#pragma once
#include <stdbool.h>
#include <stddef.h>

// beta443: montowanie pamieci historii (LittleFS pod "/spiffs") razem z
// jednorazowym przeniesieniem danych z SPIFFS. Zwraca true, gdy LittleFS jest
// zamontowany. false - historia wylaczona (dane na partycji nietkniete).
bool hist_fs_mount(void);

// Wolne bajty partycji (dla hist_store). SIZE_MAX gdy nieznane.
size_t hist_fs_free(void);
