#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- NVS-backed config storage (web page writes, components read) ---- */
/* Returns NVS value if present, otherwise the provided fallback. */
void webcfg_get_str(const char *key, char *buf, size_t len, const char *fallback);
void webcfg_set_str(const char *key, const char *value);
void webcfg_erase_all(void);
/* i32 配置存取(与 UI 共用 "webcfg" 命名空间, 如亮度/熄屏) */
void webcfg_set_i32(const char *key, int32_t value);

/* ---- HTTP server (config page + OTA). Call after WiFi is connected. ---- */
void webcfg_start(void);

/* Callback invoked after the user saves new config on the web page */
typedef void (*webcfg_apply_cb_t)(void);
void webcfg_set_apply_cb(webcfg_apply_cb_t cb);

/* ---- Runtime status the web page displays ---- */
void webcfg_set_mqtt_connected(bool connected);
void webcfg_set_ip(const char *ip_str);

#ifdef __cplusplus
}
#endif