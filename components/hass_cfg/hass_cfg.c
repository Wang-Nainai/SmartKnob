#include "hass_cfg.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define TAG "hass_cfg"

#define HASS_CFG_NS "webcfg"
#define HASS_CFG_KEY "hass_devices"

static const hass_device_cfg_t s_defaults[HASS_MAX_DEVICES] = {
    { "\xE5\x8D\xA7\xE5\xAE\xA4\xE7\x81\xAF", HASS_TYPE_LIGHT },   /* 卧室灯 */
    { "\xE5\xAE\xA2\xE5\x8E\x85\xE7\x81\xAF", HASS_TYPE_LIGHT },   /* 客厅灯 */
    { "\xE8\xBF\x87\xE9\x81\x93\xE7\x81\xAF", HASS_TYPE_LIGHT },   /* 过道灯 */
    { "\xE5\x8D\xA7\xE5\xAE\xA4\xE7\xA9\xBA\xE8\xB0\x83", HASS_TYPE_AC }, /* 卧室空调 */
};
#define HASS_DEFAULT_NUM 4

static uint8_t parse_type(const char *t)
{
    while (*t == ' ' || *t == '\t') t++;
    if (!strcmp(t, "\xE7\x81\xAF") || !strcmp(t, "light")) {          /* 灯 */
        return HASS_TYPE_LIGHT;
    }
    if (!strcmp(t, "\xE7\xA9\xBA\xE8\xB0\x83") || !strcmp(t, "ac")) { /* 空调 */
        return HASS_TYPE_AC;
    }
    return 0xFF;
}

/* 找 UTF-8 全角逗号（EF BC 8C）或 ASCII 逗号, 返回指向分隔符的指针 */
static char *find_comma(char *s)
{
    for (char *p = s; *p; p++) {
        if (*p == ',') {
            return p;
        }
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBC &&
            (unsigned char)p[2] == 0x8C) {
            return p;
        }
    }
    return NULL;
}

static int parse_lines(const char *text, hass_device_cfg_t *out, int max)
{
    int n = 0;
    const char *p = text;
    char buf[HASS_NAME_MAX_BYTES + 12];
    while (n < max && p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof(buf)) {
            len = sizeof(buf) - 1;
        }
        memcpy(buf, p, len);
        buf[len] = 0;
        /* 去尾部 \r 与空格 */
        size_t bl = strlen(buf);
        while (bl > 0 && (buf[bl - 1] == '\r' || buf[bl - 1] == ' ')) {
            buf[--bl] = 0;
        }
        if (bl > 0) {
            char *comma = find_comma(buf);
            if (comma) {
                *comma = 0;
                const char *type_str = comma + 1;
                /* 名称按字节截断到 HASS_NAME_MAX_BYTES-1 并保证不切断 UTF-8 序列 */
                size_t nl = strlen(buf);
                while (nl > 0 && (unsigned char)buf[nl - 1] >= 0x80) {
                    /* 尾部 UTF-8 连续字节: 回退到序列起始 */
                    size_t k = nl;
                    while (k > 0 && ((unsigned char)buf[k - 1] & 0xC0) == 0x80) k--;
                    if ((unsigned char)buf[k - 1] >= 0x80) {
                        nl = k - 1;   /* 丢弃截断的多字节字符 */
                    } else {
                        break;
                    }
                }
                while (nl > 0 && buf[nl - 1] == ' ') {
                    buf[--nl] = 0;
                }
                if (nl > 0 && nl <= HASS_NAME_MAX_BYTES - 1) {
                    uint8_t type = parse_type(type_str);
                    if (type != 0xFF) {
                        memcpy(out[n].name, buf, nl + 1);
                        out[n].type = type;
                        n++;
                    }
                }
            }
        }
        p = eol ? eol + 1 : NULL;
    }
    return n;
}

static int load_blob(char *text, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(HASS_CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    size_t sz = size;
    esp_err_t err = nvs_get_str(h, HASS_CFG_KEY, text, &sz);
    nvs_close(h);
    return (err == ESP_OK && sz > 0) ? 1 : 0;
}

int hass_cfg_load(hass_device_cfg_t *out, int max)
{
    char text[256] = {0};
    if (load_blob(text, sizeof(text))) {
        int n = parse_lines(text, out, max);
        if (n > 0) {
            return n;
        }
        ESP_LOGW(TAG, "device config invalid, using defaults");
    }
    int n = max < HASS_DEFAULT_NUM ? max : HASS_DEFAULT_NUM;
    for (int i = 0; i < n; i++) {
        out[i] = s_defaults[i];
    }
    return n;
}

bool hass_cfg_save(const char *lines)
{
    if (!lines || !*lines) {
        return false;
    }
    hass_device_cfg_t tmp[HASS_MAX_DEVICES];
    int n = parse_lines(lines, tmp, HASS_MAX_DEVICES);
    if (n <= 0) {
        ESP_LOGW(TAG, "save rejected: no valid device lines");
        return false;
    }
    /* 只保留合法行的原文, 重新规范化写回 (去空行/超长行) */
    char norm[512];
    size_t off = 0;
    norm[0] = 0;
    for (int i = 0; i < n && off < sizeof(norm) - 8; i++) {
        int nw = snprintf(norm + off, sizeof(norm) - off, "%s,%s\n",
                          tmp[i].name, tmp[i].type == HASS_TYPE_AC ?
                          "\xE7\xA9\xBA\xE8\xB0\x83" : "\xE7\x81\xAF");
        if (nw < 0 || (size_t)nw >= sizeof(norm) - off) {
            break;
        }
        off += nw;
    }
    nvs_handle_t h;
    if (nvs_open(HASS_CFG_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_str(h, HASS_CFG_KEY, norm) == ESP_OK;
    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "saved %d devices", n);
    return ok;
}
