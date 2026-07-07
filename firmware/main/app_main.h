#pragma once
#include <stdint.h>

// Wersja firmware
#define FW_VERSION_MAJOR  1
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  4
#define FW_VERSION_STR    "1.0.4"

// Piny CC1101 (ESP32-C6)
#define CC1101_SPI_HOST   SPI2_HOST
#define CC1101_PIN_CLK    6
#define CC1101_PIN_MOSI   7
#define CC1101_PIN_MISO   2
#define CC1101_PIN_CS     10
#define CC1101_PIN_GDO0   5
#define CC1101_PIN_GDO2   3

// Piny e-ink Waveshare 2.13" v3
#define EINK_PIN_CS       11
#define EINK_PIN_DC       21
#define EINK_PIN_BUSY     20
#define EINK_PIN_RST      22

// LED RGB WS2812
#define LED_PIN           8
#define LED_NUM           1

// Przycisk BOOT
#define BOOT_BUTTON_PIN   9

// LED status wMbus
#define STATUS_LED_PIN    19

// Częstotliwość radia
#define WMBUS_FREQ_MHZ    868.950f

void app_main_init(void);
