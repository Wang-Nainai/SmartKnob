#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "mqtt.h"
#include "hass_cfg.h"
#include "webcfg.h"
#include "led.h"
#include "motor.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include <stdlib.h>

static const char *TAG = "mqtt";

#ifndef CONFIG_MQTT_HA_ENABLE
/* MQTT upload disabled in menuconfig */
void mqtt_ha_init(void) {}
void mqtt_ha_reinit(void) {}
void mqtt_ha_publish(uint16_t co2_ppm, float temp_c, float humidity_pct) { (void)co2_ppm; (void)temp_c; (void)humidity_pct; }
bool mqtt_ha_is_connected(void) { return false; }
bool mqtt_ha_is_configured(void) { return false; }
void mqtt_ha_publish_cmd(const char *device, const char *cmd) { (void)device; (void)cmd; }
void mqtt_ha_publish_action(int dev_idx, const char *act) { (void)dev_idx; (void)act; }
void mqtt_ha_publish_level(const char *key, int value) { (void)key; (void)value; }
#else

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
/* s_client 生命周期互斥: reinit(httpd 任务, stop/destroy) 与
 * publish(scd40/LVGL 任务) 并发时的 use-after-free 防护 */
static SemaphoreHandle_t s_client_mux = NULL;
static volatile uint16_t s_co2 = 0;
static volatile float s_temp = 0.0f;
static volatile float s_rh = 0.0f;
static volatile uint8_t s_has_data = 0;

#define MQTT_DEVICE        CONFIG_MQTT_HA_CLIENT_ID
#define MQTT_TOPIC_STATE   "homeassistant/sensor/" MQTT_DEVICE "/state"
#define MQTT_TOPIC_CO2     "homeassistant/sensor/" MQTT_DEVICE "_co2/config"
#define MQTT_TOPIC_TEMP    "homeassistant/sensor/" MQTT_DEVICE "_temp/config"
#define MQTT_TOPIC_RH      "homeassistant/sensor/" MQTT_DEVICE "_humidity/config"

/* 设备槽位键 dev1..devN (按配置序号, 与具体设备无关, HA 侧映射表按序写) */
#define MQTT_DEV_NUM HASS_MAX_DEVICES

/* ---------------- HA 设备自动化触发器 (旋钮 → HA 任意设备) ----------------
 * 设备列表来自 Web 配置(hass_cfg), 触发器按 dev1..devN 动态生成:
 *   smartknob/action payload = devN_on / devN_off (边沿事件)
 *   smartknob/level/devN_temp / devN_fan (绝对值, 结算发布)
 * HA 侧用一条 YAML 自动化把 devN 映射到真实实体 */

static hass_device_cfg_t s_dev_cfg[MQTT_DEV_NUM];
static int s_dev_num = 0;

/* 历史遗留触发器(占位时代 + 真实设备 v1), 连接时发空保留载荷让 HA 清除 */
static const char *s_legacy_trigs[] = {
    "light_on", "light_off", "light_left", "light_right",
    "ac_on", "ac_off", "ac_left", "ac_right",
    "fan_on", "fan_off", "fan_left", "fan_right",
    "washer_on", "washer_off", "washer_left", "washer_right",
    "bedroom_light_on", "bedroom_light_off", "bedroom_light_bright_up",
    "bedroom_light_bright_down",
    "living_light_on", "living_light_off", "living_light_bright_up",
    "living_light_bright_down",
    "hall_light_on", "hall_light_off", "hall_light_bright_up",
    "hall_light_bright_down",
    "bedroom_ac_on", "bedroom_ac_off", "bedroom_ac_temp_up",
    "bedroom_ac_temp_down", "bedroom_ac_fan_up", "bedroom_ac_fan_down",
};
#define LEGACY_TRIG_NUM (sizeof(s_legacy_trigs) / sizeof(s_legacy_trigs[0]))

static void publish_trigger_one(esp_mqtt_client_handle_t client, int dev, const char *act)
{
    char topic[160];
    char payload[320];
    snprintf(topic, sizeof(topic),
             "homeassistant/device_automation/" MQTT_DEVICE "/dev%d_%s/config",
             dev + 1, act);
    snprintf(payload, sizeof(payload),
             "{\"automation_type\":\"trigger\",\"topic\":\"smartknob/action\","
             "\"payload\":\"dev%d_%s\",\"type\":\"action\",\"subtype\":\"button_%d\","
             "\"device\":{\"identifiers\":[\"" MQTT_DEVICE "\"],\"name\":\"SmartKnob\","
             "\"manufacturer\":\"DIY\",\"model\":\"SmartKnob\"}}",
             dev + 1, act, dev * 2 + (strcmp(act, "on") == 0 ? 1 : 2));
    esp_mqtt_client_publish(client, topic, payload, 0, 1, 1);
}

/* 传感器发现: 3 条小消息(连接时立即发送) */
static void publish_discovery(esp_mqtt_client_handle_t client)
{
    char payload[512];
    int len;

    len = snprintf(payload, sizeof(payload),
        "{\"name\":\"" MQTT_DEVICE " CO2\",\"unique_id\":\"" MQTT_DEVICE "_co2\","
        "\"object_id\":\"" MQTT_DEVICE "_co2\","
        "\"device_class\":\"carbon_dioxide\",\"unit_of_measurement\":\"ppm\","
        "\"state_topic\":\"" MQTT_TOPIC_STATE "\",\"value_template\":\"{{ value_json.co2 }}\","
        "\"device\":{\"identifiers\":[\"" MQTT_DEVICE "\"],\"name\":\"SmartKnob\",\"manufacturer\":\"DIY\"}}");
    esp_mqtt_client_publish(client, MQTT_TOPIC_CO2, payload, len, 1, 1);

    len = snprintf(payload, sizeof(payload),
        "{\"name\":\"" MQTT_DEVICE " Temperature\",\"unique_id\":\"" MQTT_DEVICE "_temp\","
        "\"object_id\":\"" MQTT_DEVICE "_temp\","
        "\"device_class\":\"temperature\",\"unit_of_measurement\":\"\\u00B0C\","
        "\"state_topic\":\"" MQTT_TOPIC_STATE "\",\"value_template\":\"{{ value_json.temp }}\","
        "\"device\":{\"identifiers\":[\"" MQTT_DEVICE "\"],\"name\":\"SmartKnob\",\"manufacturer\":\"DIY\"}}");
    esp_mqtt_client_publish(client, MQTT_TOPIC_TEMP, payload, len, 1, 1);

    len = snprintf(payload, sizeof(payload),
        "{\"name\":\"" MQTT_DEVICE " Humidity\",\"unique_id\":\"" MQTT_DEVICE "_humidity\","
        "\"object_id\":\"" MQTT_DEVICE "_humidity\","
        "\"device_class\":\"humidity\",\"unit_of_measurement\":\"%%\","
        "\"state_topic\":\"" MQTT_TOPIC_STATE "\",\"value_template\":\"{{ value_json.rh }}\","
        "\"device\":{\"identifiers\":[\"" MQTT_DEVICE "\"],\"name\":\"SmartKnob\",\"manufacturer\":\"DIY\"}}");
    esp_mqtt_client_publish(client, MQTT_TOPIC_RH, payload, len, 1, 1);
}

/* 发现消息错开发送: BLE 广播抢占空口时连发会写超时掉线,
 * 改为每 400ms 发 1 条(esp_timer 回调仅入队, 非阻塞).
 * 顺序: 先发旧触发器清理(空保留载荷), 再发当前设备触发器 */
static esp_timer_handle_t s_disc_timer = NULL;
static int s_disc_i = 0;

static void disc_timer_cb(void *arg)
{
    if (!s_connected || !s_client) {
        return;
    }
    /* esp_timer 回调不可阻塞: try-take 失败说明 reinit 正在销毁 client,
     * 跳过本轮(400ms 后重试), 防止检查与 publish 之间 client 被 destroy */
    if (!s_client_mux || xSemaphoreTake(s_client_mux, 0) != pdTRUE) {
        return;
    }
    if (!s_connected || !s_client) {
        xSemaphoreGive(s_client_mux);
        return;
    }
    if (s_disc_i < (int)LEGACY_TRIG_NUM) {
        /* 清理历史遗留触发器 */
        char topic[160];
        snprintf(topic, sizeof(topic),
                 "homeassistant/device_automation/" MQTT_DEVICE "/%s/config",
                 s_legacy_trigs[s_disc_i]);
        esp_mqtt_client_publish(s_client, topic, "", 0, 1, 1);
        s_disc_i++;
    } else {
        int trig_i = s_disc_i - (int)LEGACY_TRIG_NUM;
        if (s_dev_num > 0 && trig_i < s_dev_num * 2) {
            publish_trigger_one(s_client, trig_i / 2, (trig_i % 2) ? "off" : "on");
            s_disc_i++;
        } else {
            esp_timer_stop(s_disc_timer);
            ESP_LOGI(TAG, "discovery complete (%d msgs)", s_disc_i);
        }
    }
    xSemaphoreGive(s_client_mux);
}

/* ---------------- HA → 旋钮 命令 (smartknob/cmnd/<cmd>) ---------------- */

static void handle_cmnd(const char *topic, int topic_len, const char *data, int data_len)
{
    const char *prefix = "smartknob/cmnd/";
    int prefix_len = strlen(prefix);
    if (topic_len <= prefix_len || strncmp(topic, prefix, prefix_len) != 0) {
        return;
    }
    const char *sub = topic + prefix_len;
    int sub_len = topic_len - prefix_len;

    char arg[16] = {0};
    int n = data_len < (int)sizeof(arg) - 1 ? data_len : (int)sizeof(arg) - 1;
    if (data) {
        memcpy(arg, data, n);
    }

    if (sub_len == 5 && !strncmp(sub, "shake", 5)) {
        motor_shake(3, 40);
        ESP_LOGI(TAG, "cmnd: shake");
    } else if (sub_len == 4 && !strncmp(sub, "mode", 4)) {
        int m = atoi(arg);
        motor_set_mode((motor_mode_t)m, 0, 0);
        ESP_LOGI(TAG, "cmnd: mode=%d", m);
    } else if (sub_len == 3 && !strncmp(sub, "led", 3)) {
        if (strlen(arg) == 6) {
            char hex[3] = {0};
            hex[0] = arg[0]; hex[1] = arg[1];
            int r = (int)strtol(hex, NULL, 16);
            hex[0] = arg[2]; hex[1] = arg[3];
            int g = (int)strtol(hex, NULL, 16);
            hex[0] = arg[4]; hex[1] = arg[5];
            int b = (int)strtol(hex, NULL, 16);
            led_set_color(r, g, b);
            ESP_LOGI(TAG, "cmnd: led=%02X%02X%02X", r, g, b);
        }
    }
}

static void publish_state(esp_mqtt_client_handle_t client)
{
    if (!s_has_data) return;
    char payload[96];
    int n = snprintf(payload, sizeof(payload), "{\"co2\":%u,\"temp\":%.1f,\"rh\":%.1f}",
                     s_co2, s_temp, s_rh);
    esp_mqtt_client_publish(client, MQTT_TOPIC_STATE, payload, n, 1, 0);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Connected to broker");
        webcfg_set_mqtt_connected(true);
        led_set_color(0, 255, 255);   /* 青: WiFi+MQTT 就绪 */
        publish_discovery(client);            /* 3 条传感器发现 */
        esp_mqtt_client_subscribe(client, "smartknob/cmnd/#", 1);
        /* 设备列表实时读配置(保存设备后 webcfg 会触发重连, 即刻生效) */
        s_dev_num = hass_cfg_load(s_dev_cfg, MQTT_DEV_NUM);
        ESP_LOGI(TAG, "hass devices: %d", s_dev_num);
        /* 发现消息逐条错开发送(400ms/条), 防止 BLE 抢空口时写超时 */
        if (!s_disc_timer) {
            const esp_timer_create_args_t targs = {
                .callback = disc_timer_cb, .name = "ha_disc",
            };
            esp_timer_create(&targs, &s_disc_timer);
        }
        s_disc_i = 0;
        esp_timer_start_periodic(s_disc_timer, 400 * 1000);
        publish_state(client);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        webcfg_set_mqtt_connected(false);
        if (s_disc_timer) {
            esp_timer_stop(s_disc_timer);
        }
        ESP_LOGW(TAG, "Disconnected from broker");
        led_set_color(0, 255, 0);     /* 绿: 仅 WiFi */
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT error, type=%d", event->error_handle ? event->error_handle->error_type : -1);
        break;
    case MQTT_EVENT_DATA:
        if (event->topic && event->topic_len > 0) {
            handle_cmnd(event->topic, event->topic_len, event->data, event->data_len);
        }
        break;
    default:
        break;
    }
}

static void client_start_locked(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = NULL,
        .credentials.username = NULL,
        .credentials.client_id = CONFIG_MQTT_HA_CLIENT_ID,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 30000,   /* 5s->30s: broker 不可达时 TLS 重连风暴
                                              * (mbedtls ~40KB 内部内存峰值)会挤掉 BLE
                                              * 连接期分配, 实测 BLE_INIT Malloc failed */
        .network.timeout_ms = 30000,   /* BLE 广播抢占空口时发布变慢, 默认 10s 会误超时 */
        .task.stack_size = 4096,   /* 默认 6144, 开机 BLE+WiFi 同开后 6KB 连续栈可能分配失败 */
    };
    char uri[128] = {0};
    char user[64] = {0};
    char pass[64] = {0};
    webcfg_get_str("mqtt_uri", uri, sizeof(uri), CONFIG_MQTT_HA_BROKER_URI);
    webcfg_get_str("mqtt_user", user, sizeof(user), CONFIG_MQTT_HA_USERNAME);
    webcfg_get_str("mqtt_pass", pass, sizeof(pass), CONFIG_MQTT_HA_PASSWORD);
    cfg.broker.address.uri = uri;
    cfg.credentials.username = user;
    if (strlen(pass) > 0) {
        cfg.credentials.authentication.password = pass;
    }

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT client started, broker=%s", uri);
}

void mqtt_ha_init(void)
{
    if (!s_client_mux) {
        s_client_mux = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_client_mux, portMAX_DELAY);
    if (!s_client) {
        client_start_locked();
    }
    xSemaphoreGive(s_client_mux);
}

void mqtt_ha_reinit(void)
{
    if (!s_client_mux) {
        return;
    }
    xSemaphoreTake(s_client_mux, portMAX_DELAY);
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    client_start_locked();
    xSemaphoreGive(s_client_mux);
}

void mqtt_ha_publish(uint16_t co2_ppm, float temp_c, float humidity_pct)
{
    if (!s_connected || !s_client_mux) {
        return;
    }
    s_co2 = co2_ppm;
    s_temp = temp_c;
    s_rh = humidity_pct;
    s_has_data = 1;
    xSemaphoreTake(s_client_mux, portMAX_DELAY);
    if (s_connected && s_client) {
        publish_state(s_client);
    }
    xSemaphoreGive(s_client_mux);
}

bool mqtt_ha_is_connected(void)
{
    return s_connected;
}

bool mqtt_ha_is_configured(void)
{
    return s_client != NULL;
}

void mqtt_ha_publish_cmd(const char *device, const char *cmd)
{
    if (!s_connected || !s_client_mux || !device || !cmd) {
        return;
    }
    xSemaphoreTake(s_client_mux, portMAX_DELAY);
    if (s_connected && s_client) {
        char topic[192];
        char prefix[64] = {0};
        webcfg_get_str("mqtt_topic", prefix, sizeof(prefix), CONFIG_MQTT_HA_TOPIC);
        snprintf(topic, sizeof(topic), "%s/HOME/%s", prefix, device);
        esp_mqtt_client_publish(s_client, topic, cmd, strlen(cmd), 1, 0);
        ESP_LOGI(TAG, "publish %s -> %s", topic, cmd);
    }
    xSemaphoreGive(s_client_mux);
}

/* HA 设备自动化动作: dev_idx(0..N-1, 对应 Web 配置的设备槽位)
 * + act("on"/"off") 发布 smartknob/action, payload = devN_<act>,
 * HA 触发器捕获后在自动化里绑定到真实实体 */
void mqtt_ha_publish_action(int dev_idx, const char *act)
{
    if (!s_connected || !s_client_mux || dev_idx < 0 || dev_idx >= MQTT_DEV_NUM || !act) {
        return;
    }
    char payload[24];
    snprintf(payload, sizeof(payload), "dev%d_%s", dev_idx + 1, act);
    xSemaphoreTake(s_client_mux, portMAX_DELAY);
    if (s_connected && s_client) {
        esp_mqtt_client_publish(s_client, "smartknob/action", payload, 0, 1, 0);
        ESP_LOGI(TAG, "action: %s", payload);
    }
    xSemaphoreGive(s_client_mux);
}

/* 绝对量值通道: smartknob/level/<key>, payload = 整数值.
 * 旋转类调节(温度/风速)用它: 结算后只发一条终值, 惯性滑过若干档
 * 也不会触发多次 HA 动作 */
void mqtt_ha_publish_level(const char *key, int value)
{
    if (!s_connected || !s_client_mux || !key) {
        return;
    }
    char topic[64];
    char payload[16];
    snprintf(topic, sizeof(topic), "smartknob/level/%s", key);
    snprintf(payload, sizeof(payload), "%d", value);
    xSemaphoreTake(s_client_mux, portMAX_DELAY);
    if (s_connected && s_client) {
        esp_mqtt_client_publish(s_client, topic, payload, strlen(payload), 1, 0);
        ESP_LOGI(TAG, "level: %s = %s", topic, payload);
    }
    xSemaphoreGive(s_client_mux);
}
#endif /* CONFIG_MQTT_HA_ENABLE */