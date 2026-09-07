#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "mqtt.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * X-Knob 风格智能家居页
 * - 设备列表: 与主菜单同款"聚焦展开"行
 * - 控制视图: 全屏表盘(73 刻度 360°, 蓝色指针)
 *   旋转 = LEFT/RIGHT, 点击 = ON/OFF
 * ============================================================ */

#define HASS_DEVICE_NUM 4
#define ROW_H 70
#define LIST_PAD ((320 - ROW_H) / 2)
#define ICON_W_OPEN   220
#define ICON_W_FOCUS  70

static const char *device_names[HASS_DEVICE_NUM] = {
    "\xE7\x81\xAF\xE5\x85\x89",   /* 灯光 */
    "\xE7\xA9\xBA\xE8\xB0\x83",   /* 空调 */
    "\xE9\xA3\x8E\xE6\x89\x87",   /* 风扇 */
    "\xE6\xB4\x97\xE8\xA1\xA3\xE6\x9C\xBA", /* 洗衣机 */
};

static const char *device_icons[HASS_DEVICE_NUM] = {
    LV_SYMBOL_BELL,        /* 灯光 */
    LV_SYMBOL_SETTINGS,    /* 空调 */
    LV_SYMBOL_CHARGE,      /* 风扇 */
    LV_SYMBOL_LOOP,        /* 洗衣机 */
};

typedef struct {
    int focus;
    bool in_control;
    lv_obj_t *list;         /* 设备列表滚动容器 */
    lv_obj_t *rows[HASS_DEVICE_NUM];
    lv_obj_t *icons[HASS_DEVICE_NUM];
    lv_obj_t *ctrl_scr;     /* 控制视图 */
    lv_obj_t *scale;        /* 控制视图表盘 */
    lv_obj_t *needle;
    lv_obj_t *label_name;
    lv_obj_t *label_last;
    lv_timer_t *timer;
} hass_data_t;

static void hass_set_focus(hass_data_t *d, int idx)
{
    if (idx < 0 || idx >= HASS_DEVICE_NUM) {
        return;
    }
    for (int i = 0; i < HASS_DEVICE_NUM; i++) {
        if (i == idx) {
            lv_obj_add_state(d->icons[i], LV_STATE_FOCUSED);
        } else {
            lv_obj_remove_state(d->icons[i], LV_STATE_FOCUSED);
        }
    }
    lv_obj_scroll_to_view(d->rows[idx], LV_ANIM_ON);
}

static void hass_show_control(hass_data_t *d, bool ctrl)
{
    d->in_control = ctrl;
    if (ctrl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s / %d", device_names[d->focus], d->focus + 1);
        lv_label_set_text(d->label_name, buf);
        lv_label_set_text(d->label_last, "\xE7\x82\xB9\xE5\x87\xBB\x3A ON/OFF");
        lv_obj_clear_flag(d->ctrl_scr, LV_OBJ_FLAG_HIDDEN);
        motor_set_mode(MOTOR_MODE_UNBOUND_NO_DETENTS, 0, 0);
    } else {
        lv_obj_add_flag(d->ctrl_scr, LV_OBJ_FLAG_HIDDEN);
        motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    }
}

/* 点击设备行 → 控制视图 */
static void hass_row_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    page_t *p = (page_t *)lv_obj_get_user_data(row);
    hass_data_t *d = p->data;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    d->focus = idx;
    hass_set_focus(d, idx);
    hass_show_control(d, true);
    pm_shake();
}

/* 控制视图点击 → ON/OFF */
static void hass_tap_cb(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);
    hass_data_t *d = p->data;
    if (!d->in_control) {
        return;
    }
    mqtt_ha_publish_cmd(device_names[d->focus], "ON/OFF");
    mqtt_ha_publish_action(d->focus, "ON");
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

    /* 设备列表: 独立滚动容器(flex), 控制视图作为根上浮层, 互不影响 */
    lv_obj_t *list = lv_obj_create(p->root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 240, 320);
    lv_obj_set_pos(list, 0, 0);
    d->list = list;
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(list, LIST_PAD, 0);

    for (int i = 0; i < HASS_DEVICE_NUM; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 220, ROW_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, p);
        lv_obj_add_event_cb(row, hass_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        d->rows[i] = row;

        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, ICON_W_OPEN, ROW_H);
        lv_obj_set_style_bg_color(icon, lv_color_hex(XK_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_align(icon, LV_ALIGN_LEFT_MID, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(icon, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(icon, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_set_style_width(icon, ICON_W_FOCUS, LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(icon, LV_BORDER_SIDE_RIGHT, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(icon, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(icon, lv_color_hex(XK_COLOR_ACCENT), LV_STATE_FOCUSED);

        static lv_style_transition_dsc_t trans;
        static const lv_style_prop_t props[] = { LV_STYLE_WIDTH, LV_STYLE_PROP_INV };
        lv_style_transition_dsc_init(&trans, props, lv_anim_path_overshoot, 200, 0, NULL);
        lv_obj_set_style_transition(icon, &trans, LV_STATE_FOCUSED);
        lv_obj_set_style_transition(icon, &trans, 0);

        lv_obj_t *img = lv_label_create(icon);
        lv_obj_set_style_text_color(img, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(img, &lv_font_montserrat_26, 0);
        lv_label_set_text(img, device_icons[i]);

        lv_obj_t *name = lv_label_create(icon);
        lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
        lv_label_set_text(name, device_names[i]);
        d->icons[i] = icon;

        lv_obj_t *info = lv_label_create(row);
        lv_obj_set_style_text_color(info, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(info, &lv_font_msyh_16, 0);
        lv_label_set_text(info, "\xE7\x82\xB9\xE5\x87\xBB\xE6\x8E\xA7\xE5\x88\xB6");
        lv_obj_align(info, LV_ALIGN_LEFT_MID, ICON_W_FOCUS + 5, 0);

        lv_obj_move_foreground(icon);
    }

    /* ---- 控制视图 (全屏表盘) ---- */
    d->ctrl_scr = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->ctrl_scr);
    lv_obj_set_size(d->ctrl_scr, 240, 320);
    lv_obj_set_pos(d->ctrl_scr, 0, 0);
    lv_obj_set_style_bg_color(d->ctrl_scr, lv_color_hex(XK_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(d->ctrl_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(d->ctrl_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->ctrl_scr, hass_tap_cb, LV_EVENT_CLICKED, p);
    lv_obj_set_user_data(d->ctrl_scr, p);
    lv_obj_add_flag(d->ctrl_scr, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *scale = lv_scale_create(d->ctrl_scr);
    d->scale = scale;
    lv_obj_set_pos(scale, 0, 50);
    lv_obj_set_size(scale, 240, 240);
    lv_obj_set_style_bg_color(scale, lv_color_hex(XK_COLOR_PANEL), 0);
    lv_obj_set_style_radius(scale, LV_RADIUS_CIRCLE, 0);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(scale, false);
    lv_obj_set_style_length(scale, 5, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x4A4A4A), LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 12, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(XK_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(scale, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_arc_width(scale, 1, LV_PART_MAIN);
    lv_scale_set_total_tick_count(scale, 73);
    lv_scale_set_major_tick_every(scale, 1);
    lv_scale_set_range(scale, 0, 72);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 270);

    static lv_point_precise_t needle_points[2] = { {0, 0}, {0, 0} };
    d->needle = lv_line_create(scale);
    lv_line_set_points_mutable(d->needle, needle_points, 2);
    lv_obj_set_style_line_width(d->needle, 8, 0);
    lv_obj_set_style_line_rounded(d->needle, true, 0);
    lv_obj_set_style_line_color(d->needle, lv_color_hex(XK_COLOR_BLUE), 0);
    lv_scale_set_post_draw(scale, true);
    lv_scale_set_line_needle_value(scale, d->needle, 95, 0);
    /* scale 默认可点击且不冒泡, 会吞掉控制视图的"点击=ON/OFF" */
    lv_obj_remove_flag(scale, LV_OBJ_FLAG_CLICKABLE);

    d->label_name = lv_label_create(d->ctrl_scr);
    lv_obj_set_style_text_color(d->label_name, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->label_name, &lv_font_msyh_16, 0);
    lv_obj_align(d->label_name, LV_ALIGN_TOP_MID, 0, 40);

    d->label_last = lv_label_create(d->ctrl_scr);
    lv_obj_set_style_text_color(d->label_last, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_last, &lv_font_msyh_16, 0);
    lv_obj_align(d->label_last, LV_ALIGN_BOTTOM_MID, 0, -40);

    d->timer = lv_timer_create(hass_timer_cb, 50, d);
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    hass_set_focus(d, 0);
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
        /* 节流: 快转时最多 10 条/秒, 避免 MQTT 洪泛 */
        static uint32_t last_cmd_tick = 0;
        uint32_t now = lv_tick_get();
        if (now - last_cmd_tick < 100) {
            return;
        }
        last_cmd_tick = now;
        mqtt_ha_publish_cmd(device_names[d->focus], steps > 0 ? "RIGHT" : "LEFT");
        mqtt_ha_publish_action(d->focus, steps > 0 ? "RIGHT" : "LEFT");
        lv_label_set_text(d->label_last, steps > 0 ? "RIGHT" : "LEFT");
        pm_shake();
    } else {
        d->focus = (d->focus + steps) % HASS_DEVICE_NUM;
        if (d->focus < 0) {
            d->focus += HASS_DEVICE_NUM;
        }
        hass_set_focus(d, d->focus);
    }
}

static void pg_hass_on_back(page_t *p)
{
    hass_data_t *d = p->data;
    if (d->in_control) {
        hass_show_control(d, false);
    } else {
        pm_pop();
    }
}

static void pg_hass_on_resume(page_t *p)
{
    hass_data_t *d = p->data;
    motor_set_mode(d->in_control ? MOTOR_MODE_UNBOUND_NO_DETENTS
                                 : MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
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
    .on_resume = pg_hass_on_resume,
};