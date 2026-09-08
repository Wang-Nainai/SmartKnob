#include <stdlib.h>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "env_hist.h"

/* 环境历史环形缓冲: scd40 任务按间隔采样, 环境页趋势图读取。
 * 整块缓冲放 PSRAM, 避免吃内部内存(内部 free 仅 ~7KB) */
typedef struct {
    uint16_t co2[ENV_HIST_N];
    int16_t  temp_x10[ENV_HIST_N];
    int      head;      /* 下一个写入位置 */
    int      count;
    uint32_t seq;       /* 累计样本数 */
    int64_t  last_us;
} env_hist_t;

static env_hist_t *s_hist;

void env_hist_push_if_due(uint16_t co2_ppm, float temp_c)
{
    if (!s_hist) {
        s_hist = heap_caps_calloc(1, sizeof(env_hist_t), MALLOC_CAP_SPIRAM);
        if (!s_hist) {
            s_hist = calloc(1, sizeof(env_hist_t));   /* 无 PSRAM 兜底 */
        }
        if (!s_hist) {
            return;
        }
        s_hist->last_us = -((int64_t)ENV_HIST_MS * 1000);   /* 首样本立即入队 */
    }
    int64_t now = esp_timer_get_time();
    if (now - s_hist->last_us < (int64_t)ENV_HIST_MS * 1000) {
        return;
    }
    s_hist->last_us = now;
    s_hist->co2[s_hist->head] = co2_ppm;
    s_hist->temp_x10[s_hist->head] = (int16_t)(temp_c * 10.0f);
    s_hist->head = (s_hist->head + 1) % ENV_HIST_N;
    if (s_hist->count < ENV_HIST_N) {
        s_hist->count++;
    }
    s_hist->seq++;
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
