#pragma once

#include <stdint.h>

void smartknob_ui_init(void);
/* 供页面读取环境数据(数据源为 app_state) */
void ui_env_get(uint16_t *co2, float *temp, float *rh, uint8_t *has_data);