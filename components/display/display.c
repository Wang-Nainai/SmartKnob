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
#define LVGL_TASK_MIN_DELAY_MS 5
#define LVGL_TASK_STACK_SIZE   (10 * 1024)
#define LVGL_TASK_PRIORITY     2
#define LVGL_TASK_CORE         0   /* motor 独占 core1, LVGL 固定 core0 */

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

/* ==================== XPT2046 裸驱动 ====================
 * 命令字(12bit, DFR, PD=00): 0xB0=Z1, 0xC0=Z2
 * 轴向(参考店家 XPT2406.C 实测约定):
 *   0xD0 = 横轴(X), 0x90 = 纵轴(Y)
 * 注意: 数据手册定义 0x90=A[001] "X-Position"/0xD0=A[101] "Y-Position",
 *       但面板玻璃接线决定实际轴向; 店家对该屏的实测代码即为
 *       CMD_RDX=0xD0 / CMD_RDY=0x90, 且可通过 TOUCH_SWAP_XY 再纠正。
 * 读数: 命令后跟 2 字节, 第 1 位为 busy, 12 位数据, 右移 3 位。 */
#define TP_CMD_X    0xD0
#define TP_CMD_Y    0x90
#define TP_CMD_Z1   0xB0
#define TP_CMD_Z2   0xC0

/* 店家滤波策略: 采 5 次, 排序, 去掉最大最小, 取中间均值 */
#define TP_READ_TIMES 5
#define TP_ERR_RANGE  100   /* 双读一致性阈值(原始值) */

typedef struct {
    esp_lcd_panel_io_handle_t io;
    /* 最近一次原始读数(诊断用) */
    uint16_t raw_z1;
    uint16_t raw_z2;
    uint16_t raw_x;
    uint16_t raw_y;
    bool touched;
    uint16_t x;         /* 映射后的屏幕坐标 */
    uint16_t y;
} xpt2046_state_t;

static xpt2046_state_t s_tp = { .io = NULL };

static uint16_t tp_read_reg(uint8_t cmd)
{
    uint8_t buf[2] = {0, 0};
    if (!s_tp.io) {
        return 0;
    }
    if (esp_lcd_panel_io_rx_param(s_tp.io, cmd, buf, 2) != ESP_OK) {
        return 0;
    }
    return (((uint16_t)buf[0] << 8) | buf[1]) >> 3;
}

/* 店家滤波: 采 TP_READ_TIMES 次, 排序去极值, 取中间均值 */
static uint16_t tp_read_filtered(uint8_t cmd)
{
    uint16_t buf[TP_READ_TIMES];
    for (int i = 0; i < TP_READ_TIMES; i++) {
        buf[i] = tp_read_reg(cmd);
    }
    for (int i = 0; i < TP_READ_TIMES - 1; i++) {
        for (int j = i + 1; j < TP_READ_TIMES; j++) {
            if (buf[i] > buf[j]) {
                uint16_t t = buf[i]; buf[i] = buf[j]; buf[j] = t;
            }
        }
    }
    uint32_t sum = 0;
    for (int i = 1; i < TP_READ_TIMES - 1; i++) {
        sum += buf[i];
    }
    return (uint16_t)(sum / (TP_READ_TIMES - 2));
}

static uint16_t tp_map(int32_t raw, int32_t raw_min, int32_t raw_max, int32_t out_max)
{
    if (raw_max <= raw_min) {
        return 0;
    }
    int32_t v = (raw - raw_min) * out_max / (raw_max - raw_min);
    if (v < 0) v = 0;
    if (v >= out_max) v = out_max - 1;
    return (uint16_t)v;
}

static void tp_poll(void)
{
    uint16_t z1 = tp_read_reg(TP_CMD_Z1);
    uint16_t z2 = tp_read_reg(TP_CMD_Z2);
    int32_t pressure = (int32_t)z1 + 4095 - (int32_t)z2;

    s_tp.raw_z1 = z1;
    s_tp.raw_z2 = z2;

    /* 总线异常防护: 芯片缺失/接线故障时 MISO 悬空, 读数恒 0 或 0xFFF,
     * 此时 pressure 恒为 4095, 会产生持续的幽灵触摸, 必须判无效 */
    if ((z1 >= 4090 && z2 >= 4090) || (z1 <= 5 && z2 <= 5)) {
        s_tp.touched = false;
        s_tp.raw_x = 0;
        s_tp.raw_y = 0;
        return;
    }

    if (pressure < CONFIG_TOUCH_Z_THRESHOLD) {
        s_tp.touched = false;
        s_tp.raw_x = 0;
        s_tp.raw_y = 0;
        return;
    }

    /* 首次转换丢弃(Vref 稳定), 再滤波取样 */
    tp_read_reg(TP_CMD_X);
    uint16_t rx = tp_read_filtered(TP_CMD_X);
    tp_read_reg(TP_CMD_Y);
    uint16_t ry = tp_read_filtered(TP_CMD_Y);

    /* 店家一致性校验(Read_XY2): 二次读数偏差过大视为噪声丢弃 */
    uint16_t rx2 = tp_read_filtered(TP_CMD_X);
    uint16_t ry2 = tp_read_filtered(TP_CMD_Y);
    int32_t dx = (int32_t)rx - (int32_t)rx2;
    int32_t dy = (int32_t)ry - (int32_t)ry2;
    if (dx < -TP_ERR_RANGE || dx > TP_ERR_RANGE ||
        dy < -TP_ERR_RANGE || dy > TP_ERR_RANGE) {
        s_tp.touched = false;
        s_tp.raw_x = 0;
        s_tp.raw_y = 0;
        return;
    }

    s_tp.raw_x = rx;
    s_tp.raw_y = ry;

    /* 坐标有效性窗口(参照通用触摸库: 50 < raw < 4045) */
    if (rx < 50 || rx > 4045 || ry < 50 || ry > 4045) {
        s_tp.touched = false;
        return;
    }
    s_tp.touched = true;

    uint16_t mx = tp_map(rx, CONFIG_TOUCH_X_MIN, CONFIG_TOUCH_X_MAX, LCD_H_RES);
    uint16_t my = tp_map(ry, CONFIG_TOUCH_Y_MIN, CONFIG_TOUCH_Y_MAX, LCD_V_RES);
#if CONFIG_TOUCH_SWAP_XY
    uint16_t t = mx; mx = my; my = t;
#endif
#if CONFIG_TOUCH_MIRROR_X
    mx = LCD_H_RES - 1 - mx;
#endif
#if CONFIG_TOUCH_MIRROR_Y
    my = LCD_V_RES - 1 - my;
#endif
    s_tp.x = mx;
    s_tp.y = my;
}

bool display_touch_get_raw(uint16_t *z1, uint16_t *z2, uint16_t *raw_x, uint16_t *raw_y)
{
    *z1 = s_tp.raw_z1;
    *z2 = s_tp.raw_z2;
    *raw_x = s_tp.raw_x;
    *raw_y = s_tp.raw_y;
    return s_tp.touched;
}

static void lvgl_touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    tp_poll();
    if (s_tp.touched) {
        data->point.x = s_tp.x;
        data->point.y = s_tp.y;
        data->state = LV_INDEV_STATE_PRESSED;
        display_notify_activity();   /* 触摸同样唤醒/重置熄屏计时 */
    } else {
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

    /* 该panel颜色需要 INVOFF(实机验证: INVON 时黑底显示为白、颜色反相) */
    esp_lcd_panel_io_tx_param(io, 0x20, NULL, 0);

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

    ESP_LOGI(TAG, "Initialize XPT2046 touch (raw driver)");
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_spi_config_t tp_io_config = {
        .cs_gpio_num = PIN_NUM_TOUCH_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,                  /* XPT2046: mode 0, <=2.5MHz */
        .pclk_hz = 1 * 1000 * 1000,
        .trans_queue_depth = 3,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &tp_io_config, &tp_io_handle));
    s_tp.io = tp_io_handle;

    /* 开机自检: 读一次原始值并自动判读
     * 正常待机: z1≈0, z2≈4095 (未按下)
     * 全 0:     芯片未响应 -> T_CLK/T_DIN/T_DO/T_CS 接线问题,
     *           或模块根本没有触摸 IC(GMT240 部分版本 T_* 引脚为 NC) */
    vTaskDelay(pdMS_TO_TICKS(100));
    uint16_t z1 = tp_read_reg(TP_CMD_Z1);
    uint16_t z2 = tp_read_reg(TP_CMD_Z2);
    uint16_t rx = tp_read_reg(TP_CMD_X);
    uint16_t ry = tp_read_reg(TP_CMD_Y);
    if (z1 == 0 && z2 == 0 && rx == 0 && ry == 0) {
        ESP_LOGW(TAG, "XPT2046 selftest: z1=0 z2=0 x=0 y=0 -> 芯片未响应!");
        ESP_LOGW(TAG, "  检查接线: T_CLK->GPIO12 T_DIN->GPIO11 T_DO->GPIO13 T_CS->GPIO14");
        ESP_LOGW(TAG, "  或确认模块背面是否有 2046 触摸芯片(无触摸版本 T_* 引脚为 NC)");
    } else if (z2 >= 4000 && z1 <= 50) {
        ESP_LOGI(TAG, "XPT2046 selftest: z1=%u z2=%u x=%u y=%u (正常待机, 触摸后进工厂测试看实时值)",
                 z1, z2, rx, ry);
    } else {
        ESP_LOGI(TAG, "XPT2046 selftest: z1=%u z2=%u x=%u y=%u (有信号, 可能正被触摸)", z1, z2, rx, ry);
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, lvgl_disp);
    lv_indev_set_user_data(indev, &s_tp);
    lv_indev_set_read_cb(indev, lvgl_touch_cb);

    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreatePinnedToCore(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL, LVGL_TASK_CORE);

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