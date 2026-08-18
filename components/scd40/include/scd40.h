#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define SCD40_I2C_ADDR 0x62

typedef struct {
    uint16_t co2_ppm;
    float temperature_c;
    float humidity_pct;
} scd40_data_t;

esp_err_t scd40_init(void);
esp_err_t scd40_start_periodic(void);
esp_err_t scd40_stop_periodic(void);
esp_err_t scd40_data_ready(bool *ready);
esp_err_t scd40_read(scd40_data_t *data);
