#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "mqtt.h"
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

#define MQTT_DEV_NUM 4

/* ---------------- HA 设备自动化触发器 (旋钮 → HA 任意设备) ----------------
 * 真实设备槽位: 3 盏灯(开关/亮度步进) + 卧室空调(开关/温度/风速).
 * 触发消息: smartknob/action  payload = <dev>_<act> (如 bedroom_light_on)
 * HA 侧用单条 YAML 自动化按 payload 映射到真实实体 */

static const char *ha_dev_keys[MQTT_DEV_NUM] = {
    "bedroom_light", "living_light", "hall_light", "bedroom_ac",
};

typedef struct {
    uint8_t dev;
    const char *act;
} ha_trig_t;

static const ha_trig_t ha_trigs[] = {
    {0, "on"}, {0, "off"},
    {1, "on"}, {1, "off"},
    {2, "on"}, {2, "off"},
    {3, "on"}, {3, "off"},
};
#define HA_TRIG_NUM (sizeof(ha_trigs) / sizeof(ha_trigs[0]))

static void publish_trigger_one(esp_mqtt_client_handle_t client, int t)
{
    char topic[160];
    char payload[320];
    snprintf(topic, sizeof(topic),
             "homeassistant/device_automation/" MQTT_DEVICE "/%s_%s/config",
             ha_dev_keys[ha_trigs[t].dev], ha_trigs[t].act);
    snprintf(payload, sizeof(payload),
             "{\"automation_type\":\"trigger\",\"topic\":\"smartknob/action\","
             "\"payload\":\"%s_%s\",\"type\":\"action\",\"subtype\":\"button_%d\","
             "\"device\":{\"identifiers\":[\"" MQTT_DEVICE "\"],\"name\":\"SmartKnob\","
             "\"manufacturer\":\"DIY\",\"model\":\"SmartKnob\"}}",
             ha_dev_keys[ha_trigs[t].dev], ha_trigs[t].act, t + 1);
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

/* 16 条触发器发现错开发送: BLE 广播抢占空口时连发 19 条会写超时掉线,
 * 改为每 400ms 发 1 条(esp_timer 回调仅入队, 非阻塞) */
static esp_timer_handle_t s_disc_timer = NULL;
static int s_disc_i = 0;

static void disc_timer_cb(void *arg)
{
    if (!s_connected || !s_client) {
        return;
    }
    if (s_disc_i < (int)HA_TRIG_NUM) {
        publish_trigger_one(s_client, s_disc_i);
        s_disc_i++;
    } else {
        esp_timer_stop(s_disc_timer);
        ESP_LOGI(TAG, "discovery complete (%d msgs)", s_disc_i);
    }
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

/* 旧版占位触发器(light/ac/fan/washer x on/off/left/right)的发现消息是
 * retained 的, 换真实设备后发空保留载荷让 HA 清掉它们 */
static void publish_legacy_cleanup(esp_mqtt_client_handle_t client)
{
    static const char *old_devs[4] = {"light", "ac", "fan", "washer"};
    static const char *old_acts[4] = {"on", "off", "left", "right"};
    for (int d = 0; d < 4; d++) {
        for (int a = 0; a < 4; a++) {
            char topic[128];
            snprintf(topic, sizeof(topic),
                     "homeassistant/device_automation/" MQTT_DEVICE "/%s_%s/config",
                     old_devs[d], old_acts[a]);
            esp_mqtt_client_publish(client, topic, "", 0, 1, 1);
        }
    }
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
        publish_legacy_cleanup(client);       /* 清掉旧占位触发器的 retained 消息 */
        esp_mqtt_client_subscribe(client, "smartknob/cmnd/#", 1);
        /* 16 条触发器发现逐条错开发送(400ms/条), 防止 BLE 抢空口时写超时 */
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

/* HA 设备自动化动作: dev_idx(0-3) + act("on"/"off"/"bright_up"/"temp_up"/...)
 * 发布 smartknob/action, payload = <dev>_<act>, HA 触发器捕获后
 * 在自动化里绑定到真实实体 —— 旋钮直接控制 HA 设备的标准通道 */
void mqtt_ha_publish_action(int dev_idx, const char *act)
{
    if (!s_connected || !s_client_mux || dev_idx < 0 || dev_idx >= MQTT_DEV_NUM || !act) {
        return;
    }
    char payload[40];
    snprintf(payload, sizeof(payload), "%s_%s", ha_dev_keys[dev_idx], act);
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