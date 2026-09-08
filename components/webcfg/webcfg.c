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
#include "env_hist.h"
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
    "<meta name=\"theme-color\" content=\"#0b0d10\">"
    "<title>SmartKnob 管理台</title><style>"
    "*{box-sizing:border-box}"
    "body{background:#0b0d10;color:#f2f4f8;font-family:system-ui,-apple-system,'PingFang SC','Microsoft YaHei',sans-serif;max-width:520px;margin:0 auto;padding:0 14px 26px}"
    "header{background:linear-gradient(135deg,#0a84ff,#5e5ce6);margin:0 -14px 14px;padding:20px 18px 16px}"
    "header h1{font-size:21px;margin:0;letter-spacing:.5px}"
    "header .sub{font-size:12px;opacity:.85;margin-top:5px}"
    "h2{font-size:12px;color:#8b93a3;text-transform:uppercase;letter-spacing:.1em;margin:20px 2px 8px}"
    ".card{background:#151821;border:1px solid #232a36;border-radius:14px;padding:14px;margin-bottom:12px}"
    ".grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}"
    ".stat{background:#0f1218;border:1px solid #232a36;border-radius:10px;padding:9px 6px;text-align:center}"
    ".stat .l{font-size:11px;color:#8b93a3}"
    ".stat .v{font-size:14px;font-weight:600;margin-top:4px;min-height:18px}"
    ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px;vertical-align:1px}"
    ".on{background:#30d158;box-shadow:0 0 6px #30d15888}.off{background:#4a5160}"
    "label{display:block;font-size:12px;color:#8b93a3;margin:10px 0 4px}"
    "input,select{width:100%;padding:10px;border:1px solid #2a3140;border-radius:9px;background:#0f1218;color:#f2f4f8;font-size:14px}"
    "input:focus,select:focus{outline:none;border-color:#0a84ff}"
    "button{padding:10px 14px;border:0;border-radius:9px;background:#0a84ff;color:#fff;font-size:14px;cursor:pointer;font-weight:500}"
    "button:hover{filter:brightness(1.15)}button:active{transform:scale(.98)}"
    "button.sec{background:#242b38}button.warn{background:#ff453a}"
    ".row{display:flex;gap:8px;margin-top:10px}.row>*{flex:1;min-width:0}.row button{margin:0;white-space:nowrap;padding:10px 8px}"
    "#msg{font-size:13px;color:#6db2ff;background:#0f1a2b;border:1px solid #1b3350;border-radius:9px;padding:9px 12px;margin-top:12px;min-height:16px}"
    "progress{width:100%;height:8px;margin-top:10px}"
    ".tip{font-size:12px;color:#8b93a3;line-height:1.6;margin-top:8px}"
    "</style></head><body>"
    "<header><h1>SmartKnob 管理台</h1><div class=\"sub\" id=\"ver\">SmartKnob</div></header>"
    "<div class=\"card\"><div class=\"grid\">"
    "<div class=\"stat\"><div class=\"l\">WiFi</div><div class=\"v\"><span class=\"dot off\" id=\"d_wifi\"></span><span id=\"v_wifi\">-</span></div></div>"
    "<div class=\"stat\"><div class=\"l\">MQTT</div><div class=\"v\"><span class=\"dot off\" id=\"d_mqtt\"></span><span id=\"v_mqtt\">-</span></div></div>"
    "<div class=\"stat\"><div class=\"l\">蓝牙HID</div><div class=\"v\"><span class=\"dot off\" id=\"d_ble\"></span><span id=\"v_ble\">-</span></div></div>"
    "<div class=\"stat\"><div class=\"l\">CO\xE2\x82\x82</div><div class=\"v\" id=\"v_co2\">-</div></div>"
    "<div class=\"stat\"><div class=\"l\">温/湿</div><div class=\"v\" id=\"v_th\">-</div></div>"
    "<div class=\"stat\"><div class=\"l\">内存</div><div class=\"v\" id=\"v_heap\">-</div></div>"
    "</div>"
    "<div class=\"tip\" id=\"info\">-</div></div>"
    "<h2>\xE7\x8E\xAF\xE5\xA2\x83\xE8\xB6\x8B\xE5\x8A\xBF (24h)</h2>"
    "<div class=\"card\">"
    "<div class=\"row\">"
    "<button class=\"sec\" onclick=\"setEM(0)\">CO2</button>"
    "<button class=\"sec\" onclick=\"setEM(1)\">\xE6\xB8\xA9\xE5\xBA\xA6</button>"
    "<button class=\"sec\" onclick=\"setEM(2)\">\xE6\xB9\xBF\xE5\xBA\xA6</button>"
    "</div>"
    "<canvas id=\"envcv\" width=\"480\" height=\"150\" style=\"width:100%;background:#0f1218;border-radius:8px;margin-top:10px\"></canvas>"
    "<div class=\"tip\">\xE6\xAF\x8F 1 \xE5\x88\x86\xE9\x92\x9F\xE9\x87\x87\xE4\xB8\x80\xE4\xB8\xAA\xE7\x82\xB9\xEF\xBC\x8C\xE9\xBC\xA0\xE6\xA0\x87\xE6\x82\xAC\xE5\x81\x9C\xE6\x9F\xA5\xE7\x9C\x8B\xE6\x95\xB0\xE5\x80\xBC\xEF\xBC\x8C\xE5\x8E\x86\xE5\x8F\xB2\xE5\xAD\x98\xE4\xBA\x8E\xE5\x86\x85\xE5\xAD\x98\xEF\xBC\x8C\xE9\x87\x8D\xE5\x90\xAF\xE5\x90\x8E\xE6\xB8\x85\xE7\xA9\xBA</div></div>"
    "<h2>电机 / 手感</h2>"
    "<div class=\"card\">"
    "<select id=\"mmode\"></select>"
    "<div class=\"row\">"
    "<button onclick=\"api('motor_mode',mmode.value)\">应用模式</button>"
    "<button class=\"sec\" onclick=\"api('shake')\">振动测试</button>"
    "<button class=\"sec\" onclick=\"api('estop')\">急停</button>"
    "</div></div>"
    "<h2>屏幕 / LED</h2>"
    "<div class=\"card\">"
    "<div class=\"row\"><input type=\"number\" id=\"bri\" min=\"10\" max=\"100\" placeholder=\"亮度 %\">"
    "<button onclick=\"api('brightness',bri.value)\">设置</button></div>"
    "<div class=\"row\"><input type=\"number\" id=\"tmo\" min=\"0\" max=\"30\" placeholder=\"熄屏 (分)\">"
    "<button onclick=\"api('timeout',tmo.value)\">设置</button></div>"
    "<div class=\"row\"><input type=\"text\" id=\"ledc\" maxlength=\"6\" placeholder=\"LED 颜色 RRGGBB\" value=\"FF0000\">"
    "<button onclick=\"api('led',ledc.value)\">设置</button></div></div>"
    "<h2>蓝牙 HID · 电脑控制</h2>"
    "<div class=\"card\">"
    "<div style=\"font-size:13px\" id=\"ble_txt\">-</div>"
    "<div class=\"row\" style=\"margin-top:10px\">"
    "<button onclick=\"api('hid','vol_up')\">音量 +</button>"
    "<button onclick=\"api('hid','vol_dn')\">音量 −</button>"
    "</div>"
    "<div class=\"row\">"
    "<button class=\"sec\" onclick=\"api('hid','mute')\">静音</button>"
    "<button class=\"sec\" onclick=\"api('hid','play')\">播放 / 暂停</button>"
    "</div>"
    "<div class=\"row\">"
    "<button class=\"sec\" onclick=\"api('hid','next')\">下一首</button>"
    "<button class=\"sec\" onclick=\"api('hid','prev')\">上一首</button>"
    "</div>"
    "<div class=\"row\">"
    "<button class=\"sec\" onclick=\"api('hid','scr_up')\">滚轮 ↑</button>"
    "<button class=\"sec\" onclick=\"api('hid','scr_dn')\">滚轮 ↓</button>"
    "</div>"
    "<button class=\"sec\" style=\"margin-top:10px\" onclick=\"api('ble_disc')\">断开并重新广播</button>"
    "<div class=\"tip\">电脑蓝牙搜索 \"SmartKnob\" 配对后, S-Dial 页与本页均可控制音量/滚轮/媒体</div></div>"
    "<h2>网络 与 MQTT</h2>"
    "<div class=\"card\">"
    "<form method=\"post\" action=\"/save\" onsubmit=\"save(event)\">"
    "<label>WiFi 名称 (SSID)</label><input type=\"text\" name=\"wifi_ssid\" id=\"wifi_ssid\" maxlength=\"31\">"
    "<label>WiFi 密码</label><input type=\"password\" name=\"wifi_pass\" id=\"wifi_pass\" maxlength=\"63\">"
    "<label>MQTT 服务地址 (mqtt://host:port)</label><input type=\"text\" name=\"mqtt_uri\" id=\"mqtt_uri\" maxlength=\"127\">"
    "<label>MQTT 用户名</label><input type=\"text\" name=\"mqtt_user\" id=\"mqtt_user\" maxlength=\"63\">"
    "<label>MQTT 密码</label><input type=\"password\" name=\"mqtt_pass\" id=\"mqtt_pass\" maxlength=\"63\">"
    "<button type=\"submit\">保存并重连</button></form></div>"
    "<h2>固件升级 (OTA)</h2>"
    "<div class=\"card\">"
    "<input type=\"file\" id=\"fwfile\" accept=\".bin\" style=\"margin-bottom:10px\">"
    "<button onclick=\"upload()\">上传并烧录</button><progress id=\"pg\" value=\"0\" max=\"100\" hidden></progress>"
    "<div class=\"row\">"
    "<button class=\"sec\" onclick=\"restart()\">重启设备</button>"
    "<button class=\"warn\" onclick=\"factoryReset()\">恢复出厂</button>"
    "</div></div>"
    "<div id=\"msg\"></div>"
    "<script>"
    "function msg(t){const m=document.getElementById('msg');m.textContent=t;"
    "setTimeout(()=>{if(m.textContent===t)m.textContent=''},4000)}"
    "function api(a,v){fetch('/api/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'action='+a+'&value='+encodeURIComponent(v||'')}).then(r=>r.text()).then(t=>msg(t)).catch(()=>{})}"
    "function dot(id,on){document.getElementById(id).className='dot '+(on?'on':'off')}"
    "const modes=['\u65e0\u8fb9\u754c\u548c\u5236\u52a8','\u6709\u8fb9\u754c\u65e0\u5236\u52a8','\u591a\u5708\u65e0\u5236\u52a8','\u5f00\u5173\u6a21\u5f0f','\u81ea\u52a8\u56de\u4e2d','\u7cbe\u7ec6\u65e0\u5236\u52a8','\u7cbe\u7ec6\u6709\u5236\u52a8','\u7c97\u7565\u5f3a\u5236\u52a8','\u7c97\u7565\u5f31\u5236\u52a8','\u78c1\u6027\u5236\u52a8','\u56de\u4e2d\u5e26\u5236\u52a8','\u65e0\u8fb9\u754c\u68d8\u8f6e'];"
    "const sel=document.getElementById('mmode');"
    "modes.forEach((m,i)=>{const o=document.createElement('option');o.value=i;o.textContent=(i+1)+'. '+m;sel.appendChild(o)});"
    "function refresh(){fetch('/status').then(r=>r.json()).then(s=>{"
    "dot('d_wifi',s.wifi);dot('d_mqtt',s.mqtt);dot('d_ble',s.ble);"
    "document.getElementById('v_wifi').textContent=s.ap?'\u70ed\u70b9':(s.wifi?'\u5df2\u8fde':'\u672a\u8fde');"
    "document.getElementById('v_mqtt').textContent=s.mqtt?'\u5df2\u8fde':'\u672a\u8fde';"
    "document.getElementById('v_ble').textContent=s.ble?'\u5df2\u8fde':'\u672a\u8fde';"
    "document.getElementById('v_co2').textContent=s.co2+'ppm';"
    "document.getElementById('v_th').textContent=s.temp+'/'+s.rh;"
    "document.getElementById('v_heap').textContent=Math.round(s.heap/1024)+'K';"
    "document.getElementById('ble_txt').textContent=s.ble?'\u5df2\u8fde\u63a5\u7535\u8111':'\u672a\u8fde\u63a5';"
    "document.getElementById('ver').textContent='v'+s.version+' \u00b7 \u8fd0\u884c '+s.uptime+' \u00b7 \u4fe1\u53f7 '+s.rssi+'dBm';"
    "document.getElementById('info').textContent=(s.wifi?'\u5df2\u8fde\u63a5 '+s.ssid:'\u672a\u8fde\u63a5 WiFi')"
    "+(s.ap?' | \u914d\u7f51\u70ed\u70b9 '+s.ap_ssid+' ('+s.ap_ip+')':'')"
    "+' | \u7535\u673a\u6863\u4f4d '+s.motor_pos+' \u00b7 \u4eae\u5ea6 '+s.brightness+'%';"
    "document.getElementById('wifi_ssid').value=s.ssid||'';"
    "document.getElementById('mqtt_uri').value=s.mqtt_uri||'';"
    "document.getElementById('mqtt_user').value=s.mqtt_user||'';"
    "document.getElementById('mqtt_pass').value=s.mqtt_pass||'';"
    "if(!document.getElementById('bri').value)document.getElementById('bri').value=s.brightness;"
    "if(!document.getElementById('tmo').value)document.getElementById('tmo').value=s.timeout;"
    "}).catch(()=>{})}"
    "refresh();setInterval(refresh,5000);"
    "let em=0,ED=null,HV=-1;"
    "const ECFG=[['CO2 ppm',1,'#00c864'],['Temp C',10,'#3399ff'],['RH %',10,'#00c8b4']];"
    "function setEM(i){em=i;HV=-1;drawEnv()}"
    "function fT(ms){const d=new Date(ms);return ('0'+d.getHours()).slice(-2)+':'+('0'+d.getMinutes()).slice(-2)}"
    "function paint(){if(!ED)return;"
    "const cv=document.getElementById('envcv'),c=cv.getContext('2d');"
    "const W=cv.width,H=cv.height;c.clearRect(0,0,W,H);"
    "const arr=(em==0?ED.co2:em==1?ED.temp:ED.rh)||[];"
    "const nm=ECFG[em][0],dv=ECFG[em][1],col=ECFG[em][2];"
    "const L=38,R=8,T=8,B=16,PW=W-L-R,PH=H-T-B;"
    "const iv=(ED.iv_s||60)*1000,now=Date.now(),span=24*3600*1000,t0=now-span;"
    "const X=i=>L+(now-(arr.length-1-i)*iv-t0)/span*PW;"
    "c.strokeStyle='#232a36';c.lineWidth=1;c.beginPath();"
    "for(let i=0;i<5;i++){c.moveTo(L,T+i*PH/4);c.lineTo(W-R,T+i*PH/4)}c.stroke();"
    "c.fillStyle='#8b93a3';c.font='10px sans-serif';"
    "if(arr.length){"
    "let mn=Math.min(...arr),mx=Math.max(...arr);if(mx===mn)mx=mn+dv;"
    "const pad=Math.max((mx-mn)*0.12,dv);const lo=mn-pad,hi=mx+pad;"
    "const Y=v=>T+(hi-v)/(hi-lo)*PH;"
    "const f=v=>(v/dv).toFixed(dv>1?1:0);"
    "c.textAlign='left';c.fillText(f(hi),2,T+4);c.fillText(f((lo+hi)/2),2,T+PH/2+3);c.fillText(f(lo),2,T+PH);"
    "c.strokeStyle=col;c.lineWidth=1.6;c.beginPath();"
    "arr.forEach((v,i)=>{i?c.lineTo(X(i),Y(v)):c.moveTo(X(0),Y(arr[0]))});c.stroke();"
    "c.fillStyle='#8b93a3';c.textAlign='center';"
    "for(let k=0;k<5;k++){c.fillText(fT(t0+k*span/4),L+k*PW/4,H-4)}"
    "if(HV>=0&&HV<arr.length){const vx=X(HV),vy=Y(arr[HV]);"
    "c.strokeStyle='#88889c';c.setLineDash([4,3]);c.lineWidth=1;c.beginPath();"
    "c.moveTo(vx,T);c.lineTo(vx,T+PH);c.moveTo(L,vy);c.lineTo(W-R,vy);c.stroke();c.setLineDash([]);"
    "c.fillStyle=col;c.beginPath();c.arc(vx,vy,3,0,6.3);c.fill();"
    "const txt=fT(now-(arr.length-1-HV)*iv)+'  '+(arr[HV]/dv);"
    "c.font='11px sans-serif';const tw=c.measureText(txt).width+10;"
    "let bx=vx+6;if(bx+tw>W-R)bx=vx-6-tw;const by=Math.max(T,vy-24);"
    "c.fillStyle='rgba(20,24,33,0.95)';c.fillRect(bx,by,tw,18);"
    "c.strokeStyle=col;c.strokeRect(bx,by,tw,18);"
    "c.fillStyle='#f2f4f8';c.textAlign='left';c.fillText(txt,bx+5,by+13)"
    "}else{"
    "c.textAlign='right';c.fillText(nm,W-R,T+4)"
    "}"
    "}else{c.textAlign='center';c.fillText(nm+' -',W/2,H/2)}"
    "}"
    "function drawEnv(){fetch('/api/envhist').then(r=>r.json()).then(h=>{ED=h;paint()}).catch(()=>{})}"
    "window.addEventListener('load',()=>{const cv=document.getElementById('envcv');"
    "cv.addEventListener('mousemove',e=>{if(!ED)return;"
    "const r=cv.getBoundingClientRect();const x=(e.clientX-r.left)*(cv.width/r.width);"
    "const iv=(ED.iv_s||60)*1000,now=Date.now(),span=24*3600*1000,t0=now-span;"
    "const series=(em==0?ED.co2:em==1?ED.temp:ED.rh)||[];const n=series.length;"
    "if(!n)return;"
    "const t=t0+(x-38)/(480-38-8)*span;"
    "const i=Math.round((t-(now-(n-1)*iv))/iv);"
    "const ni=Math.max(0,Math.min(n-1,i));"
    "if(ni!=HV){HV=ni;paint()}});"
    "cv.addEventListener('mouseleave',()=>{if(HV!=-1){HV=-1;paint()}});"
    "});"
    "drawEnv();setInterval(drawEnv,30000);"
    "function save(ev){ev.preventDefault();const f=new FormData(ev.target);"
    "fetch('/save',{method:'POST',body:new URLSearchParams(f)}).then(r=>r.text()).then(t=>{"
    "msg(t);setTimeout(()=>location.reload(),3000)}).catch(()=>{})}"
    "function upload(){const f=document.getElementById('fwfile').files[0];if(!f){return}"
    "const pg=document.getElementById('pg');pg.hidden=false;const x=new XMLHttpRequest();"
    "x.open('POST','/ota');x.upload.onprogress=e=>{if(e.lengthComputable)pg.value=e.loaded/e.total*100};"
    "x.onload=()=>{msg(x.responseText)};x.send(f)}"
    "function restart(){fetch('/restart',{method:'POST'});msg('\u91cd\u542f\u4e2d...')}"
    "function factoryReset(){if(!confirm('\u786e\u5b9a\u6e05\u9664\u5168\u90e8\u914d\u7f6e(WiFi/MQTT/\u4eae\u5ea6\u7b49)\u5e76\u91cd\u542f?'))return;"
    "fetch('/factory_reset',{method:'POST'});msg('\u5df2\u6e05\u9664, \u91cd\u542f\u4e2d...')}"
    "</script></body></html>";

static esp_err_t handler_root(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET %s", req->uri);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_status(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET %s", req->uri);
    char buf[1024];
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

/* GET /api/envhist: 24h 环境历史 (5min/点, 温湿度为 x10 整数)
 * 体积 ~5KB, 用 chunked 发送避免大响应缓冲 */
static esp_err_t handler_envhist(httpd_req_t *req)
{
    ESP_LOGI(TAG, "HTTP GET %s", req->uri);
    httpd_resp_set_type(req, "application/json");
    char buf[256];
    int cnt = env_day_count();
    int n = snprintf(buf, sizeof(buf), "{\"iv_s\":300,\"n\":%d,\"co2\":[", cnt);
    httpd_resp_send_chunk(req, buf, n);
    for (int i = 0; i < cnt; i++) {
        n = snprintf(buf, sizeof(buf), "%u%s", env_day_co2_at(i),
                     i + 1 < cnt ? "," : "");
        httpd_resp_send_chunk(req, buf, n);
    }
    httpd_resp_send_chunk(req, "],\"temp\":[", -1);
    for (int i = 0; i < cnt; i++) {
        n = snprintf(buf, sizeof(buf), "%d%s", env_day_temp_x10_at(i),
                     i + 1 < cnt ? "," : "");
        httpd_resp_send_chunk(req, buf, n);
    }
    httpd_resp_send_chunk(req, "],\"rh\":[", -1);
    for (int i = 0; i < cnt; i++) {
        n = snprintf(buf, sizeof(buf), "%d%s", env_day_rh_x10_at(i),
                     i + 1 < cnt ? "," : "");
        httpd_resp_send_chunk(req, buf, n);
    }
    httpd_resp_send_chunk(req, "]}", -1);
    httpd_resp_send_chunk(req, NULL, 0);   /* 结束块 */
    return ESP_OK;
}

/* ---------------- 设备控制 API (POST /api/set, body: action=&value=) ---------------- */

static int hex2(const char *s)
{
    return (int)strtol(s, NULL, 16);
}

static esp_err_t handler_api_set(httpd_req_t *req)
{
    display_notify_activity();   /* web 控制也算用户活动, 重置熄屏计时 */
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
    } else if (!strcmp(action, "hid")) {
        /* 网页遥控电脑: 通过 BLE HID 转发消费控制/滚轮 */
        if (!blehid_is_connected()) {
            msg = "BLE HID not connected";
        } else if (!strcmp(value, "vol_up")) {
            blehid_consumer_send(HID_CONSUMER_VOLUME_UP);
            msg = "volume up";
        } else if (!strcmp(value, "vol_dn")) {
            blehid_consumer_send(HID_CONSUMER_VOLUME_DOWN);
            msg = "volume down";
        } else if (!strcmp(value, "mute")) {
            blehid_consumer_send(HID_CONSUMER_MUTE);
            msg = "mute toggle";
        } else if (!strcmp(value, "play")) {
            blehid_consumer_send(HID_CONSUMER_PLAY_PAUSE);
            msg = "play/pause";
        } else if (!strcmp(value, "next")) {
            blehid_consumer_send(HID_CONSUMER_SCAN_NEXT);
            msg = "next track";
        } else if (!strcmp(value, "prev")) {
            blehid_consumer_send(HID_CONSUMER_SCAN_PREV);
            msg = "prev track";
        } else if (!strcmp(value, "scr_up")) {
            blehid_mouse_scroll(2);
            msg = "scroll up";
        } else if (!strcmp(value, "scr_dn")) {
            blehid_mouse_scroll(-2);
            msg = "scroll down";
        } else {
            msg = "unknown hid cmd";
        }
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
    /* coex + BLE HID 并存时出方向很慢, 5s 默认值会把 10KB 页面
     * 发送超时(EAGAIN); 放宽到 20s, 慢链路也能完成 */
    cfg.send_wait_timeout = 20;
    cfg.recv_wait_timeout = 20;

    esp_err_t hs_err = httpd_start(&s_server, &cfg);
    if (hs_err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start HTTP server: %s (0x%x)",
                 esp_err_to_name(hs_err), (unsigned)hs_err);
        portENTER_CRITICAL(&s_mux);
        s_starting = false;
        portEXIT_CRITICAL(&s_mux);
        return;
    }

    httpd_uri_t uris[] = {
        {.uri = "/",      .method = HTTP_GET,  .handler = handler_root,     .user_ctx = NULL},
        {.uri = "/status",.method = HTTP_GET,  .handler = handler_status,   .user_ctx = NULL},
        {.uri = "/api/envhist",.method = HTTP_GET,.handler = handler_envhist,.user_ctx = NULL},
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