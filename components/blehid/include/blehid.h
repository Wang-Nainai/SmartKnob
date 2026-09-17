#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BLE HID 设备(仿 X-Knob Surface Dial)
 * 以标准 BLE HID 键鼠设备身份连接电脑, Windows/macOS 免驱:
 *   - 消费控制: 音量加减/静音/播放暂停/上一首/下一首
 *   - 鼠标滚轮/相对移动
 * 配对: 电脑蓝牙设置中搜索 "SmartKnob" 连接即可, 支持断线重连(绑定持久化)。 */

esp_err_t blehid_init(void);
bool blehid_is_connected(void);
/* Surface Dial 原生报告 (Win10 1903+ 系统级处理: 旋转/按压)
 * steps = 电机档数, 1 档 = 10 度拨盘旋转 (Windows 圆盘 UI 每档一次响应) */
void blehid_dial_rotate(int steps);
void blehid_dial_button(bool down);
/* 主动断开当前 HID 连接并重新开始广播 */
void blehid_disconnect(void);
/* 清除所有绑定(设备端解除配对; 电脑侧需重新配对) */
void blehid_unpair_all(void);

/* 消费控制: 发送一次按下+释放 (Consumer usage code, 如 0xE9=音量+) */
void blehid_consumer_send(uint16_t usage);

/* 鼠标滚轮, wheel = ±1 格 */
void blehid_mouse_scroll(int8_t wheel);

/* 鼠标相对移动(预留) */
void blehid_mouse_move(int8_t dx, int8_t dy);

/* 常用消费控制 usage */
#define HID_CONSUMER_VOLUME_UP      0xE9
#define HID_CONSUMER_VOLUME_DOWN    0xEA
#define HID_CONSUMER_MUTE           0xE2
#define HID_CONSUMER_PLAY_PAUSE     0xCD
#define HID_CONSUMER_SCAN_NEXT      0xB5
#define HID_CONSUMER_SCAN_PREV      0xB6

#ifdef __cplusplus
}
#endif