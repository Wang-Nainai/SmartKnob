#include <stdio.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "led.h"
#include "wifi.h"
#include "display.h"
#include "smartknob_ui.h"
#include "scd40.h"
#include "mqtt.h"
#include "motor.h"
#include "webcfg.h"
#include "input.h"
#include "app_state.h"
#include "blehid.h"

static const char *TAG = "SmartKnob";

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "SNTP time synced");
}

static void init_sntp(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "120.25.115.20");   /* 阿里 NTP 的 IP, 绕过 DNS 便于诊断 */
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_set_sync_interval(10 * 60 * 1000);   /* 10 分钟 */
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized");
}

static void scd40_task(void *arg)
{
    scd40_data_t data;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SCD40_POLL_INTERVAL_MS));
        bool ready = false;
        if (scd40_data_ready(&ready) != ESP_OK || !ready) {
            continue;
        }
        if (scd40_read(&data) == ESP_OK) {
            ESP_LOGI(TAG, "CO2=%u ppm, T=%.1f C, RH=%.1f %%",
                     data.co2_ppm, data.temperature_c, data.humidity_pct);
            app_state_set_env(data.co2_ppm, data.temperature_c, data.humidity_pct);
            mqtt_ha_publish(data.co2_ppm, data.temperature_c, data.humidity_pct);
        }
    }
}

void on_wifi_connected(void)
{
    ESP_LOGI(TAG, "WiFi connected callback");
    led_set_color(0, 255, 0);

    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    ESP_LOGI(TAG, "IP: %s", ip);
    webcfg_set_ip(ip);
    webcfg_start();
}

void on_wifi_disconnected(void)
{
    ESP_LOGI(TAG, "WiFi disconnected callback");
    led_set_color(255, 0, 0);
}

static void on_webcfg_apply(void)
{
    ESP_LOGI(TAG, "web config saved, reconnecting WiFi/MQTT");
    webcfg_set_mqtt_connected(false);
    wifi_reconnect_with_config();
    mqtt_ha_reinit();
}

void app_main(void)
{
    ESP_LOGI(TAG, "SmartKnob starting...");

    /* NVS 尽早初始化: UI 配置/网页配置/BLE 绑定均依赖 */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    app_state_init();
    led_init();
    display_init();

#if CONFIG_MOTOR_ENABLE
    ESP_LOGI(TAG, "Initializing motor");
    if (motor_init() == ESP_OK) {
        ESP_LOGI(TAG, "Motor initialized");
    } else {
        ESP_LOGE(TAG, "Motor init failed");
    }
#endif

    smartknob_ui_init();

    knob_input_init();

    ESP_LOGI(TAG, "Initializing SCD40");
    if (scd40_init() == ESP_OK) {
        scd40_stop_periodic();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_err_t start_err = scd40_start_periodic();
        if (start_err == ESP_OK) {
            ESP_LOGI(TAG, "SCD40 periodic measurement started");
        } else {
            ESP_LOGE(TAG, "SCD40 failed to start measurement, err=0x%X", start_err);
        }
        xTaskCreatePinnedToCore(scd40_task, "scd40", 4096, NULL, 3, NULL, 0);
    } else {
        ESP_LOGE(TAG, "SCD40 init failed");
    }

    wifi_set_callbacks(on_wifi_connected, on_wifi_disconnected);
    webcfg_set_apply_cb(on_webcfg_apply);

    led_set_color(0, 0, 255);
    blehid_init();        /* BLE HID: 电脑控制(S-Dial), 开机可配对 */
    wifi_init();
    init_sntp();

    bool wifi_ok = wifi_wait_connected(
#if CONFIG_WIFI_AP_FALLBACK_ENABLE
        CONFIG_WIFI_AP_FALLBACK_TIMEOUT_SEC * 1000
#else
        30000
#endif
    );
#if CONFIG_WIFI_AP_FALLBACK_ENABLE
    if (!wifi_ok) {
        /* STA 超时: 开热点进配网模式, 屏幕自动弹配网页; STA 后台持续重试 */
        ESP_LOGW(TAG, "WiFi not connected in %ds, starting SoftAP provisioning",
                 CONFIG_WIFI_AP_FALLBACK_TIMEOUT_SEC);
        wifi_ap_fallback_start();
        webcfg_start();   /* 热点网段立即提供管理页(httpd 绑定 0.0.0.0) */
    }
#endif
    if (wifi_ok) {
        ESP_LOGI(TAG, "Successfully connected to WiFi");
    }
    mqtt_ha_init();   /* 幂等; 断网时 esp-mqtt 自动重试, 联网即接上 */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
