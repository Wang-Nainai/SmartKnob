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

/* ---- WiFi / AP 状态 ---- */

static volatile bool s_wifi_conn = false;
static volatile bool s_ap_active = false;
static char s_ap_ssid[33] = "SmartKnob";
static char s_ap_ip[16] = "192.168.4.1";

void app_state_set_wifi(bool connected)
{
    s_wifi_conn = connected;
}

bool app_state_get_wifi(void)
{
    return s_wifi_conn;
}

void app_state_set_ap(bool active, const char *ssid, const char *ip)
{
    /* ssid/ip 先写, active 后置(读侧仅在 active 时取字符串) */
    if (ssid) {
        strncpy(s_ap_ssid, ssid, sizeof(s_ap_ssid) - 1);
        s_ap_ssid[sizeof(s_ap_ssid) - 1] = 0;
    }
    if (ip) {
        strncpy(s_ap_ip, ip, sizeof(s_ap_ip) - 1);
        s_ap_ip[sizeof(s_ap_ip) - 1] = 0;
    }
    s_ap_active = active;
}

bool app_state_get_ap(char *ssid, size_t ssid_len, char *ip, size_t ip_len)
{
    if (ssid && ssid_len) {
        strncpy(ssid, s_ap_ssid, ssid_len - 1);
        ssid[ssid_len - 1] = 0;
    }
    if (ip && ip_len) {
        strncpy(ip, s_ap_ip, ip_len - 1);
        ip[ip_len - 1] = 0;
    }
    return s_ap_active;
}