#include "wifi.h"
#include "app_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_event;
static int s_retry_num = 0;
static wifi_callback_t s_on_connected = NULL;
static wifi_callback_t s_on_disconnected = NULL;
static esp_ip4_addr_t s_ip_addr;

/* SoftAP 配网回退状态 (跨任务读: 事件任务写/LVGL 读, 用 volatile) */
static volatile bool s_ap_active = false;
static char s_ap_ssid[33] = "SmartKnob";
static esp_netif_t *s_ap_netif = NULL;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_num++;
        if (s_retry_num == 1 || s_retry_num % 5 == 0) {
            ESP_LOGI(TAG, "STA reconnect attempt %d", s_retry_num);
        }
        app_state_set_wifi(false);
        esp_wifi_connect();   /* 无限重连: 断网自愈, AP 配网页依赖持续重试 */
        if (s_on_disconnected) s_on_disconnected();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        s_ip_addr = event->ip_info.ip;
        ESP_LOGI(TAG, "connected, IP: " IPSTR, IP2STR(&s_ip_addr));
        s_retry_num = 0;
        app_state_set_wifi(true);
        /* 关联完成后再强制一次: 省电模式会让 AP 缓存/丢弃入站流量,
         * 导致 Web 管理页打不开、UDP 应答丢失 */
        esp_err_t ps_rc = esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI(TAG, "wifi ps off rc=%d", ps_rc);
        xEventGroupSetBits(s_wifi_event, WIFI_CONNECTED_BIT);
        if (s_ap_active) {
            wifi_ap_fallback_stop();   /* 配网成功: 自动关闭热点 */
        }
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

/* 本地读取网页配置(NVS "webcfg" 命名空间), 避免 wifi->webcfg 循环依赖 */
static void wifi_read_cfg(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    ssid[0] = 0;
    pass[0] = 0;
    nvs_handle_t h;
    if (nvs_open("webcfg", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = ssid_len;
        nvs_get_str(h, "wifi_ssid", ssid, &sz);
        sz = pass_len;
        nvs_get_str(h, "wifi_pass", pass, &sz);
        nvs_close(h);
    }
    if (!ssid[0]) {
        strncpy(ssid, CONFIG_ESP_WIFI_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = 0;
    }
    if (!pass[0]) {
        strncpy(pass, CONFIG_ESP_WIFI_PASS, pass_len - 1);
        pass[pass_len - 1] = 0;
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
    wifi_read_cfg(ssid, sizeof(ssid), pass, sizeof(pass));
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    /* AP 配网期间保持 APSTA, 避免杀掉热点 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA));
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
    s_ap_netif = esp_netif_create_default_wifi_ap();   /* 配网回退用, 平时不启用 */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    wifi_apply_config();
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 关闭 modem 省电: 默认省电模式下入站 TCP(SYN)常被漏收,
     * 导致 Web 管理页打不开/响应迟钝; 旋钮设备功耗不敏感 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
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

/* ---------------- SoftAP 配网回退 ---------------- */

void wifi_ap_fallback_start(void)
{
    if (s_ap_active) {
        return;
    }
    if (wifi_is_connected()) {
        return;   /* 已连上 STA, 无需热点 */
    }

    /* 热点名带 MAC 尾字节, 多设备同场不撞名 */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "SmartKnob-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = {
        .ap = {
            .channel = 6,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap_config.ap.ssid, s_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(s_ap_ssid);

    if (strlen(CONFIG_WIFI_AP_PASSWORD) > 0) {
        strncpy((char *)ap_config.ap.password, CONFIG_WIFI_AP_PASSWORD, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    /* 先置标志再动硬件: GOT_IP 事件若在配置期间到达, stop 路径能正确执行 */
    app_state_set_ap(true, s_ap_ssid, "192.168.4.1");
    s_ap_active = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_LOGW(TAG, "AP fallback ON: ssid=%s ip=192.168.4.1 (web config)", s_ap_ssid);
    /* webcfg HTTP 服务由 main 在调用本函数后启动(httpd 绑定 0.0.0.0,
     * AP/STA 网段均可达); 此处不再直接调用以避免 wifi->webcfg 循环依赖 */
    /* 双保险: 配置期间 STA 恰好连上则立即收掉热点 */
    if (wifi_is_connected()) {
        wifi_ap_fallback_stop();
        return;
    }
    /* APSTA 模式切换会重新触发 STA_START -> event_handler 继续 esp_wifi_connect() */
}

void wifi_ap_fallback_stop(void)
{
    if (!s_ap_active) {
        return;
    }
    s_ap_active = false;
    app_state_set_ap(false, NULL, NULL);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_LOGI(TAG, "AP fallback OFF (STA connected)");
}

bool wifi_ap_is_active(void)
{
    return s_ap_active;
}

void wifi_ap_get_ip_str(char *buf, int buflen)
{
    esp_netif_ip_info_t info = {0};
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK && info.ip.addr != 0) {
        snprintf(buf, buflen, IPSTR, IP2STR(&info.ip));
    } else {
        snprintf(buf, buflen, "192.168.4.1");
    }
}

const char *wifi_ap_get_ssid(void)
{
    return s_ap_ssid;
}
