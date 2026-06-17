#pragma once
#include <stdint.h>
#include <stdbool.h>

// Dioda RX na GPIO19 (inverted - stan niski = swieci), sterowana PWM dla jasnosci.
// Blyska przy odbiorze telegramu wMbus.

// Inicjalizacja PWM. enabled/brightness wczytane z konfiguracji.
void led_rx_init(bool enabled, uint8_t brightness);

// Krotki blysk (wywolywane przy odbiorze ramki).
void led_rx_blink(void);

// Zmiana ustawien w locie (np. z panelu). brightness 0-100 (%).
void led_rx_set(bool enabled, uint8_t brightness);
