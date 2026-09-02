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
#include <stdlib.h>

static const char *TAG = "mqtt";

#ifndef CONFIG_MQTT_HA_ENABLE
/* MQTT upload disabled in menuconfig */
void mqtt_ha_init(void) {}
void mqtt_ha_reinit(void) {}
void mqtt_ha_publish(uint16_t co2_ppm, float temp_c, float humidity_pct) { (void)co2_ppm; (void)temp_c; (void)humidity_pct; }
bool mqtt_ha_is_connected(void) { return false; }
void mqtt_ha_publish_cmd(const char *device, const char *cmd) { (void)device; (void)cmd; }
#else

static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
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

static void publish_device_automation(esp_mqtt_client_handle_t client);

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

    publish_device_automation(client);
}

/* ---------------- HA 设备自动化触发器 (旋钮 → HA 任意设备) ----------------
 * 发布 16 个 device_automation 触发器(4 设备 x on/off/left/right)的发现配置,
 * HA 界面里 SmartKnob 设备出现这些"动作", 自动化可视化绑定到任意实体, 无需 YAML。
 * 触发消息: smartknob/action  payload = <dev>_<act> (如 light_on) */

static const char *ha_dev_keys[MQTT_DEV_NUM] = {"light", "ac", "fan", "washer"};
static const char *ha_dev_acts[4] = {"on", "off", "left", "right"};

static void publish_device_automation(esp_mqtt_client_handle_t client)
{
    char topic[160];
    char payload[320];
    for (int d = 0; d < MQTT_DEV_NUM; d++) {
        for (int a = 0; a < 4; a++) {
            snprintf(topic, sizeof(topic),
                     "homeassistant/device_automation/" MQTT_DEVICE "/%s_%s/config",
                     ha_dev_keys[d], ha_dev_acts[a]);
            snprintf(payload, sizeof(payload),
                     "{\"automation_type\":\"trigger\",\"topic\":\"smartknob/action\","
                     "\"payload\":\"%s_%s\",\"type\":\"action\",\"subtype\":\"button_%d\","
                     "\"device\":{\"identifiers\":[\"" MQTT_DEVICE "\"],\"name\":\"SmartKnob\","
                     "\"manufacturer\":\"DIY\",\"model\":\"SmartKnob\"}}",
                     ha_dev_keys[d], ha_dev_acts[a], d * 4 + a + 1);
            esp_mqtt_client_publish(client, topic, payload, 0, 1, 1);
        }
    }
    /* 订阅 HA → 旋钮 命令通道 */
    esp_mqtt_client_subscribe(client, "smartknob/cmnd/#", 1);
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
        publish_discovery(client);
        publish_state(client);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        webcfg_set_mqtt_connected(false);
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

static void mqtt_client_start(void)
{
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = NULL,
        .credentials.username = NULL,
        .credentials.client_id = CONFIG_MQTT_HA_CLIENT_ID,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
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
    if (s_client) {
        return;   /* 幂等: 断网场景 esp-mqtt 自带重连 */
    }
    mqtt_client_start();
}

void mqtt_ha_reinit(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    mqtt_client_start();
}

void mqtt_ha_publish(uint16_t co2_ppm, float temp_c, float humidity_pct)
{
    if (!s_connected || !s_client) return;
    s_co2 = co2_ppm;
    s_temp = temp_c;
    s_rh = humidity_pct;
    s_has_data = 1;
    publish_state(s_client);
}

bool mqtt_ha_is_connected(void)
{
    return s_connected;
}

void mqtt_ha_publish_cmd(const char *device, const char *cmd)
{
    if (!s_connected || !s_client || !device || !cmd) {
        return;
    }
    char topic[192];
    char prefix[64] = {0};
    webcfg_get_str("mqtt_topic", prefix, sizeof(prefix), CONFIG_MQTT_HA_TOPIC);
    snprintf(topic, sizeof(topic), "%s/HOME/%s", prefix, device);
    esp_mqtt_client_publish(s_client, topic, cmd, strlen(cmd), 1, 0);
    ESP_LOGI(TAG, "publish %s -> %s", topic, cmd);
}

/* HA 设备自动化动作: dev_idx(0-3) + cmd("ON"/"OFF"/"LEFT"/"RIGHT")
 * 发布 smartknob/action, HA 触发器(发现配置见 publish_device_automation)捕获后
 * 可在自动化里绑定到任意实体 —— 旋钮直接控制 HA 设备的标准通道 */
void mqtt_ha_publish_action(int dev_idx, const char *cmd)
{
    if (!s_connected || !s_client || dev_idx < 0 || dev_idx >= MQTT_DEV_NUM || !cmd) {
        return;
    }
    char cmdkey[8] = {0};
    int i = 0;
    while (cmd[i] && i < 7) {
        cmdkey[i] = (cmd[i] >= 'A' && cmd[i] <= 'Z') ? cmd[i] - 'A' + 'a' : cmd[i];
        i++;
    }
    char payload[32];
    snprintf(payload, sizeof(payload), "%s_%s", ha_dev_keys[dev_idx], cmdkey);
    esp_mqtt_client_publish(s_client, "smartknob/action", payload, 0, 1, 0);
    ESP_LOGI(TAG, "action: %s", payload);
}
#endif /* CONFIG_MQTT_HA_ENABLE */