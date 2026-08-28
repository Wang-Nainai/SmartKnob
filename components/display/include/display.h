#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t display_init(void);
esp_err_t display_show_text(const char *text);
void display_set_status_label(void *label);
void display_lvgl_lock(void);
void display_lvgl_unlock(void);

/* XPT2046 原始读数(工厂诊断); 返回当前是否触压 */
bool display_touch_get_raw(uint16_t *z1, uint16_t *z2, uint16_t *raw_x, uint16_t *raw_y);

/* Backlight PWM brightness, 0-100 percent (0 = screen off) */
void display_set_brightness(int percent);
int display_get_brightness(void);
/* Call on any user interaction (knob rotation) to wake screen / reset timeout */
void display_notify_activity(void);
/* Auto screen-off timeout in seconds (0 = never off) */
void display_set_screen_timeout(int seconds);
int display_get_screen_timeout(void);
