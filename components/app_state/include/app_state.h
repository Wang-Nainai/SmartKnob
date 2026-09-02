#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 应用共享状态（Sensor Manager 数据源）
 * 由传感器任务写入, 任意任务只读(volatile 单字原子快照)。 */

typedef struct {
    uint16_t co2_ppm;
    float temperature_c;
    float humidity_pct;
    bool has_data;
} app_env_t;

esp_err_t app_state_init(void);

/* 由 SCD40 任务调用: 更新环境数据快照 */
void app_state_set_env(uint16_t co2_ppm, float temperature_c, float humidity_pct);

/* 任意任务读取环境数据快照 */
void app_state_get_env(app_env_t *env);

/* ---- WiFi / AP 状态快照(wifi 任务写入, 其它任务只读) ---- */
void app_state_set_wifi(bool connected);
bool app_state_get_wifi(void);

/* active 变为 true 前必须先写好 ssid/ip (写后置位顺序保证) */
void app_state_set_ap(bool active, const char *ssid, const char *ip);
bool app_state_get_ap(char *ssid, size_t ssid_len, char *ip, size_t ip_len);

#ifdef __cplusplus
}
#endif