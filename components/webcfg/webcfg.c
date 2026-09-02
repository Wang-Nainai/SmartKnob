#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "webcfg.h"
#include "app_state.h"
#include "motor.h"
#include "blehid.h"
#include "display.h"
#include "led.h"
#include "esp_wifi.h"

static const char *TAG = "webcfg";

#define CFG_NAMESPACE "webcfg"
#define OTA_MAX_SIZE   0x300000  /* app 分区 3MB */

static webcfg_apply_cb_t s_apply_cb = NULL;
static httpd_handle_t s_server = NULL;

static volatile bool s_mqtt = false;
static char s_ip[16] = "0.0.0.0";

/* ---------------- NVS config ---------------- */

void webcfg_get_str(const char *key, char *buf, size_t len, const char *fallback)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = len;
        if (nvs_get_str(h, key, buf, &sz) != ESP_OK) {
            snprintf(buf, len, "%s", fallback);
        }
        nvs_close(h);
    } else {
        snprintf(buf, len, "%s", fallback);
    }
}

void webcfg_set_str(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (value && value[0]) {
        nvs_set_str(h, key, value);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

void webcfg_erase_all(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------------- runtime status ---------------- */

void webcfg_set_mqtt_connected(bool connected)
{
    s_mqtt = connected;
}

void webcfg_set_ip(const char *ip_str)
{
    snprintf(s_ip, sizeof(s_ip), "%s", ip_str);
}

/* i32 配置(NVS "webcfg" 命名空间, 与 UI 亮度/熄屏共用) */
void webcfg_set_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- URL decoding ---------------- */

static int url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t i = 0, o = 0;
    while (src[i] && o < dst_len - 1) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[o++] = ' ';
            i++;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = 0;
    return o;
}

/* ---------------- handlers ---------------- */

static const char page_html[] =
    "<!DOCTYPE html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>SmartKnob 管理</title><style>"
    "body{background:#0a0a0a;color:#eee;font-family:system-ui,sans-serif;max-width:480px;margin:0 auto;padding:16px}"
    "h1{font-size:20px;color:#0a84ff;margin:0 0 4px}h2{font-size:15px;color:#aaa;margin:18px 0 6px}"
    "label{display:block;font-size:13px;color:#888;margin:8px 0 3px}"
    "input[type=text],input[type=password],input[type=number],select{width:100%;box-sizing:border-box;padding:9px;border:1px solid #333;"
    "border-radius:6px;background:#151515;color:#eee;font-size:15px}"
    "button{width:100%;padding:11px;border:0;border-radius:6px;background:#0a84ff;color:#fff;font-size:15px;margin-top:14px}"
    "button.sec{background:#2a2a2a;margin-top:8px}"
    "button.warn{background:#c33}"
    ".row{display:flex;gap:8px}.row>*{flex:1}"
    "#status{font-size:13px;color:#aaa;background:#111;border-radius:6px;padding:10px;line-height:1.7;margin-top:12px}"
    "#msg{font-size:13px;color:#0a84ff;margin-top:10px;min-height:18px}"
    "progress{width:100%;height:10px;margin-top:10px}"
    "</style></head><body>"
    "<h1>SmartKnob 管理</h1><div id=\"status\">加载中...</div>"
    "<h2>电机 / 手感</h2>"
    "<div class=\"row\">"
    "<select id=\"mmode\"></select>"
    "<button style=\"margin:0\" onclick=\"api('motor_mode',document.getElementById('mmode').value)\">应用</button>"
    "</div>"
    "<div class=\"row\">"
    "<button style=\"margin-top:8px\" onclick=\"api('shake')\">振动测试</button>"
    "<button style=\"margin-top:8px;background:#2a2a2a\" onclick=\"api('estop')\">电机急停</button>"
    "</div>"
    "<h2>屏幕 / LED</h2>"
    "<div class=\"row\">"
    "<input type=\"number\" id=\"bri\" min=\"10\" max=\"100\" placeholder=\"亮度%\">"
    "<button style=\"margin:0\" onclick=\"api('brightness',document.getElementById('bri').value)\">设亮度</button>"
    "</div>"
    "<div class=\"row\">"
    "<input type=\"number\" id=\"tmo\" min=\"0\" max=\"30\" placeholder=\"熄屏(分)\">"
    "<button style=\"margin:0\" onclick=\"api('timeout',document.getElementById('tmo').value)\">设熄屏</button>"
    "</div>"
    "<div class=\"row\">"
    "<input type=\"text\" id=\"ledc\" placeholder=\"LED 颜色 RRGGBB, 如 FF0000\">"
    "<button style=\"margin:0\" onclick=\"api('led',document.getElementById('ledc').value)\">设LED</button>"
    "</div>"
    "<h2>蓝牙 HID (电脑控制)</h2>"
    "<div id=\"ble\">-</div>"
    "<button onclick=\"api('ble_disc')\">断开蓝牙并重新广播</button>"
    "<h2>网络 与 MQTT</h2>"
    "<form method=\"post\" action=\"/save\" onsubmit=\"save(event)\">"
    "<label>WiFi 名称 (SSID)</label><input type=\"text\" name=\"wifi_ssid\" id=\"wifi_ssid\" maxlength=\"31\">"
    "<label>WiFi 密码</label><input type=\"password\" name=\"wifi_pass\" id=\"wifi_pass\" maxlength=\"63\">"
    "<label>MQTT 服务地址 (mqtt://host:port)</label><input type=\"text\" name=\"mqtt_uri\" id=\"mqtt_uri\" maxlength=\"127\">"
    "<label>MQTT 用户名</label><input type=\"text\" name=\"mqtt_user\" id=\"mqtt_user\" maxlength=\"63\">"
    "<label>MQTT 密码</label><input type=\"password\" name=\"mqtt_pass\" id=\"mqtt_pass\" maxlength=\"63\">"
    "<button type=\"submit\">保存并重连</button></form>"
    "<div id=\"msg\"></div>"
    "<h2>固件升级 (OTA)</h2>"
    "<input type=\"file\" id=\"fwfile\" accept=\".bin\">"
    "<button onclick=\"upload()\">上传并烧录</button><progress id=\"pg\" value=\"0\" max=\"100\" hidden></progress>"
    "<button class=\"sec\" onclick=\"restart()\">重启设备</button>"
    "<h2>恢复出厂</h2>"
    "<button class=\"warn\" onclick=\"factoryReset()\">清除全部配置并重启</button>"
    "<script>"
    "function qs(){return Object.fromEntries(new URLSearchParams(location.search))}"
    "function msg(t){document.getElementById('msg').textContent=t;setTimeout(()=>{document.getElementById('msg').textContent=''},4000)}"
    "function api(a,v){fetch('/api/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'action='+a+'&value='+encodeURIComponent(v||'')}).then(r=>r.text()).then(t=>msg(t)).catch(()=>{})}"
    "const modes=['无边界和制动','有边界无制动','多圈无制动','开关模式','自动回中','精细无制动','精细有制动','粗略强制动','粗略弱制动','磁性制动','回中带制动'];"
    "const sel=document.getElementById('mmode');"
    "modes.forEach((m,i)=>{const o=document.createElement('option');o.value=i;o.textContent=i+1+'. '+m;sel.appendChild(o)});"
    "function refresh(){fetch('/status').then(r=>r.json()).then(s=>{"
    "document.getElementById('status').innerHTML='IP <b>'+s.ip+'</b> · WiFi <b>'+(s.wifi?'已连接':'未连接')+' ('+(s.rssi||0)+'dBm)</b>'"
    "+' · MQTT <b>'+(s.mqtt?'已连接':'未连接')+'</b><br>热点 <b>'+(s.ap? s.ap_ssid+' ('+s.ap_ip+')':'未开启')+'</b>'"
    "+' · 固件 v'+s.version+'<br>CO2 <b>'+s.co2+'ppm</b> · '+s.temp+'°C · '+s.rh+'%"
    "+' · 内存 <b>'+Math.round(s.heap/1024)+'KB</b><br>运行 '+s.uptime+' · 电机位置 <b>'+s.motor_pos+'</b>';"
    "document.getElementById('ble').textContent=s.ble?'已连接电脑':'未连接';"
    "document.getElementById('wifi_ssid').value=s.ssid||'';"
    "document.getElementById('mqtt_uri').value=s.mqtt_uri||'';"
    "document.getElementById('mqtt_user').value=s.mqtt_user||'';"
    "document.getElementById('mqtt_pass').value=s.mqtt_pass||'';"
    "if(!document.getElementById('bri').value)document.getElementById('bri').value=s.brightness;"
    "if(!document.getElementById('tmo').value)document.getElementById('tmo').value=s.timeout;"
    "}).catch(()=>{})}"
"refresh();setInterval(refresh,5000);"
    "function save(ev){ev.preventDefault();const f=new FormData(ev.target);"
    "fetch('/save',{method:'POST',body:new URLSearchParams(f)}).then(r=>r.text()).then(t=>{"
    "document.getElementById('msg').textContent=t;setTimeout(()=>location.reload(),3000)}).catch(()=>{})}"
    "function upload(){const f=document.getElementById('fwfile').files[0];if(!f){return}"
    "const pg=document.getElementById('pg');pg.hidden=false;const x=new XMLHttpRequest();"
    "x.open('POST','/ota');x.upload.onprogress=e=>{if(e.lengthComputable)pg.value=e.loaded/e.total*100};"
    "x.onload=()=>{document.getElementById('msg').textContent=x.responseText};x.send(f)}"
    "function restart(){fetch('/restart',{method:'POST'});msg('重启中...')}"
    "function factoryReset(){if(!confirm('确定清除全部配置(WiFi/MQTT/亮度等)并重启?'))return;"
    "fetch('/factory_reset',{method:'POST'});msg('已清除, 重启中...')}"
    "</script></body></html>";

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    char buf[768];
    const esp_app_desc_t *app = esp_app_get_description();
    uint32_t uptime_s = esp_timer_get_time() / 1000000ULL;
    char ssid[33] = {0}, mqtt_uri[128] = {0}, mqtt_user[64] = {0}, mqtt_pass[64] = {0};
    webcfg_get_str("wifi_ssid", ssid, sizeof(ssid), "");
    webcfg_get_str("mqtt_uri", mqtt_uri, sizeof(mqtt_uri), "");
    webcfg_get_str("mqtt_user", mqtt_user, sizeof(mqtt_user), "");
    webcfg_get_str("mqtt_pass", mqtt_pass, sizeof(mqtt_pass), "");
    app_env_t env;
    app_state_get_env(&env);

    /* rssi (STA 已连接时有效) */
    int rssi = 0;
    wifi_ap_record_t ap_info;
    if (app_state_get_wifi() && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }

    char ap_ssid[33] = {0}, ap_ip[16] = {0};
    bool ap = app_state_get_ap(ap_ssid, sizeof(ap_ssid), ap_ip, sizeof(ap_ip));

    int n = snprintf(buf, sizeof(buf),
                     "{\"ip\":\"%s\",\"ssid\":\"%s\",\"mqtt_uri\":\"%s\",\"mqtt_user\":\"%s\",\"mqtt_pass\":\"%s\","
                     "\"wifi\":%s,\"mqtt\":%s,\"ap\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
                     "\"ble\":%s,"
                     "\"co2\":%u,\"temp\":%.1f,\"rh\":%.1f,"
                     "\"motor_mode\":%d,\"motor_pos\":%ld,"
                     "\"brightness\":%d,\"timeout\":%d,"
                     "\"heap\":%u,\"rssi\":%d,"
                     "\"version\":\"%s\",\"uptime\":\"%ud %02uh %02um\"}",
                     s_ip, ssid, mqtt_uri, mqtt_user, mqtt_pass,
                     app_state_get_wifi() ? "true" : "false",
                     s_mqtt ? "true" : "false",
                     ap ? "true" : "false",
                     ap_ssid, ap_ip,
                     blehid_is_connected() ? "true" : "false",
                     env.co2_ppm, (double)env.temperature_c, (double)env.humidity_pct,
                     (int)motor_get_mode(), (long)motor_get_position(),
                     display_get_brightness(), display_get_screen_timeout() / 60,
                     (unsigned)esp_get_free_heap_size(), rssi,
                     app->version,
                     (unsigned int)(uptime_s / 86400), (unsigned int)((uptime_s % 86400) / 3600),
                     (unsigned int)((uptime_s % 3600) / 60));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* ---------------- 设备控制 API (POST /api/set, body: action=&value=) ---------------- */

static int hex2(const char *s)
{
    return (int)strtol(s, NULL, 16);
}

static esp_err_t handler_api_set(httpd_req_t *req)
{
    char body[256] = {0};
    int total = req->content_len;
    if (total <= 0 || total > (int)sizeof(body) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_FAIL;
    }
    if (httpd_req_recv(req, body, total) <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }

    char decoded[256];
    url_decode(body, decoded, sizeof(decoded));

    char action[32] = {0}, value[64] = {0};
    const char *pa = strstr(decoded, "action=");
    const char *pv = strstr(decoded, "value=");
    if (pa) {
        const char *e = strchr(pa + 7, '&');
        size_t n = e ? (size_t)(e - pa - 7) : strlen(pa + 7);
        if (n >= sizeof(action)) n = sizeof(action) - 1;
        memcpy(action, pa + 7, n);
    }
    if (pv) {
        const char *e = strchr(pv + 6, '&');
        size_t n = e ? (size_t)(e - pv - 6) : strlen(pv + 6);
        if (n >= sizeof(value)) n = sizeof(value) - 1;
        memcpy(value, pv + 6, n);
    }

    const char *msg = "OK";
    int v = (int)strtol(value, NULL, 0);

    if (!strcmp(action, "motor_mode")) {
        motor_set_mode((motor_mode_t)v, 0, 0);
        msg = "motor mode set";
    } else if (!strcmp(action, "shake")) {
        motor_shake(3, 40);
        msg = "motor shake";
    } else if (!strcmp(action, "estop")) {
        motor_disable();
        msg = "motor disabled (next mode change re-enables)";
    } else if (!strcmp(action, "brightness")) {
        if (v < 10) v = 10;
        if (v > 100) v = 100;
        display_set_brightness(v);
        webcfg_set_i32("brightness", v);
        msg = "brightness set";
    } else if (!strcmp(action, "timeout")) {
        if (v < 0) v = 0;
        if (v > 30) v = 30;
        display_set_screen_timeout(v * 60);
        webcfg_set_i32("timeout", v);
        msg = "screen timeout set";
    } else if (!strcmp(action, "led")) {
        /* value = RRGGBB */
        if (strlen(value) == 6) {
            led_set_color(hex2(value), hex2(value + 2), hex2(value + 4));
            msg = "led set";
        } else {
            msg = "led value must be RRGGBB";
        }
    } else if (!strcmp(action, "ble_disc")) {
        blehid_disconnect();
        msg = "BLE HID disconnecting";
    } else {
        msg = "unknown action";
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[1024];
    int total = req->content_len;
    if (total > (int)sizeof(body) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "payload too large");
        return ESP_FAIL;
    }
    int received = httpd_req_recv(req, body, total);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_FAIL;
    }
    body[received] = 0;

    char decoded[1024];
    url_decode(body, decoded, sizeof(decoded));

    /* copy value up to '&' separator */
    char val[128];
    const char *p;

#define GET_FIELD(field, key, offset) do { \
        p = strstr(decoded, field); \
        if (p) { \
            const char *v = p + offset; \
            const char *end = strchr(v, '&'); \
            size_t n = end ? (size_t)(end - v) : strlen(v); \
            if (n >= sizeof(val)) n = sizeof(val) - 1; \
            memcpy(val, v, n); \
            val[n] = 0; \
            webcfg_set_str(key, val); \
        } \
    } while (0)

    GET_FIELD("wifi_ssid=", "wifi_ssid", 10);
    GET_FIELD("wifi_pass=", "wifi_pass", 10);
    GET_FIELD("mqtt_uri=", "mqtt_uri", 9);
    GET_FIELD("mqtt_user=", "mqtt_user", 10);
    GET_FIELD("mqtt_pass=", "mqtt_pass", 10);
#undef GET_FIELD

    httpd_resp_sendstr(req, "配置已保存，正在重连...");
    ESP_LOGI(TAG, "config saved, applying");
    if (s_apply_cb) {
        s_apply_cb();
    }
    return ESP_OK;
}

static esp_err_t handler_ota(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle;
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > OTA_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad firmware size");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA start: %d bytes -> %s", req->content_len, update->label);
    if (esp_ota_begin(update, req->content_len, &ota_handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_FAIL;
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int recv = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (recv <= 0) {
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }
        if (esp_ota_write(ota_handle, buf, recv) != ESP_OK) {
            free(buf);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_FAIL;
        }
        remaining -= recv;
    }
    free(buf);

    if (esp_ota_end(ota_handle) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(update) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot partition failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "烧录完成，设备即将重启...");
    ESP_LOGI(TAG, "OTA success, rebooting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_restart(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_factory_reset(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "factory reset, rebooting");
    ESP_LOGW(TAG, "factory reset via web");
    webcfg_erase_all();   /* 清除 WiFi/MQTT/亮度/熄屏 全部配置 */
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

void webcfg_start(void)
{
    /* 可能被 app_main(AP 回退) 和 WiFi 事件任务(连上路由器)并发调用:
     * 临界区只保护 check+flag, httpd_start 本身在临界区外, 仅一方执行 */
    static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
    static bool s_starting = false;
    bool go = false;

    portENTER_CRITICAL(&s_mux);
    if (!s_server && !s_starting) {
        s_starting = true;
        go = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!go) {
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_WEBCFG_HTTP_PORT;
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server");
        portENTER_CRITICAL(&s_mux);
        s_starting = false;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    httpd_uri_t uris[] = {
        {.uri = "/",      .method = HTTP_GET,  .handler = handler_root,     .user_ctx = NULL},
        {.uri = "/status",.method = HTTP_GET,  .handler = handler_status,   .user_ctx = NULL},
        {.uri = "/save",  .method = HTTP_POST, .handler = handler_save,     .user_ctx = NULL},
        {.uri = "/api/set",.method = HTTP_POST,.handler = handler_api_set,  .user_ctx = NULL},
        {.uri = "/ota",   .method = HTTP_POST, .handler = handler_ota,      .user_ctx = NULL},
        {.uri = "/restart",.method = HTTP_POST,.handler = handler_restart,  .user_ctx = NULL},
        {.uri = "/factory_reset",.method = HTTP_POST,.handler = handler_factory_reset,.user_ctx = NULL},
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        if (httpd_register_uri_handler(s_server, &uris[i]) != ESP_OK) {
            ESP_LOGW(TAG, "failed to register uri %s", uris[i].uri);
        }
    }
    ESP_LOGI(TAG, "web config server ready on port %d", cfg.server_port);
}

void webcfg_set_apply_cb(webcfg_apply_cb_t cb)
{
    s_apply_cb = cb;
}