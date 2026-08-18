#include <stdio.h>
#include <unistd.h>
#include <sys/lock.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lcd_touch_xpt2046.h"
#include "display.h"

static const char *TAG = "display";

#define LCD_HOST          SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_H_RES          240
#define LCD_V_RES          320
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8

#define PIN_NUM_SCLK       12
#define PIN_NUM_MOSI       11
#define PIN_NUM_MISO       13
#define PIN_NUM_LCD_DC     9
#define PIN_NUM_LCD_RST    8
#define PIN_NUM_LCD_CS     10
#define PIN_NUM_LCD_BL     21
#define PIN_NUM_TOUCH_CS   14

#define LVGL_DRAW_BUF_LINES    40
#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 100
#define LVGL_TASK_STACK_SIZE   (10 * 1024)
#define LVGL_TASK_PRIORITY     2

static _lock_t lvgl_api_lock;
static lv_display_t *lvgl_disp = NULL;
static lv_obj_t *status_label = NULL;

/* ---- Backlight PWM + auto screen-off ---- */
#define LCD_BL_LEDC_TIMER    LEDC_TIMER_0
#define LCD_BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define LCD_BL_LEDC_FREQ_HZ  5000
#define LCD_BL_DEFAULT_PCT   80
#define LCD_BL_TIMEOUT_DEFAULT_SEC 30

static volatile int s_brightness_pct = LCD_BL_DEFAULT_PCT;
static volatile int s_screen_timeout_sec = LCD_BL_TIMEOUT_DEFAULT_SEC;
static volatile bool s_screen_off = false;
static esp_timer_handle_t s_off_timer = NULL;

static void bl_set_duty(int pct)
{
    uint32_t duty = (uint32_t)(pct * 255 / 100);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BL_LEDC_CHANNEL);
}

static void screen_off_cb(void *arg)
{
    s_screen_off = true;
    bl_set_duty(0);
}

void display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_brightness_pct = percent;
    if (!s_screen_off) {
        bl_set_duty(percent);
    }
}

int display_get_brightness(void)
{
    return s_brightness_pct;
}

void display_set_screen_timeout(int seconds)
{
    if (seconds < 0) seconds = 0;
    s_screen_timeout_sec = seconds;
    if (s_off_timer) {
        if (seconds > 0) {
            esp_timer_stop(s_off_timer);
            if (!s_screen_off) {
                esp_timer_start_once(s_off_timer, (uint64_t)seconds * 1000000ULL);
            }
        } else {
            esp_timer_stop(s_off_timer);
        }
    }
}

int display_get_screen_timeout(void)
{
    return s_screen_timeout_sec;
}

void display_notify_activity(void)
{
    if (s_screen_off) {
        s_screen_off = false;
        bl_set_duty(s_brightness_pct);
    }
    if (s_screen_timeout_sec > 0 && s_off_timer) {
        esp_timer_start_once(s_off_timer, (uint64_t)s_screen_timeout_sec * 1000000ULL);
    }
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    lv_draw_sw_rgb565_swap(px_map, (offsetx2 + 1 - offsetx1) * (offsety2 + 1 - offsety1));
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_touch_point_data_t touch_data[1] = {{0}};
    uint8_t touchpad_cnt = 0;
    static int call_count = 0;

    esp_lcd_touch_handle_t touch_pad = lv_indev_get_user_data(indev);
    esp_err_t err = esp_lcd_touch_read_data(touch_pad);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Touch read SPI error: %d", err);
    }
    esp_lcd_touch_get_data(touch_pad, touch_data, &touchpad_cnt, 1);

    call_count++;
    if (touchpad_cnt > 0) {
        ESP_LOGI(TAG, "Touch: x=%d, y=%d, strength=%d",
                 touch_data[0].x, touch_data[0].y, touch_data[0].strength);
        data->point.x = touch_data[0].x;
        data->point.y = touch_data[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        if (call_count % 50 == 0) {
            ESP_LOGI(TAG, "Touch poll #%d: no touch detected", call_count);
        }
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    while (1) {
        _lock_acquire(&lvgl_api_lock);
        uint32_t time_till_next_ms = lv_timer_handler();
        _lock_release(&lvgl_api_lock);
        time_till_next_ms = time_till_next_ms > LVGL_TASK_MIN_DELAY_MS ? time_till_next_ms : LVGL_TASK_MIN_DELAY_MS;
        time_till_next_ms = time_till_next_ms < LVGL_TASK_MAX_DELAY_MS ? time_till_next_ms : LVGL_TASK_MAX_DELAY_MS;
        usleep(1000 * time_till_next_ms);
    }
}

static void st7789_post_init(esp_lcd_panel_io_handle_t io)
{
    ESP_LOGI(TAG, "ST7789 post-init");

    esp_lcd_panel_io_tx_param(io, 0x13, NULL, 0);

    esp_lcd_panel_io_tx_param(io, 0x21, NULL, 0);

    {
        uint8_t data[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
        esp_lcd_panel_io_tx_param(io, 0xB2, data, sizeof(data));
    }
    {
        uint8_t data[] = {0x35};
        esp_lcd_panel_io_tx_param(io, 0xB7, data, sizeof(data));
    }
    {
        uint8_t data[] = {0x3F};
        esp_lcd_panel_io_tx_param(io, 0xBB, data, sizeof(data));
    }
    {
        uint8_t data[] = {0x2C};
        esp_lcd_panel_io_tx_param(io, 0xC0, data, sizeof(data));
    }
    {
        uint8_t data[] = {0x01};
        esp_lcd_panel_io_tx_param(io, 0xC1, data, sizeof(data));
    }
    {
        uint8_t data[] = {0x19};
        esp_lcd_panel_io_tx_param(io, 0xC5, data, sizeof(data));
    }
    {
        uint8_t pos_gamma[] = {0xD0, 0x08, 0x14, 0x09, 0x0E, 0x27, 0x3B, 0x44, 0x4D, 0x0E, 0x16, 0x14, 0x2B, 0x32};
        esp_lcd_panel_io_tx_param(io, 0xE0, pos_gamma, sizeof(pos_gamma));
    }
    {
        uint8_t neg_gamma[] = {0xD0, 0x08, 0x10, 0x08, 0x06, 0x06, 0x3B, 0x44, 0x4D, 0x0B, 0x16, 0x14, 0x2B, 0x33};
        esp_lcd_panel_io_tx_param(io, 0xE1, neg_gamma, sizeof(neg_gamma));
    }
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initialize backlight PWM");
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LCD_BL_LEDC_TIMER,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz = LCD_BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_channel = {
        .gpio_num = PIN_NUM_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_channel));

    const esp_timer_create_args_t off_timer_args = {
        .callback = &screen_off_cb,
        .name = "bl_off",
    };
    ESP_ERROR_CHECK(esp_timer_create(&off_timer_args, &s_off_timer));

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_NUM_SCLK,
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_LCD_DC,
        .cs_gpio_num = PIN_NUM_LCD_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_LOGI(TAG, "Install ST7789 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    st7789_post_init(io_handle);

    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "Turn on LCD backlight");
    bl_set_duty(LCD_BL_DEFAULT_PCT);

    ESP_LOGI(TAG, "Initialize LVGL");
    lv_init();

    lvgl_disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    size_t draw_buffer_sz = LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(lv_color16_t);
    void *buf1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf1);
    void *buf2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_sz, 0);
    assert(buf2);
    lv_display_set_buffers(lvgl_disp, buf1, buf2, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(lvgl_disp, panel_handle);
    lv_display_set_color_format(lvgl_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(lvgl_disp, lvgl_flush_cb);

    ESP_LOGI(TAG, "Create status label");
    lv_obj_t *scr = lv_display_get_screen_active(lvgl_disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "SmartKnob\nStarting...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(status_label);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Register flush ready callback");
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, lvgl_disp));

    ESP_LOGI(TAG, "Initialize XPT2046 touch");
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_config = ESP_LCD_TOUCH_IO_SPI_XPT2046_CONFIG(PIN_NUM_TOUCH_CS);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &tp_io_config, &tp_io_handle));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_xpt2046(tp_io_handle, &tp_cfg, &tp));

    // XPT2046 diagnostic: read raw registers
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "=== XPT2046 Raw Register Dump ===");
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t buf[2];
        esp_err_t e;

        buf[0] = buf[1] = 0;
        e = esp_lcd_panel_io_rx_param(tp_io_handle, 0xB1, buf, 2);
        ESP_LOGI(TAG, "  Z1 cmd=0xB1: buf=[0x%02X,0x%02X] raw=0x%04X err=%d", buf[0], buf[1], (buf[0]<<8)|buf[1], e);

        buf[0] = buf[1] = 0;
        e = esp_lcd_panel_io_rx_param(tp_io_handle, 0xC1, buf, 2);
        ESP_LOGI(TAG, "  Z2 cmd=0xC1: buf=[0x%02X,0x%02X] raw=0x%04X err=%d", buf[0], buf[1], (buf[0]<<8)|buf[1], e);

        buf[0] = buf[1] = 0;
        e = esp_lcd_panel_io_rx_param(tp_io_handle, 0xD1, buf, 2);
        ESP_LOGI(TAG, "  X  cmd=0xD1: buf=[0x%02X,0x%02X] raw=0x%04X err=%d", buf[0], buf[1], (buf[0]<<8)|buf[1], e);

        buf[0] = buf[1] = 0;
        e = esp_lcd_panel_io_rx_param(tp_io_handle, 0x91, buf, 2);
        ESP_LOGI(TAG, "  Y  cmd=0x91: buf=[0x%02X,0x%02X] raw=0x%04X err=%d", buf[0], buf[1], (buf[0]<<8)|buf[1], e);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "=== XPT2046 Dump End ===");

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, lvgl_disp);
    lv_indev_set_user_data(indev, tp);
    lv_indev_set_read_cb(indev, lvgl_touch_cb);

    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);

    ESP_LOGI(TAG, "Display init done");
    return ESP_OK;
}

esp_err_t display_show_text(const char *text)
{
    if (!lvgl_disp || !status_label) return ESP_ERR_INVALID_STATE;

    _lock_acquire(&lvgl_api_lock);
    lv_label_set_text(status_label, text);
    _lock_release(&lvgl_api_lock);

    return ESP_OK;
}

void display_set_status_label(void *label)
{
    status_label = (lv_obj_t *)label;
}

void display_lvgl_lock(void)
{
    _lock_acquire(&lvgl_api_lock);
}

void display_lvgl_unlock(void)
{
    _lock_release(&lvgl_api_lock);
}