#include <stdlib.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "env_hist.h"

/* 环境历史环形缓冲: scd40 任务按间隔采样。
 * 整块缓冲放 PSRAM, 避免吃内部内存(内部 free 仅 ~7KB)。
 * hi-res 2h@60s 给设备 UI, day 24h@5min 给 web 记录。 */
typedef struct {
    /* 2h @ 60s */
    uint16_t co2[ENV_HIST_N];
    int16_t  temp_x10[ENV_HIST_N];
    int16_t  rh_x10[ENV_HIST_N];
    int      head;
    int      count;
    uint32_t seq;
    int64_t  last_us;
    /* 24h @ 5min */
    uint16_t day_co2[ENV_DAY_N];
    int16_t  day_temp_x10[ENV_DAY_N];
    int16_t  day_rh_x10[ENV_DAY_N];
    int      day_head;
    int      day_count;
    uint32_t day_seq;
    int64_t  last_day_us;
} env_hist_t;

static env_hist_t *s_hist;

void env_hist_push_if_due(uint16_t co2_ppm, float temp_c, float rh_pct)
{
    if (!s_hist) {
        s_hist = heap_caps_calloc(1, sizeof(env_hist_t), MALLOC_CAP_SPIRAM);
        if (!s_hist) {
            s_hist = calloc(1, sizeof(env_hist_t));   /* 无 PSRAM 兜底 */
        }
        if (!s_hist) {
            return;
        }
        s_hist->last_us = -((int64_t)ENV_HIST_MS * 1000);     /* 首样本立即入队 */
        s_hist->last_day_us = -((int64_t)ENV_DAY_MS * 1000);
    }

    int16_t t10 = (int16_t)(temp_c * 10.0f);
    int16_t r10 = (int16_t)(rh_pct * 10.0f);
    int64_t now = esp_timer_get_time();

    if (now - s_hist->last_us >= (int64_t)ENV_HIST_MS * 1000) {
        s_hist->last_us = now;
        s_hist->co2[s_hist->head] = co2_ppm;
        s_hist->temp_x10[s_hist->head] = t10;
        s_hist->rh_x10[s_hist->head] = r10;
        s_hist->head = (s_hist->head + 1) % ENV_HIST_N;
        if (s_hist->count < ENV_HIST_N) {
            s_hist->count++;
        }
        s_hist->seq++;
    }

    if (now - s_hist->last_day_us >= (int64_t)ENV_DAY_MS * 1000) {
        s_hist->last_day_us = now;
        s_hist->day_co2[s_hist->day_head] = co2_ppm;
        s_hist->day_temp_x10[s_hist->day_head] = t10;
        s_hist->day_rh_x10[s_hist->day_head] = r10;
        s_hist->day_head = (s_hist->day_head + 1) % ENV_DAY_N;
        if (s_hist->day_count < ENV_DAY_N) {
            s_hist->day_count++;
        }
        s_hist->day_seq++;
    }
}

uint32_t env_hist_seq(void)
{
    return s_hist ? s_hist->seq : 0;
}

int env_hist_count(void)
{
    return s_hist ? s_hist->count : 0;
}

uint16_t env_hist_co2_at(int i)
{
    if (!s_hist || i < 0 || i >= s_hist->count) {
        return 0;
    }
    int idx = (s_hist->head - s_hist->count + i + ENV_HIST_N * 2) % ENV_HIST_N;
    return s_hist->co2[idx];
}

int16_t env_hist_temp_x10_at(int i)
{
    if (!s_hist || i < 0 || i >= s_hist->count) {
        return 0;
    }
    int idx = (s_hist->head - s_hist->count + i + ENV_HIST_N * 2) % ENV_HIST_N;
    return s_hist->temp_x10[idx];
}

int16_t env_hist_rh_x10_at(int i)
{
    if (!s_hist || i < 0 || i >= s_hist->count) {
        return 0;
    }
    int idx = (s_hist->head - s_hist->count + i + ENV_HIST_N * 2) % ENV_HIST_N;
    return s_hist->rh_x10[idx];
}

uint32_t env_day_seq(void)
{
    return s_hist ? s_hist->day_seq : 0;
}

int env_day_count(void)
{
    return s_hist ? s_hist->day_count : 0;
}

uint16_t env_day_co2_at(int i)
{
    if (!s_hist || i < 0 || i >= s_hist->day_count) {
        return 0;
    }
    int idx = (s_hist->day_head - s_hist->day_count + i + ENV_DAY_N * 2) % ENV_DAY_N;
    return s_hist->day_co2[idx];
}

int16_t env_day_temp_x10_at(int i)
{
    if (!s_hist || i < 0 || i >= s_hist->day_count) {
        return 0;
    }
    int idx = (s_hist->day_head - s_hist->day_count + i + ENV_DAY_N * 2) % ENV_DAY_N;
    return s_hist->day_temp_x10[idx];
}

int16_t env_day_rh_x10_at(int i)
{
    if (!s_hist || i < 0 || i >= s_hist->day_count) {
        return 0;
    }
    int idx = (s_hist->day_head - s_hist->day_count + i + ENV_DAY_N * 2) % ENV_DAY_N;
    return s_hist->day_rh_x10[idx];
}
