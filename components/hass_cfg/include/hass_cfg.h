#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 * hass_cfg -- 智能家居设备槽位配置 (Web 表单写入, NVS 持久化)
 *
 * 存储: NVS namespace "webcfg", key "hass_devices", 值为行文本:
 *   <名称>,<类型>\n<名称>,<类型>\n...
 *   类型: "灯" 或 "空调" (也接受 light / ac)
 * 上限 HASS_MAX_DEVICES 台; 空配置/解析失败时回退默认 4 台。
 * ============================================================ */

#define HASS_MAX_DEVICES 6
#define HASS_NAME_MAX_BYTES 19   /* 6 个 CJK + NUL */

enum {
    HASS_TYPE_LIGHT = 0,
    HASS_TYPE_AC = 1,
};

typedef struct {
    char name[HASS_NAME_MAX_BYTES];   /* UTF-8 显示名 */
    uint8_t type;                     /* HASS_TYPE_* */
} hass_device_cfg_t;

/* 读出设备列表; 返回设备数 (>=1, 解析失败回退默认 4 台) */
int hass_cfg_load(hass_device_cfg_t *out, int max);

/* 保存原始行文本 ("名称,类型" 每行一条); 返回实际解析到的设备数,
 * 0 条视为非法不写入并返回 0. 写入方负责在保存后触发 mqtt 重连重发发现 */
bool hass_cfg_save(const char *lines);
