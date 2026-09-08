#pragma once

#include <stdint.h>

#define ENV_HIST_N      120            /* 环形点数 */
#define ENV_HIST_MS     (60 * 1000)    /* 采样间隔 60s -> 2 小时窗口 */

/* 距上次采样满间隔才入队(首个样本立即入队); 缓冲在 PSRAM */
void env_hist_push_if_due(uint16_t co2_ppm, float temp_c);

/* 累计样本数(含已被覆盖的): 值变化即代表有新样本 */
uint32_t env_hist_seq(void);

/* 当前缓冲内的样本数 */
int env_hist_count(void);

/* i=0 最旧, i=count-1 最新; 越界返回 0 */
uint16_t env_hist_co2_at(int i);
int16_t  env_hist_temp_x10_at(int i);
