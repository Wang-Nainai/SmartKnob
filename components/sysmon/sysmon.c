#include "sysmon.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

/* 系统监控: 独立低优先级任务, 1Hz 采样 FreeRTOS 运行时统计 + 堆水位。
 * - 页面未开启时任务挂起(事件组等待), 零 CPU 开销
 * - 全部使用静态缓冲, 无动态内存分配
 * - 输出快照用临界区保护, UI 任务可安全拷贝 */

#define TAG "sysmon"

#define TASK_ARRAY_MAX   SYSMON_MAX_TASKS
#define SAMPLE_PERIOD_MS 1000
/* uxTaskGetSystemState 挂起双核调度并逐任务扫描栈水印, 多任务+coex 场景
 * 下长时间占用双核 spinlock (实测触发 IWDT panic 重启) —— 每 N 轮才重扫描
 * 一次, 其余轮次复用上一轮任务表渲染 */
#define TASK_SCAN_DIV    5

typedef struct {
    TaskHandle_t handle;
    uint32_t last_runtime;
} prev_slot_t;

static TaskStatus_t s_states[TASK_ARRAY_MAX];
static prev_slot_t s_prev[TASK_ARRAY_MAX];
static UBaseType_t s_prev_count = 0;
static uint32_t s_last_total = 0;

static sysmon_snapshot_t s_snap;
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_enabled = false;
static TaskHandle_t s_task_handle = NULL;

void sysmon_get_snapshot(sysmon_snapshot_t *out)
{
    if (!out) {
        return;
    }
    portENTER_CRITICAL(&s_snap_mux);
    *out = s_snap;
    portEXIT_CRITICAL(&s_snap_mux);
}

static void sample_once(void)
{
    static int scan_div = 0;
    if ((scan_div++ % TASK_SCAN_DIV) != 0 && s_prev_count > 0) {
        /* 非重扫描轮: 保留上一轮任务表, 只重算内存字段 */
        uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        uint32_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        uint32_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        uint32_t min_free = esp_get_minimum_free_heap_size();
        portENTER_CRITICAL(&s_snap_mux);
        s_snap.int_free = int_free;
        s_snap.int_total = int_total;
        s_snap.ps_free = ps_free;
        s_snap.ps_total = ps_total;
        s_snap.min_free = min_free;
        portEXIT_CRITICAL(&s_snap_mux);
        return;
    }

    uint32_t total = 0;
    UBaseType_t n = uxTaskGetSystemState(s_states, TASK_ARRAY_MAX, &total);
    if (n == 0) {
        return;
    }

    uint32_t dtotal = total - s_last_total;
    /* ccount 32 位计数器 @160MHz 约 26.8s 回绕一次: 回绕轮的增量是天文数字,
     * 跳过该轮并重新同步基线, 避免所有百分比算出 0 */
    if (dtotal > 1000000000u) {
        for (UBaseType_t i = 0; i < n && i < TASK_ARRAY_MAX; i++) {
            s_prev[i].handle = s_states[i].xHandle;
            s_prev[i].last_runtime = s_states[i].ulRunTimeCounter;
        }
        s_prev_count = n;
        s_last_total = total;
        return;
    }
    uint32_t d_idle0 = 0, d_idle1 = 0;

    /* 按 xTaskHandle 匹配上一轮, 计算各任务运行时增量 */
    portENTER_CRITICAL(&s_snap_mux);
    s_snap.task_count = 0;
    for (UBaseType_t i = 0; i < n && s_snap.task_count < SYSMON_MAX_TASKS; i++) {
        TaskStatus_t *st = &s_states[i];
        uint32_t drt = 0;
        for (UBaseType_t j = 0; j < s_prev_count; j++) {
            if (s_prev[j].handle == st->xHandle) {
                uint32_t last = s_prev[j].last_runtime;
                drt = (st->ulRunTimeCounter >= last) ? (st->ulRunTimeCounter - last) : 0;
                break;
            }
        }
        s_prev[i].handle = st->xHandle;
        s_prev[i].last_runtime = st->ulRunTimeCounter;

        sysmon_task_info_t *ti = &s_snap.tasks[s_snap.task_count];
        strncpy(ti->name, st->pcTaskName, sizeof(ti->name) - 1);
        ti->name[sizeof(ti->name) - 1] = 0;
        ti->cpu = (dtotal > 0) ? (uint8_t)((uint64_t)drt * 100 / dtotal) : 0;
        ti->stack_min = (uint32_t)st->usStackHighWaterMark * sizeof(StackType_t);
        ti->state = (uint8_t)st->eCurrentState;
#if CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID
        ti->core = (int8_t)st->xCoreID;
#else
        ti->core = -1;
#endif
        s_snap.task_count++;

        if (!strcmp(ti->name, "IDLE0")) d_idle0 = drt;
        if (!strcmp(ti->name, "IDLE1")) d_idle1 = drt;
    }
    s_prev_count = n;

    if (dtotal == 0) {
        dtotal = 1;
    }
    uint32_t cap = dtotal / 2;
    uint8_t cpu0 = (cap > 0) ? (uint8_t)(100 - (uint64_t)d_idle0 * 100 / cap) : 0;
    uint8_t cpu1 = (cap > 0) ? (uint8_t)(100 - (uint64_t)d_idle1 * 100 / cap) : 0;
    if (cpu0 > 100) cpu0 = 100;
    if (cpu1 > 100) cpu1 = 100;

    /* 堆采样移出临界区: heap_caps_* 内部取堆互斥锁, 若在关中断临界区内
     * 拿锁, 持锁任务恰好被切走时将死锁(本任务中断已关) */
    uint32_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t int_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t ps_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t ps_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    uint32_t min_free = esp_get_minimum_free_heap_size();

    portENTER_CRITICAL(&s_snap_mux);
    s_snap.cpu0 = cpu0;
    s_snap.cpu1 = cpu1;
    s_snap.cpu_total = (cpu0 + cpu1) / 2;
    s_snap.int_free = int_free;
    s_snap.int_total = int_total;
    s_snap.ps_free = ps_free;
    s_snap.ps_total = ps_total;
    s_snap.min_free = min_free;
    portEXIT_CRITICAL(&s_snap_mux);

    s_last_total = total;
}

static void sysmon_task(void *arg)
{
    /* 第一轮只记录基线(增量需要两轮) */
    uint32_t total = 0;
    uxTaskGetSystemState(s_states, TASK_ARRAY_MAX, &total);
    s_last_total = total;
    s_prev_count = 0;

    while (1) {
        if (!s_enabled) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* 挂起等待页面开启 */
            /* 重新使能时重置基线, 避免陈旧数据产生巨值 */
            uxTaskGetSystemState(s_states, TASK_ARRAY_MAX, &total);
            s_last_total = total;
            s_prev_count = 0;
            vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }
        sample_once();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void sysmon_set_enabled(bool on)
{
    if (on == s_enabled) {
        return;
    }
    s_enabled = on;
    if (on && s_task_handle) {
        xTaskNotifyGive(s_task_handle);
    }
}

void sysmon_init(void)
{
    if (xTaskCreatePinnedToCore(sysmon_task, "sysmon", 4096, NULL, 1, &s_task_handle, 0) != pdPASS) {
        ESP_LOGE(TAG, "failed to create sysmon task");
    }
}