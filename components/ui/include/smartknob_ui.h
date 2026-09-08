#pragma once

#include <stdint.h>
#include "lvgl.h"

void smartknob_ui_init(void);
/* 供页面读取环境数�?数据源为 app_state) */
void ui_env_get(uint16_t *co2, float *temp, float *rh, uint8_t *has_data);
/* 数值滚动更新: 文本变化时旧值下滑淡出、新值自上滑入 (文本未变则无操作) */
void ui_label_roll(lv_obj_t *label, const char *text);