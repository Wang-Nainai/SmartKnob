#include <stdio.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
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
#include "env_hist.h"
#include "esp_coexist.h"

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
        /* Sensirion 命令间隔 >=1ms: get_data_ready(0xE1B8) 后立即发
         * read_measurement(0x0344) 会 NACK/超时 (实测 err=0x103 刷屏),
         * v1.0.0-16 偶发成功的那次恰因前次失败自带 200ms 超时间隔 */
        vTaskDelay(pdMS_TO_TICKS(2));
        if (scd40_read(&data) == ESP_OK) {
            ESP_LOGI(TAG, "CO2=%u ppm, T=%.1f C, RH=%.1f %%",
                     data.co2_ppm, data.temperature_c, data.humidity_pct);
            app_state_set_env(data.co2_ppm, data.temperature_c, data.humidity_pct);
            mqtt_ha_publish(data.co2_ppm, data.temperature_c, data.humidity_pct);
            env_hist_push_if_due(data.co2_ppm, data.temperature_c, data.humidity_pct);
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
    /* httpd_start 不在 WiFi 事件任务里调(在该上下文实测会失败), 由主任务启动 */
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
        esp_err_t start_err = scd40_restart_measurement();
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
    /* WiFi 先于 BLE 初始化: WiFi 驱动需要成块内部 DMA 内存,
     * BLE 先起会把内部 RAM 吃掉导致 esp_wifi_init NO_MEM 崩溃(实测) */
    wifi_init();
    blehid_init();        /* BLE HID: 电脑控制(S-Dial), 开机可配对 */
    /* BLE HID 是低速外设, 让共存调度优先保障 WiFi 空口时间。
     * 不设的话 PC 连上 BLE 后(Windows 默认 7.5ms 连接间隔)WiFi 被
     * 高频抢占, 网页几乎无法打开(实测症状: httpd 正常但无响应) */
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    init_sntp();

    /* httpd 绑定 0.0.0.0, 开机即启动, 不依赖 netif/WiFi 事件任务上下文 */
    webcfg_start();

    bool wifi_ok = wifi_wait_connected(
#if CONFIG_WIFI_AP_FALLBACK_ENABLE
        CONFIG_WIFI_AP_FALLBACK_TIMEOUT_SEC * 1000
#else
        30000
#endif
    );
#if CONFIG_WIFI_AP_FALLBACK_ENABLE
    if (!wifi_ok && !wifi_ever_connected()) {
        /* 从未连上过(密码错/路由器不可达): 开热点进配网模式;
         * 连上过之后掉线的走后台无限重连, 不抢 STA 开热点 */
        ESP_LOGW(TAG, "WiFi not connected in %ds, starting SoftAP provisioning",
                 CONFIG_WIFI_AP_FALLBACK_TIMEOUT_SEC);
        wifi_ap_fallback_start();
        webcfg_start();   /* 热点网段立即提供管理页(httpd 绑定 0.0.0.0) */
    } else if (!wifi_ok) {
        ESP_LOGW(TAG, "WiFi connected before but dropped; keep reconnecting (no AP)");
    }
#endif
    if (wifi_ok) {
        ESP_LOGI(TAG, "Successfully connected to WiFi");
        webcfg_start();   /* 主任务里启动管理页, 失败会打印具体错误码 */
    }
    mqtt_ha_init();   /* 幂等; 断网时 esp-mqtt 自动重试, 联网即接上 */

    while (1) {
        /* 堆水位监控: BLE+WiFi 同开时防堆耗尽(曾致 wifi 丢包 + printf 断言崩溃)。
         * 单独监控内部 8bit 内存 -- BLE/WiFi 控制器只从这里分配 */
        ESP_LOGI(TAG, "heap free=%u min=%u largest=%u internal_free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
