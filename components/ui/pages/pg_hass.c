#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "page_mgr.h"
#include "motor.h"
#include "mqtt.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * X-Knob 风格智能家居页 (无限循环, 与主菜单同架构)
 * - 4 设备 x 3 副本环形列表, 触摸可滑, 焦点跟随, 旋转连贯循环
 * - 控制视图 (参考 X-Knob PlaygroundView 重做):
 *   渐变光晕圆盘 + 刻度环 + 底部60°状态弧 + 旋转方向点
 *   中央电源图标 + 已开启/已关闭状态字
 *   旋转 = 方向点绕盘移动 + LEFT/RIGHT, 点击 = 开/关切换
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
    lv_obj_t *glow;         /* 表盘背景渐变圆 */
    lv_obj_t *ring;         /* 外圈细弧 */
    lv_obj_t *scale;        /* 刻度环 */
    lv_obj_t *state_arc;    /* 底部 60° 状态弧 */
    lv_obj_t *dot;          /* 旋转方向指示点 */
    lv_obj_t *icon_power;   /* 中央电源图标 */
    lv_obj_t *label_state;  /* 已开启/已关闭 */
    lv_obj_t *label_name;   /* 设备名 */
    int dot_deg;            /* 方向点角度 (0=12点方向, 顺时针) */
    bool dev_on[HASS_DEVICE_NUM];  /* 本地模拟的开/关状态 */
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

/* 控制视图状态刷新: 名字/状态字/图标/弧段/光晕随 开关状态 联动 */
static void hass_ctrl_visual(hass_data_t *d)
{
    bool on = d->dev_on[d->focus];
    lv_label_set_text(d->label_name, device_names[d->focus]);
    lv_label_set_text(d->label_state, on ? "\xE5\xB7\xB2\xE5\xBC\x80\xE5\x90\xAF"   /* 已开启 */
                                         : "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD"); /* 已关闭 */
    lv_color_t c = lv_color_hex(on ? XK_COLOR_ACCENT : XK_COLOR_GRAY);
    lv_obj_set_style_text_color(d->label_state, c, 0);
    lv_obj_set_style_text_color(d->icon_power, c, 0);
    lv_obj_set_style_bg_grad_color(d->glow, lv_color_hex(on ? 0x00329B : 0x181818), 0);
    lv_arc_set_value(d->state_arc, on ? 1 : 0);
}

/* 方向点绕盘移动: 每档 5°, 0 = 12点方向, 顺时针为正 */
static void hass_dot_update(hass_data_t *d)
{
    const float r = 95.0f;   /* 光晕圆边缘(88)与刻度尖端之间 */
    float rad = (float)(d->dot_deg % 360) * 0.01745329f;
    int32_t x = 120 + (int32_t)(r * sinf(rad)) - 5;
    int32_t y = 162 - (int32_t)(r * cosf(rad)) - 5;
    lv_obj_set_pos(d->dot, x, y);
}

static void hass_show_control(hass_data_t *d, bool ctrl)
{
    d->in_control = ctrl;
    if (ctrl) {
        d->dot_deg = 0;
        hass_ctrl_visual(d);
        hass_dot_update(d);
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

/* 控制视图点击 → 开/关切换 */
static void hass_tap_cb(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);
    hass_data_t *d = p->data;
    if (!d->in_control) {
        return;
    }
    bool on = !d->dev_on[d->focus];
    d->dev_on[d->focus] = on;
    mqtt_ha_publish_cmd(device_names[d->focus], on ? "ON" : "OFF");
    mqtt_ha_publish_action(d->focus, on ? "ON" : "OFF");
    hass_ctrl_visual(d);
    pm_shake();
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

    /* ---- 控制视图 (X-Knob 风格表盘) ---- */
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

    /* 表盘背景: 垂直渐变圆, 开启时底部泛蓝光, 关闭时纯暗 (X-Knob 语言) */
    lv_obj_t *glow = lv_obj_create(d->ctrl_scr);
    d->glow = glow;
    lv_obj_remove_style_all(glow);
    lv_obj_set_size(glow, 176, 176);
    lv_obj_set_pos(glow, 32, 74);
    lv_obj_set_style_bg_color(glow, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_bg_grad_color(glow, lv_color_hex(0x181818), 0);
    lv_obj_set_style_bg_grad_dir(glow, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(glow, lv_color_hex(0x242424), 0);
    lv_obj_set_style_border_width(glow, 1, 0);
    lv_obj_clear_flag(glow, LV_OBJ_FLAG_CLICKABLE);

    /* 外圈细弧 */
    lv_obj_t *ring = lv_arc_create(d->ctrl_scr);
    d->ring = ring;
    lv_obj_set_size(ring, 216, 216);
    lv_obj_set_pos(ring, 12, 54);
    lv_arc_set_rotation(ring, 0);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_value(ring, 0);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(ring, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x2E2E2E), LV_PART_MAIN);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);

    /* 刻度环: 73 根, 每 6 根一根长亮刻度 */
    lv_obj_t *scale = lv_scale_create(d->ctrl_scr);
    d->scale = scale;
    lv_obj_set_size(scale, 216, 216);
    lv_obj_set_pos(scale, 12, 54);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_total_tick_count(scale, 73);
    lv_scale_set_major_tick_every(scale, 6);
    lv_scale_set_range(scale, 0, 72);
    lv_scale_set_angle_range(scale, 360);
    lv_scale_set_rotation(scale, 0);
    lv_obj_set_style_length(scale, 7, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x333333), LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 12, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x606060), LV_PART_INDICATOR);
    /* scale 默认可点击且不冒泡, 会吞掉控制视图的"点击=开关" */
    lv_obj_remove_flag(scale, LV_OBJ_FLAG_CLICKABLE);

    /* 底部 60° 状态弧: 开=整段点亮, 关=熄灭 */
    lv_obj_t *sa = lv_arc_create(d->ctrl_scr);
    d->state_arc = sa;
    lv_obj_set_size(sa, 216, 216);
    lv_obj_set_pos(sa, 12, 54);
    lv_arc_set_rotation(sa, 150);
    lv_arc_set_bg_angles(sa, 0, 60);
    lv_arc_set_range(sa, 0, 1);
    lv_arc_set_value(sa, 0);
    lv_obj_remove_style(sa, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(sa, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sa, lv_color_hex(0x2E2E2E), LV_PART_MAIN);
    lv_obj_set_style_arc_width(sa, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sa, lv_color_hex(XK_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_remove_flag(sa, LV_OBJ_FLAG_CLICKABLE);

    /* 中央电源图标 + 状态字 */
    lv_obj_t *icon_power = lv_label_create(d->ctrl_scr);
    d->icon_power = icon_power;
    lv_obj_set_style_text_color(icon_power, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(icon_power, &lv_font_montserrat_48, 0);
    lv_label_set_text(icon_power, LV_SYMBOL_POWER);
    lv_obj_align(icon_power, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *ls = lv_label_create(d->ctrl_scr);
    d->label_state = ls;
    lv_obj_set_style_text_color(ls, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(ls, &lv_font_msyh_16, 0);
    lv_label_set_text(ls, "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD");
    lv_obj_align(ls, LV_ALIGN_CENTER, 0, 32);

    /* 顶部设备名 */
    lv_obj_t *name = lv_label_create(d->ctrl_scr);
    d->label_name = name;
    lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 32);

    /* 底部操作提示 */
    lv_obj_t *hint = lv_label_create(d->ctrl_scr);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE6\x97\x8B\xE8\xBD\xAC\xE8\xB0\x83\xE8\x8A\x82 \xC2\xB7 \xE7\x82\xB9\xE5\x87\xBB\xE5\xBC\x80\xE5\x85\xB3"); /* 旋转调节 · 点击开关 */
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* 旋转方向指示点 (最后创建, 置顶) */
    lv_obj_t *dot = lv_obj_create(d->ctrl_scr);
    d->dot = dot;
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_bg_color(dot, lv_color_hex(XK_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    hass_set_focus(d, 0, 0);
}

static void pg_hass_destroy(page_t *p)
{
    s_hass = NULL;
    hass_data_t *d = p->data;
    if (d) {
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
        d->dot_deg += (int)steps * 5;
        hass_dot_update(d);
        pm_shake();
    } else {
        /* 无级循环: 灯光->空调->风扇->洗衣机->灯光... */
        d->focus = ((d->focus + steps) % HASS_DEVICE_NUM + HASS_DEVICE_NUM) % HASS_DEVICE_NUM;
        hass_set_focus(d, d->focus, (int)steps);
    }
}

/* 控制视图中快转是 LEFT/RIGHT 输入, 禁用甩动返回手势 */
static bool hass_flick_block(page_t *p)
{
    hass_data_t *d = p->data;
    return d->in_control;
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
    .flick_block = hass_flick_block,
};
