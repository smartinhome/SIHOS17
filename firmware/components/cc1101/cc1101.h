#pragma once
#include <stdint.h>
#include <stddef.h>
#include "driver/spi_master.h"

typedef struct {
    spi_host_device_t spi_host;
    int  pin_clk;
    int  pin_mosi;
    int  pin_miso;
    int  pin_cs;
    int  pin_gdo0;
    int  pin_gdo2;
    float freq_mhz;
} cc1101_config_t;

typedef struct {
    uint8_t  data[512];
    size_t   len;
    int8_t   rssi;
    uint8_t  lqi;
} wmbus_frame_t;

typedef void (*wmbus_frame_cb_t)(const wmbus_frame_t *frame);

void cc1101_init(const cc1101_config_t *cfg);
void cc1101_start_receive(wmbus_frame_cb_t callback);
void cc1101_stop(void);
