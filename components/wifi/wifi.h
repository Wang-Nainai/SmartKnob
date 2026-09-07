#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*wifi_callback_t)(void);

void wifi_init(void);
bool wifi_wait_connected(int timeout_ms);
bool wifi_is_connected(void);
/* 本次开机是否曾经连上过(用于判断是否值得开配网热点) */
bool wifi_ever_connected(void);
void wifi_set_callbacks(wifi_callback_t on_connected, wifi_callback_t on_disconnected);
void wifi_get_ip_str(char *buf, size_t len);
/* Re-read NVS config and reconnect to a (possibly new) AP */
void wifi_reconnect_with_config(void);

/* SoftAP 配网回退: STA 超时未连上时开热点, 配网成功后自动关闭 */
void wifi_ap_fallback_start(void);
void wifi_ap_fallback_stop(void);
bool wifi_ap_is_active(void);
void wifi_ap_get_ip_str(char *buf, int buflen);
const char *wifi_ap_get_ssid(void);

#endif
