#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Wyswietlacz Waveshare 2.13" v3 (SSD1680), 250x122, wspoldzieli SPI z CC1101.
// Rotacja 270 (jak w konfiguracji ESPHome) => logiczne 250 (szer) x 122 (wys).

typedef struct {
    spi_host_device_t spi_host;   // ta sama magistrala co CC1101 (SPI2_HOST)
    int pin_cs;
    int pin_dc;
    int pin_busy;
    int pin_rst;
} display_eink_config_t;

// Inicjalizacja: dodaje wyswietlacz jako urzadzenie na ISTNIEJACEJ magistrali SPI.
// Magistrala musi byc juz zainicjalizowana (przez cc1101_init).
bool display_eink_init(const display_eink_config_t *cfg);

// Pokaz ekran startowy: logo SIH + kod QR do smartinhome.pl.
void display_eink_show_splash(void);

// Odswiez strony liczników na podstawie danych z historii (sledzone pola).
// Wywolaj okresowo lub po nadejsciu ramki. Pokazuje aktualnie wybrana strone.
void display_eink_refresh_pages(void);

// Przelacz na nastepna strone licznika (krotkie nacisniecie przycisku).
void display_eink_next_page(void);

// Wroc do pierwszej strony (dlugie nacisniecie przycisku).
void display_eink_first_page(void);

// Uruchom obsluge przycisku BOOT (przelaczanie stron) + okresowe odswiezanie.
// pin_button - GPIO przycisku BOOT (aktywny w stanie niskim).
void display_eink_start_tasks(int pin_button);

// Wstrzymaj taski e-ink (przed OTA - zwalnia magistrale SPI i redukuje
// aktywnosc/fragmentacje heap). Taski same nie wznawiaja sie - po OTA restart.
void display_eink_pause(void);
void display_eink_resume(void);

#ifdef __cplusplus
}
#endif
