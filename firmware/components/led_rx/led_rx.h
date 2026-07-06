#pragma once
#include <stdint.h>
#include <stdbool.h>

// Dioda RX na GPIO19 (inverted - stan niski = swieci), sterowana PWM dla jasnosci.
// Blyska przy odbiorze telegramu wMbus.

// Inicjalizacja PWM. enabled/brightness/blink_ms wczytane z konfiguracji.
void led_rx_init(bool enabled, uint8_t brightness, uint16_t blink_ms);

// Krotki blysk (wywolywane przy odbiorze ramki).
void led_rx_blink(void);

// Zmiana ustawien w locie (np. z panelu). brightness 0-100 (%),
// blink_ms = czas swiecenia po ramce (20-5000 ms).
void led_rx_set(bool enabled, uint8_t brightness, uint16_t blink_ms);
