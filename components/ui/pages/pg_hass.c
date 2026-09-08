#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "mqtt.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * X-Knob 风格智能家居页 (无限循环, 与主菜单同架构)
 * - 4 设备 x 3 副本环形列表, 触摸可滑, 焦点跟随, 旋转连贯循环
 * - 控制视图: 全屏表盘(73 刻度 360°, 蓝色指针)
 *   旋转 = LEFT/RIGHT, 点击 = ON/OFF
 * ============================================================ */

#define HASS_DEVICE_NUM 4
#define COPY_N        3
#define ROWS_N        (HASS_DEVICE_NUM * COPY_N)
#define ITEM_H        100
#define ICON_W_OPEN   220
#define ICON_W_FOCUS  70
#define ANIM_MS       180
#define SWIPE_PX      20

static const char *device_names[HASS_DEVICE_NUM] = {
    "\xE7\x81\xAF\xE5\x85\x89",   /* 灯光 */
    "\xE7\xA9\xBA\xE8\xB0\x83",   /* 空调 */
    "\xE9\xA3\x8E\xE6\x89\x87",   /* 风扇 */
    "\xE6\xB4\x97\xE8\xA1\xA3\xE6\x9C\xBA", /* 洗衣机 */
};

static const char *device_icons[HASS_DEVICE_NUM] = {
    LV_SYMBOL_POWER,    /* 灯光: 电源/开关 */
    LV_SYMBOL_TINT,     /* 空调: 冷凝水滴 */
    LV_SYMBOL_REFRESH,  /* 风扇: 旋转叶片 */
    LV_SYMBOL_LOOP,     /* 洗衣机: 滚筒循环 */
};

typedef struct {
    int focus;
    int cur_row;
    bool anim_lock;
    bool in_control;
    lv_obj_t *list;
    lv_obj_t *rows[ROWS_N];
    lv_obj_t *icons[ROWS_N];
    lv_obj_t *infos[ROWS_N];
    lv_obj_t *ctrl_scr;     /* 控制视图 */
    lv_obj_t *scale;        /* 控制视图表盘 */
    lv_obj_t *needle;
    lv_obj_t *label_name;
    lv_obj_t *label_last;
    lv_timer_t *timer;
} hass_data_t;

static hass_data_t *s_hass;   /* 行回调取实例用 (单实例页面) */

static void hass_anim_width(lv_obj_t *icon, int32_t target)
{
    int32_t cur = lv_obj_get_width(icon);
    if (cur == target) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, icon);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_width);
    lv_anim_set_values(&a, cur, target);
    lv_anim_set_duration(&a, ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 焦点视觉切换 (宽度动画 + 主题边 + 右侧信息), 不动滚动 */
static void hass_focus_visual(hass_data_t *d, int idx)
{
    if (idx < 0 || idx >= HASS_DEVICE_NUM) {
        return;
    }
    d->focus = idx;
    for (int k = 0; k < HASS_DEVICE_NUM; k++) {
        bool f = (k == idx);
        for (int c = 0; c < COPY_N; c++) {
            int r = c * HASS_DEVICE_NUM + k;
            hass_anim_width(d->icons[r], f ? ICON_W_FOCUS : ICON_W_OPEN);
            lv_obj_set_style_border_width(d->icons[r], f ? 2 : 0, 0);
            if (f) {
                lv_obj_remove_flag(d->infos[r], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(d->infos[r], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void hass_set_focus(hass_data_t *d, int idx, int dir)
{
    if (idx < 0 || idx >= HASS_DEVICE_NUM) {
        return;
    }
    hass_focus_visual(d, idx);
    int nr = d->cur_row + dir;
    if (dir == 0 || nr < 0 || nr >= ROWS_N || (nr % HASS_DEVICE_NUM) != idx) {
        nr = HASS_DEVICE_NUM + idx;
    }
    d->cur_row = nr;
    lv_obj_t *root = lv_obj_get_parent(d->rows[nr]);
    lv_obj_update_layout(root);
    int32_t vh = lv_obj_get_height(root);
    if (vh < ITEM_H) {
        vh = 3 * ITEM_H;
    }
    int32_t target = (int32_t)nr * ITEM_H - (vh - ITEM_H) / 2;
    d->anim_lock = (lv_obj_get_scroll_y(root) != target);
    lv_obj_scroll_to_y(root, target, LV_ANIM_ON);
}

/* 触摸滑动: 焦点跟随视觉居中的行 */
static void hass_scroll_follow_cb(lv_event_t *e)
{
    hass_data_t *d = lv_event_get_user_data(e);
    if (d->anim_lock || d->in_control) {
        return;
    }
    lv_obj_t *root = lv_event_get_target(e);
    int32_t vh = lv_obj_get_height(root);
    if (vh < ITEM_H) {
        return;
    }
    int32_t y = lv_obj_get_scroll_y(root);
    int32_t nr = (y + (vh - ITEM_H) / 2 + ITEM_H / 2) / ITEM_H;
    if (nr < 0) {
        nr = 0;
    }
    if (nr >= ROWS_N) {
        nr = ROWS_N - 1;
    }
    int idx = nr % HASS_DEVICE_NUM;
    if (idx != d->focus) {
        hass_focus_visual(d, idx);
    }
}

/* 滚动停止后归位中间副本 (像素相同, 跳变无感) */
static void hass_scroll_wrap_cb(lv_event_t *e)
{
    hass_data_t *d = lv_event_get_user_data(e);
    d->anim_lock = false;
    lv_obj_t *root = lv_event_get_target(e);
    int32_t vh = lv_obj_get_height(root);
    if (vh < ITEM_H) {
        vh = 3 * ITEM_H;
    }
    int32_t base = (int32_t)HASS_DEVICE_NUM * ITEM_H - (vh - ITEM_H) / 2;
    int32_t y = lv_obj_get_scroll_y(root);
    int32_t ny = y;
    if (ny >= base + HASS_DEVICE_NUM * ITEM_H) {
        ny -= HASS_DEVICE_NUM * ITEM_H;
    } else if (ny < base) {
        ny += HASS_DEVICE_NUM * ITEM_H;
    }
    d->cur_row = HASS_DEVICE_NUM + d->focus;
    if (ny != y) {
        lv_obj_scroll_to_y(root, ny, LV_ANIM_OFF);
    }
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

/* 点击设备行 → 控制视图 (滑动 >20px 不算点击) */
static lv_point_t press_pt;

static void hass_row_press_cb(lv_event_t *e)
{
    lv_indev_get_point(lv_indev_active(), &press_pt);
}

static void hass_row_cb(lv_event_t *e)
{
    lv_point_t now;
    lv_indev_get_point(lv_indev_active(), &now);
    int dx = now.x - press_pt.x;
    int dy = now.y - press_pt.y;
    if (dx * dx + dy * dy > SWIPE_PX * SWIPE_PX) {
        return;   /* 滑动, 不是点击 */
    }
    hass_data_t *d = s_hass;
    if (d == NULL || d->in_control) {
        return;
    }
    int idx = (int)(intptr_t)lv_event_get_user_data(e) % HASS_DEVICE_NUM;
    hass_focus_visual(d, idx);
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
    s_hass = d;
    d->focus = 0;
    d->in_control = false;
    p->title = "\xE6\x99\xBA\xE8\x83\xBD\xE5\xAE\xB6\xE5\xB1\x85";

    /* 设备列表: 独立滚动容器(flex), 控制视图作为根上浮层, 互不影响 */
    lv_obj_t *list = lv_obj_create(p->root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 240, 320);
    lv_obj_set_pos(list, 0, 0);
    d->list = list;
    /* 无内外边距: 12 行纯周期排列, 循环跳变才能像素级无缝 */
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(list, hass_scroll_follow_cb, LV_EVENT_SCROLL, d);
    lv_obj_add_event_cb(list, hass_scroll_wrap_cb, LV_EVENT_SCROLL_END, d);

    for (int i = 0; i < ROWS_N; i++) {
        int k = i % HASS_DEVICE_NUM;

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 220, ITEM_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, hass_row_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, hass_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        d->rows[i] = row;

        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, ICON_W_OPEN, ITEM_H);
        lv_obj_set_style_bg_color(icon, lv_color_hex(XK_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        /* 图标默认 CLICKABLE 且铺满整行, 会吞掉行点击 —— 必须关闭 */
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_align(icon, LV_ALIGN_LEFT_MID, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(icon, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(icon, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_side(icon, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_color(icon, lv_color_hex(XK_COLOR_ACCENT), 0);
        lv_obj_set_style_border_post(icon, true, 0);

        lv_obj_t *img = lv_label_create(icon);
        lv_obj_set_style_text_color(img, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(img, &lv_font_montserrat_26, 0);
        lv_label_set_text(img, device_icons[k]);

        lv_obj_t *name = lv_label_create(icon);
        lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
        lv_label_set_text(name, device_names[k]);
        d->icons[i] = icon;

        lv_obj_t *info = lv_label_create(row);
        lv_obj_set_style_text_color(info, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(info, &lv_font_msyh_16, 0);
        lv_label_set_text(info, "\xE7\x82\xB9\xE5\x87\xBB\xE6\x8E\xA7\xE5\x88\xB6");
        lv_obj_set_width(info, 132);
        lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(info, LV_ALIGN_LEFT_MID, ICON_W_FOCUS + 10, 0);
        lv_obj_add_flag(info, LV_OBJ_FLAG_HIDDEN);
        d->infos[i] = info;

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
    hass_set_focus(d, 0, 0);
}

static void pg_hass_destroy(page_t *p)
{
    s_hass = NULL;
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
        /* 无级循环: 灯光->空调->风扇->洗衣机->灯光... */
        d->focus = ((d->focus + steps) % HASS_DEVICE_NUM + HASS_DEVICE_NUM) % HASS_DEVICE_NUM;
        hass_set_focus(d, d->focus, (int)steps);
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
    if (!d->in_control) {
        hass_set_focus(d, d->focus, 0);
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
    .on_resume = pg_hass_on_resume,
};
