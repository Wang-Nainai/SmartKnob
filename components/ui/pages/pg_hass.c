#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "mqtt.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_msyh_16);

#define HASS_DEVICE_NUM 4

static const char *device_names[HASS_DEVICE_NUM] = {
    "\xE7\x81\xAF\xE5\x85\x89",   /* 灯光 */
    "\xE7\xA9\xBA\xE8\xB0\x83",   /* 空调 */
    "\xE9\xA3\x8E\xE6\x89\x87",   /* 风扇 */
    "\xE6\xB4\x97\xE8\xA1\xA3\xE6\x9C\xBA", /* 洗衣机 */
};

typedef struct {
    int focus;
    bool in_control;
    lv_obj_t *tiles[4];
    lv_obj_t *hint;
    lv_obj_t *scale;
    lv_obj_t *needle;
    lv_obj_t *label_name;
    lv_obj_t *label_last;
    lv_timer_t *timer;
} hass_data_t;

static void hass_refresh_focus(hass_data_t *d)
{
    for (int i = 0; i < HASS_DEVICE_NUM; i++) {
        if (i == d->focus) {
            lv_obj_set_style_border_color(d->tiles[i], lv_color_hex(XK_COLOR_RED), 0);
            lv_obj_set_style_border_width(d->tiles[i], 2, 0);
            lv_obj_set_style_bg_color(d->tiles[i], lv_color_hex(XK_COLOR_PANEL), 0);
            lv_obj_set_style_bg_opa(d->tiles[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_border_width(d->tiles[i], 0, 0);
            lv_obj_set_style_bg_color(d->tiles[i], lv_color_hex(0x1A1A1A), 0);
            lv_obj_set_style_bg_opa(d->tiles[i], LV_OPA_COVER, 0);
        }
    }
}

static void hass_enter_control(hass_data_t *d)
{
    d->in_control = true;
    for (int i = 0; i < HASS_DEVICE_NUM; i++) {
        lv_obj_add_flag(d->tiles[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(d->hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d->scale, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d->label_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d->label_last, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(d->label_name, device_names[d->focus]);
    lv_label_set_text(d->label_last, "\xE7\x82\xB9\xE5\x87\xBB\x3A ON/OFF");

    motor_set_mode(MOTOR_MODE_UNBOUND_NO_DETENTS, 0, 0);
    pm_shake();
}

static void hass_exit_control(hass_data_t *d)
{
    d->in_control = false;
    for (int i = 0; i < HASS_DEVICE_NUM; i++) {
        lv_obj_clear_flag(d->tiles[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(d->hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->scale, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->label_name, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->label_last, LV_OBJ_FLAG_HIDDEN);
    motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);
    pm_shake();
}

/* 触摸点击设备宫格 → 进入控制视图 */
static void hass_tile_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    page_t *p = (page_t *)lv_obj_get_user_data(obj);
    hass_data_t *d = p->data;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    d->focus = idx;
    hass_refresh_focus(d);
    hass_enter_control(d);
}

/* 控制视图: 触摸点击 → ON/OFF */
static void hass_tap_cb(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);
    hass_data_t *d = p->data;
    if (!d->in_control) {
        return;
    }
    mqtt_ha_publish_cmd(device_names[d->focus], "ON/OFF");
    lv_label_set_text(d->label_last, "ON/OFF");
    pm_shake();
}

static void hass_timer_cb(lv_timer_t *t)
{
    hass_data_t *d = lv_timer_get_user_data(t);
    if (!d->in_control) return;
    int32_t pos = motor_get_position();
    int32_t val = pos % 72;
    if (val < 0) val += 72;
    lv_scale_set_line_needle_value(d->scale, d->needle, 95, val);
}

static void pg_hass_create(page_t *p)
{
    hass_data_t *d = calloc(1, sizeof(hass_data_t));
    p->data = d;
    d->focus = 0;
    d->in_control = false;
    p->title = "\xE6\x99\xBA\xE8\x83\xBD\xE5\xAE\xB6\xE5\xB1\x85";

    d->hint = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(d->hint, &lv_font_msyh_16, 0);
    lv_label_set_text(d->hint, "\xE7\x82\xB9\xE5\x87\xBB\xE8\xAE\xBE\xE5\xA4\x87 \xC2\xB7 \xE6\x97\x8B\xE8\xBD\xAC\xE6\x8E\xA7\xE5\x88\xB6");
    lv_obj_align(d->hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    static const int gx[4] = { 14, 124, 14, 124 };
    static const int gy[4] = { 40, 40, 170, 170 };
    for (int i = 0; i < HASS_DEVICE_NUM; i++) {
        lv_obj_t *tile = lv_obj_create(p->root);
        lv_obj_remove_style_all(tile);
        lv_obj_set_size(tile, 100, 116);
        lv_obj_set_pos(tile, gx[i], gy[i]);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x1A1A1A), 0);
        lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tile, 8, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(XK_COLOR_RED), 0);
        lv_obj_set_style_border_post(tile, true, 0);
        lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(tile, p);
        lv_obj_add_event_cb(tile, hass_tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon = lv_label_create(tile);
        lv_obj_set_style_text_color(icon, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(icon, &lv_font_msyh_16, 0);
        lv_label_set_text(icon, device_names[i]);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);

        d->tiles[i] = tile;
    }
    hass_refresh_focus(d);

    d->scale = lv_scale_create(p->root);
    lv_obj_set_pos(d->scale, 0, 40);
    lv_obj_set_size(d->scale, 240, 240);
    lv_obj_set_style_bg_color(d->scale, lv_color_hex(XK_COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(d->scale, lv_color_make(64, 0, 64), 0);
    lv_obj_set_style_bg_grad_dir(d->scale, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(d->scale, LV_RADIUS_CIRCLE, 0);
    lv_scale_set_mode(d->scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(d->scale, false);
    lv_obj_set_style_length(d->scale, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_width(d->scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(d->scale, lv_color_hex(XK_COLOR_RED), LV_PART_ITEMS);
    lv_obj_set_style_length(d->scale, 14, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(d->scale, 3, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(d->scale, lv_color_hex(XK_COLOR_RED), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(d->scale, lv_color_hex(XK_COLOR_RED), LV_PART_MAIN);
    lv_obj_set_style_arc_width(d->scale, 2, LV_PART_MAIN);
    lv_scale_set_total_tick_count(d->scale, 73);
    lv_scale_set_major_tick_every(d->scale, 1);
    lv_scale_set_range(d->scale, 0, 72);
    lv_scale_set_angle_range(d->scale, 360);
    lv_scale_set_rotation(d->scale, 270);

    static lv_point_precise_t needle_points[2] = { {0, 0}, {0, 0} };
    d->needle = lv_line_create(d->scale);
    lv_line_set_points_mutable(d->needle, needle_points, 2);
    lv_obj_set_style_line_width(d->needle, 8, 0);
    lv_obj_set_style_line_rounded(d->needle, true, 0);
    lv_obj_set_style_line_color(d->needle, lv_color_hex(XK_COLOR_BLUE), 0);
    lv_scale_set_post_draw(d->scale, true);
    lv_scale_set_line_needle_value(d->scale, d->needle, 95, 0);
    lv_obj_add_flag(d->scale, LV_OBJ_FLAG_HIDDEN);

    d->label_name = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_name, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->label_name, &lv_font_montserrat_26, 0);
    lv_obj_align(d->label_name, LV_ALIGN_CENTER, 0, 70);
    lv_obj_add_flag(d->label_name, LV_OBJ_FLAG_HIDDEN);

    d->label_last = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_last, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_last, &lv_font_msyh_16, 0);
    lv_obj_align(d->label_last, LV_ALIGN_CENTER, 0, 120);
    lv_obj_add_flag(d->label_last, LV_OBJ_FLAG_HIDDEN);

    d->timer = lv_timer_create(hass_timer_cb, 50, d);

    /* 控制视图: 整页点击发 ON/OFF */
    lv_obj_add_flag(p->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->root, hass_tap_cb, LV_EVENT_CLICKED, p);

    motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);
}

static void pg_hass_destroy(page_t *p)
{
    hass_data_t *d = p->data;
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        free(d);
    }
    p->data = NULL;
}

static void pg_hass_on_rotate(page_t *p, int32_t steps)
{
    hass_data_t *d = p->data;
    if (d->in_control) {
        mqtt_ha_publish_cmd(device_names[d->focus], steps > 0 ? "RIGHT" : "LEFT");
        lv_label_set_text(d->label_last, steps > 0 ? "RIGHT" : "LEFT");
        pm_shake();
    } else {
        d->focus = (d->focus + steps) % HASS_DEVICE_NUM;
        if (d->focus < 0) {
            d->focus += HASS_DEVICE_NUM;
        }
        hass_refresh_focus(d);
    }
}

static void pg_hass_on_back(page_t *p)
{
    hass_data_t *d = p->data;
    if (d->in_control) {
        hass_exit_control(d);
    } else {
        pm_pop();
    }
}

static void pg_hass_on_tick(page_t *p)
{
}

const page_ops_t pg_hass_ops = {
    .create = pg_hass_create,
    .destroy = pg_hass_destroy,
    .on_rotate = pg_hass_on_rotate,
    .on_back = pg_hass_on_back,
    .on_tick = pg_hass_on_tick,
};