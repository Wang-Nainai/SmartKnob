#pragma once

#include <stdint.h>

/* 两组环形缓冲(均存 PSRAM, 重启清零, 不磨损 flash):
 *  - 高分辨率: 120 点 x 60s = 2h, 供设备端环境页趋势图
 *  - 粗粒度:   1440 点 x 1min = 24h, 供 web 管理台记录/展示 */
#define ENV_HIST_N       120
#define ENV_HIST_MS      (60 * 1000)
#define ENV_DAY_N        1440
#define ENV_DAY_MS       (60 * 1000)

/* 距上次采样满间隔才入队(首个样本立即入队) */
void env_hist_push_if_due(uint16_t co2_ppm, float temp_c, float rh_pct);

/* ---- 高分辨率 2h 窗口 ---- */
uint32_t env_hist_seq(void);         /* 累计样本数, 值变化即有新样本 */
int env_hist_count(void);            /* 当前缓冲内样本数 */
uint16_t env_hist_co2_at(int i);     /* i=0 最旧; 越界返回 0 */
int16_t  env_hist_temp_x10_at(int i);
int16_t  env_hist_rh_x10_at(int i);

/* ---- 粗粒度 24h 窗口 (web 用) ---- */
uint32_t env_day_seq(void);
int env_day_count(void);
uint16_t env_day_co2_at(int i);
int16_t  env_day_temp_x10_at(int i);
int16_t  env_day_rh_x10_at(int i);
