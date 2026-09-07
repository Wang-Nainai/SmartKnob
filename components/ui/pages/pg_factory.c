#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "wifi.h"
#include "mqtt.h"
#include "led.h"
#include "display.h"

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_msyh_16);

void ui_env_get(uint16_t *co2, float *temp, float *rh, uint8_t *has_data);

#define FACTORY_ITEMS 6

typedef struct {
    int focus;
    bool in_touch_test;
    int led_state;          /* 0=off 1=red 2=green 3=blue */
    lv_obj_t *rows[FACTORY_ITEMS];
    lv_obj_t *status;
    lv_obj_t *touch_scr;    /* 触摸测试全屏视图 */
    lv_obj_t *touch_label;
    lv_obj_t *raw_label;    /* XPT2046 原始读数实时诊断 */
    int touch_count;
    lv_timer_t *timer;
} factory_data_t;

typedef struct {
    const char *name;
    const char *desc;
} factory_item_t;

static const factory_item_t items[FACTORY_ITEMS] = {
    { "\xE8\xA7\xA6\xE6\x91\xB8\xE6\xB5\x8B\xE8\xAF\x95", "Touch" },
    { "LED \xE6\xB5\x8B\xE8\xAF\x95", "WS2812" },
    { "\xE7\x94\xB5\xE6\x9C\xBA\xE6\xB5\x8B\xE8\xAF\x95", "Motor" },
    { "\xE7\xBC\x96\xE7\xA0\x81\xE5\x99\xA8", "Encoder" },
    { "SCD40", "CO2/\xE6\xB8\xA9/\xE6\xB9\xBF" },
    { "WiFi / MQTT", "Network" },
};

static void factory_refresh(factory_data_t *d)
{
    for (int i = 0; i < FACTORY_ITEMS; i++) {
        lv_obj_t *row = d->rows[i];
        if (i == d->focus) {
            lv_obj_set_style_border_color(row, lv_color_hex(XK_COLOR_RED), 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(XK_COLOR_PANEL), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
    }
}

/* 动态项: 编码器/SCD40/网络 实时显示在底部状态栏 */
static void factory_update_status(factory_data_t *d)
{
    char buf[96];
    switch (d->focus) {
    case 3: {  /* Encoder */
        int32_t pos = motor_get_position();
        float deg = motor_get_angle_offset_deg();
        snprintf(buf, sizeof(buf), "pos=%ld  off=%.1f deg", (long)pos, (double)deg);
        lv_label_set_text(d->status, buf);
        break;
    }
    case 4: {  /* SCD40 */
        uint16_t co2;
        float t, rh;
        uint8_t has;
        ui_env_get(&co2, &t, &rh, &has);
        if (has) {
            snprintf(buf, sizeof(buf), "CO2=%u ppm  %.1f C  %.0f %%", co2, (double)t, (double)rh);
        } else {
            snprintf(buf, sizeof(buf), "no data");
        }
        lv_label_set_text(d->status, buf);
        break;
    }
    case 5: {  /* Network */
        snprintf(buf, sizeof(buf), "WiFi=%s  MQTT=%s",
                 wifi_is_connected() ? "ON" : "OFF",
                 mqtt_ha_is_connected() ? "ON" : "OFF");
        lv_label_set_text(d->status, buf);
        break;
    }
    default:
        break;
    }
}

/* 列表项点击执行 */
static void factory_row_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    page_t *p = (page_t *)lv_obj_get_user_data(obj);
    factory_data_t *d = p->data;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    d->focus = idx;
    factory_refresh(d);
    char buf[96];

    switch (idx) {
    case 0:  /* Touch test: 进入全屏触摸视图 */
        d->in_touch_test = true;
        d->touch_count = 0;
        for (int i = 0; i < FACTORY_ITEMS; i++) {
            lv_obj_add_flag(d->rows[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_add_flag(d->status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(d->touch_scr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(d->raw_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(d->touch_label, "\xE7\x82\xB9\xE5\x87\xBB\xE5\xB1\x8F\xE5\xB9\x95\xE6\xB5\x8B\xE8\xAF\x95");
        break;
    case 1:  /* LED: 循环颜色 */
        d->led_state = (d->led_state + 1) % 4;
        switch (d->led_state) {
        case 0: led_set_color(0, 0, 0); snprintf(buf, sizeof(buf), "LED: OFF"); break;
        case 1: led_set_color(255, 0, 0); snprintf(buf, sizeof(buf), "LED: RED"); break;
        case 2: led_set_color(0, 255, 0); snprintf(buf, sizeof(buf), "LED: GREEN"); break;
        default: led_set_color(0, 0, 255); snprintf(buf, sizeof(buf), "LED: BLUE"); break;
        }
        lv_label_set_text(d->status, buf);
        break;
    case 2:  /* Motor: 振动反馈 */
        motor_shake(3, 40);
        lv_label_set_text(d->status, "Motor shake OK");
        break;
    default:
        break;
    }
}

/* 触摸测试视图点击: 显示坐标与计数; 右上角退出 */
static void factory_touch_cb(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);
    factory_data_t *d = p->data;
    if (!d->in_touch_test) {
        return;
    }
    /* 右上角退出区域 (x>200, y<40) */
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        if (pt.x > 200 && pt.y < 40) {
            /* 退出触摸测试 */
            d->in_touch_test = false;
            lv_obj_add_flag(d->touch_scr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(d->raw_label, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < FACTORY_ITEMS; i++) {
                lv_obj_clear_flag(d->rows[i], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_clear_flag(d->status, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        d->touch_count++;
        char buf[96];
        snprintf(buf, sizeof(buf), "\xE7\x82\xB9\xE5\x87\xBB x=%d y=%d  \xE8\xAE\xA1\xE6\x95\xB0 %d",
                 (int)pt.x, (int)pt.y, d->touch_count);
        lv_label_set_text(d->touch_label, buf);
    }
}

static void factory_timer_cb(lv_timer_t *t)
{
    factory_data_t *d = lv_timer_get_user_data(t);
    if (d->in_touch_test) {
        /* 触摸原始值实时诊断: z1 按下应明显增大;
         * 恒为 0/4095 不变 => 接线问题(T_CLK/T_DIN/T_DO 未接) */
        uint16_t z1, z2, rx, ry;
        bool touched = display_touch_get_raw(&z1, &z2, &rx, &ry);
        char buf[128];
        snprintf(buf, sizeof(buf), "z1=%u  z2=%u\nraw x=%u y=%u  %s\n\nT_CLK/T_DIN/T_DO \xE9\x9C\x80\xE6\x8E\xA5" "SCK/MOSI/MISO",
                 z1, z2, rx, ry, touched ? "\xE2\x97\x8F" : "-");
        lv_label_set_text(d->raw_label, buf);
        return;
    }
    factory_update_status(d);
}

static void pg_factory_create(page_t *p)
{
    factory_data_t *d = calloc(1, sizeof(factory_data_t));
    p->data = d;
    d->focus = 0;
    d->in_touch_test = false;
    d->led_state = 0;
    p->title = "\xE5\xB7\xA5\xE5\x8E\x82\xE6\xB5\x8B\xE8\xAF\x95";

    for (int i = 0; i < FACTORY_ITEMS; i++) {
        lv_obj_t *row = lv_obj_create(p->root);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 240, 44);
        lv_obj_set_pos(row, 0, 30 + i * 44);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(XK_COLOR_RED), 0);
        lv_obj_set_style_border_post(row, true, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, p);
        lv_obj_add_event_cb(row, factory_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
        lv_label_set_text(name, items[i].name);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t *desc = lv_label_create(row);
        lv_obj_set_style_text_color(desc, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(desc, &lv_font_msyh_16, 0);
        lv_label_set_text(desc, items[i].desc);
        lv_obj_align(desc, LV_ALIGN_RIGHT_MID, -12, 0);

        d->rows[i] = row;
    }
    factory_refresh(d);

    d->status = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->status, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->status, &lv_font_msyh_16, 0);
    lv_label_set_text(d->status, "\xE7\x82\xB9\xE5\x87\xBB\xE6\x89\xA7\xE8\xA1\x8C\xE6\xB5\x8B\xE8\xAF\x95");
    lv_obj_align(d->status, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* 触摸测试全屏视图 (y=22 起, 高 298, 恰好铺满页面不产生滚动) */
    d->touch_scr = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->touch_scr);
    lv_obj_set_size(d->touch_scr, 240, 298);
    lv_obj_set_pos(d->touch_scr, 0, 22);
    lv_obj_set_style_bg_color(d->touch_scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(d->touch_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(d->touch_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->touch_scr, factory_touch_cb, LV_EVENT_CLICKED, p);
    lv_obj_add_flag(d->touch_scr, LV_OBJ_FLAG_HIDDEN);

    d->touch_label = lv_label_create(d->touch_scr);
    lv_obj_set_style_text_color(d->touch_label, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->touch_label, &lv_font_msyh_16, 0);
    lv_obj_align(d->touch_label, LV_ALIGN_CENTER, 0, 60);

    d->raw_label = lv_label_create(d->touch_scr);
    lv_obj_set_style_text_color(d->raw_label, lv_color_hex(XK_COLOR_GREEN), 0);
    lv_obj_set_style_text_font(d->raw_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(d->raw_label, "z1=--  z2=--");
    lv_obj_align(d->raw_label, LV_ALIGN_CENTER, 0, -50);
    lv_obj_add_flag(d->raw_label, LV_OBJ_FLAG_HIDDEN);

    d->timer = lv_timer_create(factory_timer_cb, 500, d);
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
}

static void pg_factory_destroy(page_t *p)
{
    factory_data_t *d = p->data;
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        led_set_color(0, 0, 0);   /* 退出时关灯 */
        free(d);
    }
    p->data = NULL;
}

static void pg_factory_on_rotate(page_t *p, int32_t steps)
{
    factory_data_t *d = p->data;
    if (d->in_touch_test) {
        return;
    }
    d->focus = (d->focus + steps) % FACTORY_ITEMS;
    if (d->focus < 0) {
        d->focus += FACTORY_ITEMS;
    }
    factory_refresh(d);
    factory_update_status(d);
}

static void pg_factory_on_back(page_t *p)
{
    factory_data_t *d = p->data;
    if (d->in_touch_test) {
        d->in_touch_test = false;
        lv_obj_add_flag(d->touch_scr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(d->raw_label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < FACTORY_ITEMS; i++) {
            lv_obj_clear_flag(d->rows[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(d->status, LV_OBJ_FLAG_HIDDEN);
    } else {
        pm_pop();
    }
}

static void pg_factory_on_tick(page_t *p)
{
}

static void pg_factory_on_resume(page_t *p)
{
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
}

const page_ops_t pg_factory_ops = {
    .create = pg_factory_create,
    .destroy = pg_factory_destroy,
    .on_rotate = pg_factory_on_rotate,
    .on_back = pg_factory_on_back,
    .on_tick = pg_factory_on_tick,
    .on_resume = pg_factory_on_resume,
};