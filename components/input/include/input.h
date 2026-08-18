#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 旋钮输入事件类型 */
typedef enum {
    KNOB_EVENT_ROTATE = 0,  /* 旋转: steps>0 顺时针, steps<0 逆时针 */
} knob_event_type_t;

typedef struct {
    knob_event_type_t type;
    int32_t steps;          /* 相对上一事件的位置增量(已合并同一采样周期的多档) */
    uint32_t timestamp_ms;  /* 事件发生时刻(esp_timer 毫秒) */
} knob_event_t;

/* 初始化 input 任务(轮询 motor 位置快照 → 事件队列) */
esp_err_t knob_input_init(void);

/* 非阻塞取一个事件; 队列为空返回 false */
bool knob_input_get_event(knob_event_t *evt);

/* 清除累积手势/残留事件, 并重新同步基准位置。
 * 必须在页面切换/模式切换后调用, 防止旧输入泄漏到新页面。 */
void knob_input_reset(void);

#ifdef __cplusplus
}
#endif