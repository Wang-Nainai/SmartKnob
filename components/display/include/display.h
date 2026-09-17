#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t display_init(void);
void display_lvgl_lock(void);
void display_lvgl_unlock(void);

/* XPT2046 原始读数(工厂诊断); 返回当前是否触压 */
bool display_touch_get_raw(uint16_t *z1, uint16_t *z2, uint16_t *raw_x, uint16_t *raw_y);

/* 触摸四点校准(仿射): 板轴 -> 屏幕坐标 x'=a*rx+b*ry+c, y'=d*rx+e*ry+f */
bool display_touch_cal_active(void);
void display_touch_cal_set(const float m[6]);
esp_err_t display_touch_cal_save(const float m[6]);

/* Backlight PWM brightness, 0-100 percent (0 = screen off) */
void display_set_brightness(int percent);
int display_get_brightness(void);
/* Call on any user interaction (knob rotation) to wake screen / reset timeout */
void display_notify_activity(void);
/* Auto screen-off timeout in seconds (0 = never off) */
void display_set_screen_timeout(int seconds);
int display_get_screen_timeout(void);
