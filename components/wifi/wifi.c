#include "wifi.h"
#include "webcfg.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include <string.h>

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_event;
static int s_retry_num = 0;
static wifi_callback_t s_on_connected = NULL;
static wifi_callback_t s_on_disconnected = NULL;
static esp_ip4_addr_t s_ip_addr;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < CONFIG_ESP_WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry %d", s_retry_num);
        } else {
            xEventGroupSetBits(s_wifi_event, WIFI_FAIL_BIT);
        }
        if (s_on_disconnected) s_on_disconnected();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_ip_addr = event->ip_info.ip;
        ESP_LOGI(TAG, "connected, IP: " IPSTR, IP2STR(&s_ip_addr));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event, WIFI_CONNECTED_BIT);
        if (s_on_connected) s_on_connected();
    }
}

void wifi_get_ip_str(char *buf, size_t len)
{
    if (s_ip_addr.addr != 0) {
        snprintf(buf, len, IPSTR, IP2STR(&s_ip_addr));
    } else {
        snprintf(buf, len, "0.0.0.0");
    }
}

static void wifi_apply_config(void)
{
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    char ssid[33] = {0};
    char pass[65] = {0};
    webcfg_get_str("wifi_ssid", ssid, sizeof(ssid), CONFIG_ESP_WIFI_SSID);
    webcfg_get_str("wifi_pass", pass, sizeof(pass), CONFIG_ESP_WIFI_PASS);
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "connecting to SSID: %s", ssid);
}

void wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_apply_config();
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_reconnect_with_config(void)
{
    wifi_apply_config();
    xEventGroupClearBits(s_wifi_event, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    esp_wifi_disconnect();
    esp_wifi_connect();
}

bool wifi_wait_connected(int timeout_ms)
{
    if (!s_wifi_event) return false;
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_is_connected(void)
{
    if (!s_wifi_event) return false;
    EventBits_t bits = xEventGroupGetBits(s_wifi_event);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_set_callbacks(wifi_callback_t on_connected, wifi_callback_t on_disconnected)
{
    s_on_connected = on_connected;
    s_on_disconnected = on_disconnected;
}
