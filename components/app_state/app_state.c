#include "app_state.h"
#include "esp_log.h"

static const char *TAG = "app_state";

/* 跨任务只读快照: 单字对齐访问, Xtensa 上原子, 由 scd40 任务写入 */
static volatile uint16_t s_co2 = 0;
static volatile float s_temp = 0.0f;
static volatile float s_rh = 0.0f;
static volatile bool s_has_data = false;

esp_err_t app_state_init(void)
{
    s_co2 = 0;
    s_temp = 0.0f;
    s_rh = 0.0f;
    s_has_data = false;
    ESP_LOGI(TAG, "app state initialized");
    return ESP_OK;
}

void app_state_set_env(uint16_t co2_ppm, float temperature_c, float humidity_pct)
{
    s_co2 = co2_ppm;
    s_temp = temperature_c;
    s_rh = humidity_pct;
    s_has_data = true;
}

void app_state_get_env(app_env_t *env)
{
    if (!env) {
        return;
    }
    env->co2_ppm = s_co2;
    env->temperature_c = s_temp;
    env->humidity_pct = s_rh;
    env->has_data = s_has_data;
}