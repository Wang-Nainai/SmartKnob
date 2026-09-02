#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "led_strip_encoder.h"

#define LED_GPIO         48
#define LED_RESOLUTION   10000000

static const char *TAG = "led";

static rmt_channel_handle_t led_chan = NULL;
static rmt_encoder_handle_t led_encoder = NULL;
/* WiFi 事件任务 / MQTT 任务 / LVGL 任务(工厂页) / httpd 任务(web 控制)
 * 都可能并发调用, RMT 通道发送必须串行化 */
static SemaphoreHandle_t s_led_mux = NULL;

void led_init(void)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = LED_RESOLUTION,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));

    led_strip_encoder_config_t encoder_config = {
        .resolution = LED_RESOLUTION,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&encoder_config, &led_encoder));

    ESP_ERROR_CHECK(rmt_enable(led_chan));

    s_led_mux = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "initialized on GPIO %d", LED_GPIO);
}

void led_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led_mux) {
        return;   /* 未初始化 */
    }
    xSemaphoreTake(s_led_mux, portMAX_DELAY);
    uint8_t pixels[3];
    pixels[0] = g;
    pixels[1] = b;
    pixels[2] = r;

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    if (rmt_transmit(led_chan, led_encoder, pixels, sizeof(pixels), &tx_config) == ESP_OK) {
        rmt_tx_wait_all_done(led_chan, portMAX_DELAY);
    }
    xSemaphoreGive(s_led_mux);
}

void led_set_hsv(uint16_t h, uint8_t s, uint8_t v)
{
    h %= 360;
    uint32_t rgb_max = v * 2.55f;
    uint32_t rgb_min = rgb_max * (100 - s) / 100.0f;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;
    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    uint32_t r, g, b;
    switch (i) {
    case 0: r = rgb_max; g = rgb_min + rgb_adj; b = rgb_min; break;
    case 1: r = rgb_max - rgb_adj; g = rgb_max; b = rgb_min; break;
    case 2: r = rgb_min; g = rgb_max; b = rgb_min + rgb_adj; break;
    case 3: r = rgb_min; g = rgb_max - rgb_adj; b = rgb_max; break;
    case 4: r = rgb_min + rgb_adj; g = rgb_min; b = rgb_max; break;
    default: r = rgb_max; g = rgb_min; b = rgb_max - rgb_adj; break;
    }

    led_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

void led_off(void)
{
    led_set_color(0, 0, 0);
}
