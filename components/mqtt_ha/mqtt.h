#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Starts the MQTT client (only if CONFIG_MQTT_HA_ENABLE). Call after WiFi is up. */
void mqtt_ha_init(void);
/* Restart the MQTT client with config from NVS (used after web config save) */
void mqtt_ha_reinit(void);
/* Publish latest CO2/temp/humidity to Home Assistant (no-op if disabled/disconnected) */
void mqtt_ha_publish(uint16_t co2_ppm, float temp_c, float humidity_pct);
/* True if MQTT enabled and currently connected to broker */
bool mqtt_ha_is_connected(void);
/* True if MQTT client was created (broker configured,连接尝试中/失败也算) */
bool mqtt_ha_is_configured(void);
/* Publish a HASS control command (X-Knob style): <topic>/HOME/<device> with payload cmd */
void mqtt_ha_publish_cmd(const char *device, const char *cmd);
/* HA 设备自动化动作: dev_idx 0-3, act 为小写动作名
 * ("on"/"off"/"bright_up"/"bright_down"/"temp_up"/"temp_down"/"fan_up"/"fan_down")
 * 发布 smartknob/action, payload = <dev>_<act>, HA 触发器可视化绑定任意实体 */
void mqtt_ha_publish_action(int dev_idx, const char *act);