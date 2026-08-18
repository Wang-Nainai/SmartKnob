#pragma once

#include <stdint.h>

void smartknob_ui_init(void);
void smartknob_ui_set_env(uint16_t co2_ppm, float temp_c, float humidity_pct);