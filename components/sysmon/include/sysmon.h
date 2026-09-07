#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYSMON_MAX_TASKS 40   /* 24->40: 任务数超 24 时按优先级排序,
                               * 最低优先级的 IDLE0/IDLE1 会被截掉 -> CPU 反推 100% */

typedef struct {
    char name[16];
    uint8_t cpu;        /* 占用率 % (双核总容量的占比) */
    uint32_t stack_min; /* 栈剩余最小值 (字节, High Water Mark) */
    uint8_t state;      /* eTaskState: 0=Running 1=Ready 2=Blocked 3=Suspended 4=Deleted */
    int8_t core;        /* 运行核心, -1 未知 */
} sysmon_task_info_t;

typedef struct {
    uint8_t cpu0;            /* core0 使用率 % */
    uint8_t cpu1;            /* core1 使用率 % */
    uint8_t cpu_total;       /* 双核平均使用率 % */
    uint32_t int_free;       /* 内部 RAM 空闲 (字节) */
    uint32_t int_total;      /* 内部 RAM 总量 */
    uint32_t ps_free;        /* PSRAM 空闲 */
    uint32_t ps_total;       /* PSRAM 总量 */
    uint32_t min_free;       /* 历史最小空闲 (全堆) */
    uint8_t task_count;      /* 有效任务条数 */
    sysmon_task_info_t tasks[SYSMON_MAX_TASKS];
} sysmon_snapshot_t;

/* 创建监控任务(挂起等待, 不采样, 零开销) */
void sysmon_init(void);

/* 页面进入/退出: true 开始 1Hz 采样, false 挂起 */
void sysmon_set_enabled(bool on);

/* 读取最新快照(内部临界区拷贝, 可在 LVGL 任务调用) */
void sysmon_get_snapshot(sysmon_snapshot_t *out);

#ifdef __cplusplus
}
#endif