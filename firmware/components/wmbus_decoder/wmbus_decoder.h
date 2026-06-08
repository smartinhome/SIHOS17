#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cc1101.h"

#define MAX_METER_VALUES 16

typedef struct {
    char    field[32];    // np. "total_energy_consumption_kwh"
    float   value;
    char    unit[8];      // "kWh", "m3", "W", "V"
} meter_field_t;

typedef struct {
    char         id_hex[12];
    char         type[16];
    char         name[32];
    int8_t       rssi;
    uint32_t     last_seen;   // unix timestamp (uptime ms)
    bool         valid;
    uint8_t      field_count;
    meter_field_t fields[MAX_METER_VALUES];
} meter_data_t;

#define MAX_ACTIVE_METERS 8

void          wmbus_decoder_init(void);
void          wmbus_decoder_on_frame(const wmbus_frame_t *frame);
int           wmbus_decoder_get_count(void);
meter_data_t *wmbus_decoder_get_meter(int index);
meter_data_t *wmbus_decoder_find_by_id(const char *id_hex);
