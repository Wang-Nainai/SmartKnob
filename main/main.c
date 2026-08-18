#include <stdio.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "led.h"
#include "wifi.h"
#include "display.h"
#include "smartknob_ui.h"
#include "scd40.h"
#include "mqtt.h"
#include "motor.h"
#include "webcfg.h"

static const char *TAG = "SmartKnob";

static void init_sntp(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "pool.ntp.org");
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
            smartknob_ui_set_env(data.co2_ppm, data.temperature_c, data.humidity_pct);
            webcfg_set_env(data.co2_ppm, data.temperature_c, data.humidity_pct);
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
        xTaskCreate(scd40_task, "scd40", 4096, NULL, 3, NULL);
    } else {
        ESP_LOGE(TAG, "SCD40 init failed");
    }

    wifi_set_callbacks(on_wifi_connected, on_wifi_disconnected);
    webcfg_set_apply_cb(on_webcfg_apply);

    led_set_color(0, 0, 255);
    wifi_init();
    init_sntp();

    if (wifi_wait_connected(15000)) {
        ESP_LOGI(TAG, "Successfully connected to WiFi");
        webcfg_set_mqtt_connected(false);
        mqtt_ha_init();
    } else {
        ESP_LOGW(TAG, "WiFi connection failed");
        led_set_color(255, 0, 0);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
