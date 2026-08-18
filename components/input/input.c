#include "input.h"
#include "motor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "input";

#ifndef CONFIG_KNOB_INPUT_POLL_MS
#define CONFIG_KNOB_INPUT_POLL_MS 5
#endif

#ifndef CONFIG_KNOB_INPUT_QUEUE_LEN
#define CONFIG_KNOB_INPUT_QUEUE_LEN 16
#endif

static QueueHandle_t s_evt_queue = NULL;

/* 需要重新同步基准位置(页面/模式切换后调用 knob_input_reset 置位) */
static volatile bool s_resync = false;

static void knob_input_task(void *arg)
{
    /* 等待电机就绪, 以读取有效位置 */
    while (!motor_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    int32_t last_pos = motor_get_position();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_KNOB_INPUT_POLL_MS));

        if (s_resync) {
            s_resync = false;
            xQueueReset(s_evt_queue);
            last_pos = motor_get_position();
            continue;
        }

        int32_t pos = motor_get_position();
        int32_t d = pos - last_pos;
        if (d == 0) {
            continue;
        }
        last_pos = pos;

        knob_event_t evt = {
            .type = KNOB_EVENT_ROTATE,
            .steps = d,
            .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        };
        if (xQueueSend(s_evt_queue, &evt, 0) != pdTRUE) {
            /* 队列满: 丢最旧保最新, 保证 UI 不滞后于手 */
            knob_event_t drop;
            xQueueReceive(s_evt_queue, &drop, 0);
            xQueueSend(s_evt_queue, &evt, 0);
            ESP_LOGW(TAG, "event queue overflow, dropped oldest");
        }
    }
}

esp_err_t knob_input_init(void)
{
    if (s_evt_queue) {
        return ESP_OK;
    }
    s_evt_queue = xQueueCreate(CONFIG_KNOB_INPUT_QUEUE_LEN, sizeof(knob_event_t));
    if (!s_evt_queue) {
        ESP_LOGE(TAG, "failed to create event queue");
        return ESP_ERR_NO_MEM;
    }
    xTaskCreatePinnedToCore(knob_input_task, "knob_input", 2048, NULL, 1, NULL, 1);
    ESP_LOGI(TAG, "knob input task started (poll=%dms)", CONFIG_KNOB_INPUT_POLL_MS);
    return ESP_OK;
}

bool knob_input_get_event(knob_event_t *evt)
{
    if (!s_evt_queue) {
        return false;
    }
    return xQueueReceive(s_evt_queue, evt, 0) == pdTRUE;
}

void knob_input_reset(void)
{
    s_resync = true;
}