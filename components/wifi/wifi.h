#ifndef WIFI_H
#define WIFI_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*wifi_callback_t)(void);

void wifi_init(void);
bool wifi_wait_connected(int timeout_ms);
bool wifi_is_connected(void);
void wifi_set_callbacks(wifi_callback_t on_connected, wifi_callback_t on_disconnected);
void wifi_get_ip_str(char *buf, size_t len);
/* Re-read NVS config and reconnect to a (possibly new) AP */
void wifi_reconnect_with_config(void);

#endif
