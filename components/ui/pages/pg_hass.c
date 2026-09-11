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
 * - 控制视图 (Apple Home / watchOS 风格):
 *   单根粗圆头圆环 (暗轨 + 16° 旋转指示弧, 无刻度)
 *   中央软色圆底设备图标 + 已开启/已关闭状态字
 *   开启态: 圆底/指示弧染主题蓝, 文字点亮
 *   旋转 = 指示弧绕环移动 + LEFT/RIGHT, 点击 = 开/关切换
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
    "\xE5\x8D\xA7\xE5\xAE\xA4\xE7\x81\xAF",               /* 卧室灯 */
    "\xE5\xAE\xA2\xE5\x8E\x85\xE7\x81\xAF",               /* 客厅灯 */
    "\xE8\xBF\x87\xE9\x81\x93\xE7\x81\xAF",               /* 过道灯 */
    "\xE5\x8D\xA7\xE5\xAE\xA4\xE7\xA9\xBA\xE8\xB0\x83",   /* 卧室空调 */
};

#define HASS_DEV_AC 3   /* 槽位 3 = 空调(开关/温度/风速) */

static const char *device_icons[HASS_DEVICE_NUM] = {
    LV_SYMBOL_EYE_OPEN, /* 灯: 亮起 */
    LV_SYMBOL_EYE_OPEN,
    LV_SYMBOL_EYE_OPEN,
    LV_SYMBOL_TINT,     /* 空调: 冷凝水滴 */
};

static const char *ac_fan_names[4] = {
    "\xE8\x87\xAA\xE5\x8A\xA8", /* 自动 */
    "\xE4\xBD\x8E\xE9\x80\x9F", /* 低速 */
    "\xE4\xB8\xAD\xE9\x80\x9F", /* 中速 */
    "\xE9\xAB\x98\xE9\x80\x9F", /* 高速 */
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
    lv_obj_t *dial;         /* 圆环: 暗色轨道 */
    lv_obj_t *dot;          /* 旋转指示圆点 (主题深蓝, 绕环移动) */
    lv_obj_t *bezel;        /* 外围细刻度圈 (X-Knob 语言) */
    lv_obj_t *icon_circle;  /* 中央图标圆底 */
    lv_obj_t *icon;         /* 设备图标 */
    lv_obj_t *label_state;  /* 已开启/已关闭 */
    lv_obj_t *label_name;   /* 设备名 */
    lv_obj_t *label_hint;   /* 底部操作提示(按设备变化) */
    int dot_deg;            /* 指示圆点角度 (0=12点方向, 顺时针) */
    bool dev_on[HASS_DEVICE_NUM];  /* 本地模拟的开/关状态 */
    int ac_temp;            /* 空调本地模拟温度 16..30 */
    int ac_fan;             /* 空调风速 0自动 1低 2中 3高 */
    int ac_mode;            /* 空调调节模式 0=温度 1=风速 */
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

/* 指示圆点放到指定角度 (骑在轨道中线 r=96, 0 = 12点方向) */
static void hass_dot_at(hass_data_t *d, int deg)
{
    deg = ((deg % 360) + 360) % 360;
    float rad = (float)deg * 0.01745329f;
    lv_obj_set_pos(d->dot, 120 + (int32_t)(96.0f * sinf(rad)) - 6,
                          168 - (int32_t)(96.0f * cosf(rad)) - 6);
}

static void hass_indic_update(hass_data_t *d)
{
    hass_dot_at(d, d->dot_deg);
}

/* 控制视图状态刷新: 状态字/图标圆底/圆环填充/提示 联动 */
static void hass_ctrl_visual(hass_data_t *d)
{
    bool on = d->dev_on[d->focus];
    bool is_ac = (d->focus == HASS_DEV_AC);
    lv_label_set_text(d->label_name, device_names[d->focus]);
    lv_label_set_text(d->label_state, on ? "\xE5\xB7\xB2\xE5\xBC\x80\xE5\x90\xAF"   /* 已开启 */
                                         : "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD"); /* 已关闭 */
    lv_obj_set_style_text_color(d->label_state, lv_color_hex(on ? XK_COLOR_TEXT : XK_COLOR_GRAY), 0);
    lv_obj_set_style_bg_color(d->icon_circle, lv_color_hex(on ? XK_COLOR_ACCENT : 0x1C1C1E), 0);
    lv_obj_set_style_text_color(d->icon, lv_color_hex(on ? XK_COLOR_TEXT : 0x8E8E93), 0);
    if (is_ac) {
        /* 圆底内直接显示当前调节值: 温度=大数字, 风速=档位文字; 填充弧隐藏 */
        lv_arc_set_angles(d->dial, 0, 0);
        if (d->ac_mode == 0) {
            lv_obj_set_style_text_font(d->icon, &lv_font_montserrat_48, 0);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", d->ac_temp);
            lv_label_set_text(d->icon, buf);
        } else {
            lv_obj_set_style_text_font(d->icon, &lv_font_msyh_16, 0);
            lv_label_set_text(d->icon, ac_fan_names[d->ac_fan]);
        }
        lv_label_set_text(d->label_hint, "\xE7\x82\xB9\xE5\x9B\xBE\xE6\xA0\x87\xE5\x88\x87\xE6\x8D\xA2\xE6\xB8\xA9\xE5\xBA\xA6/\xE9\xA3\x8E\xE9\x80\x9F"); /* 点图标切换温度/风速 */
        hass_indic_update(d);
    } else {
        /* 灯只有开/关: 静态圆环, 无填充弧无指示点, 转动无动作 */
        lv_arc_set_angles(d->dial, 0, 0);
        lv_obj_add_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(d->icon, &lv_font_montserrat_48, 0);
        lv_label_set_text(d->icon, device_icons[d->focus]);
        lv_label_set_text(d->label_hint, "\xE7\x82\xB9\xE5\x87\xBB\xE5\xBC\x80\xE5\x85\xB3"); /* 点击开关 */
    }
}

static void hass_show_control(hass_data_t *d, bool ctrl)
{
    d->in_control = ctrl;
    if (ctrl) {
        d->dot_deg = 0;
        hass_ctrl_visual(d);
        lv_obj_clear_flag(d->ctrl_scr, LV_OBJ_FLAG_HIDDEN);
        /* 棘轮档: 每转一档"咔哒"一声 = 一步调节, 旋转才有意义 */
        motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
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

/* 开/关切换 (所有设备) */
static void hass_toggle_power(hass_data_t *d)
{
    bool on = !d->dev_on[d->focus];
    d->dev_on[d->focus] = on;
    mqtt_ha_publish_cmd(device_names[d->focus], on ? "ON" : "OFF");
    mqtt_ha_publish_action(d->focus, on ? "on" : "off");
    hass_ctrl_visual(d);
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
    hass_toggle_power(d);
}

/* 中央图标圆底点击: 空调=切换 温度/风速 模式, 灯=开/关 */
static void hass_icon_cb(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);
    hass_data_t *d = p->data;
    if (!d->in_control) {
        return;
    }
    if (d->focus == HASS_DEV_AC) {
        d->ac_mode ^= 1;
        hass_ctrl_visual(d);
        pm_shake();
    } else {
        hass_toggle_power(d);
    }
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

    /* ---- 控制视图 (Apple Home / watchOS 风格) ---- */
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

    /* 圆环: 全周暗色轨道(圆头), 指示由独立圆点承担 */
    lv_obj_t *dial = lv_arc_create(d->ctrl_scr);
    d->dial = dial;
    lv_obj_set_size(dial, 204, 204);
    lv_obj_set_pos(dial, 18, 66);
    lv_arc_set_rotation(dial, 0);
    lv_arc_set_bg_angles(dial, 0, 360);
    lv_arc_set_range(dial, 0, 100);
    lv_arc_set_value(dial, 0);
    lv_obj_remove_style(dial, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(dial, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(dial, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(dial, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(dial, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_remove_flag(dial, LV_OBJ_FLAG_CLICKABLE);

    /* 旋转指示圆点: 主题深蓝, 骑在轨道中线上 (最后创建置顶) */
    lv_obj_t *dot = lv_obj_create(d->ctrl_scr);
    d->dot = dot;
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_bg_color(dot, lv_color_hex(XK_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    /* 外围刻度圈: 73 根细刻度(每6根一根长亮), 与圆环同心, 当"表圈"用 */
    lv_obj_t *bezel = lv_scale_create(d->ctrl_scr);
    d->bezel = bezel;
    lv_obj_set_size(bezel, 232, 232);
    lv_obj_set_pos(bezel, 4, 52);
    lv_scale_set_mode(bezel, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(bezel, false);
    lv_scale_set_total_tick_count(bezel, 73);
    lv_scale_set_major_tick_every(bezel, 6);
    lv_scale_set_range(bezel, 0, 72);
    lv_scale_set_angle_range(bezel, 360);
    lv_scale_set_rotation(bezel, 0);
    lv_obj_set_style_length(bezel, 5, LV_PART_ITEMS);
    lv_obj_set_style_line_width(bezel, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(bezel, lv_color_hex(0x2E2E2E), LV_PART_ITEMS);
    lv_obj_set_style_length(bezel, 10, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(bezel, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(bezel, lv_color_hex(0x5A5A5A), LV_PART_INDICATOR);
    lv_obj_remove_flag(bezel, LV_OBJ_FLAG_CLICKABLE);

    /* 内层深度圆: 极暗的微渐变, 制造表盘下沉感 */
    lv_obj_t *inner = lv_obj_create(d->ctrl_scr);
    lv_obj_remove_style_all(inner);
    lv_obj_set_size(inner, 176, 176);
    lv_obj_set_pos(inner, 32, 80);
    lv_obj_set_style_bg_color(inner, lv_color_hex(0x0E0E10), 0);
    lv_obj_set_style_bg_grad_color(inner, lv_color_hex(0x17171B), 0);
    lv_obj_set_style_bg_grad_dir(inner, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(inner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(inner, lv_color_hex(0x232327), 0);
    lv_obj_set_style_border_width(inner, 1, 0);
    lv_obj_clear_flag(inner, LV_OBJ_FLAG_CLICKABLE);

    /* 中央图标圆底 (开启=主题蓝, 关闭=暗灰), 空调模式时可点切换调节模式 */
    lv_obj_t *ic = lv_obj_create(d->ctrl_scr);
    d->icon_circle = ic;
    lv_obj_remove_style_all(ic);
    lv_obj_set_size(ic, 80, 80);
    lv_obj_set_pos(ic, 80, 128);
    lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ic, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ic, 1, 0);
    lv_obj_set_style_border_color(ic, lv_color_hex(0x2A2A2C), 0);
    lv_obj_add_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ic, hass_icon_cb, LV_EVENT_CLICKED, p);

    lv_obj_t *icon = lv_label_create(ic);
    d->icon = icon;
    lv_obj_set_style_text_color(icon, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_label_set_text(icon, device_icons[0]);
    /* 标签若可点击会抢走圆底的命中目标, 必须关闭 */
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(icon);

    /* 中央偏下状态字 */
    lv_obj_t *ls = lv_label_create(d->ctrl_scr);
    d->label_state = ls;
    lv_obj_set_style_text_color(ls, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(ls, &lv_font_msyh_16, 0);
    lv_label_set_text(ls, "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD");
    lv_obj_align(ls, LV_ALIGN_CENTER, 0, 62);

    /* 顶部设备名 */
    lv_obj_t *name = lv_label_create(d->ctrl_scr);
    d->label_name = name;
    lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 34);

    /* 底部操作提示 (按设备变化, 在 hass_ctrl_visual 中更新) */
    lv_obj_t *hint = lv_label_create(d->ctrl_scr);
    d->label_hint = hint;
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    d->ac_temp = 26;
    d->ac_fan = 0;
    d->ac_mode = 0;
    hass_ctrl_visual(d);

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
        int dir = steps > 0 ? 1 : -1;
        if (d->focus == HASS_DEV_AC) {
            /* 只有空调用旋转调节: 温度±1 / 风速档位循环 */
            if (d->ac_mode == 0) {
                /* 温度模式: 本地模拟 16..30, 每事件 1 度 */
                d->ac_temp += dir;
                if (d->ac_temp < 16) d->ac_temp = 16;
                if (d->ac_temp > 30) d->ac_temp = 30;
                mqtt_ha_publish_action(d->focus, dir > 0 ? "temp_up" : "temp_down");
            } else {
                /* 风速模式: 自动/低/中/高 循环 */
                d->ac_fan = ((d->ac_fan + dir) % 4 + 4) % 4;
                mqtt_ha_publish_action(d->focus, dir > 0 ? "fan_up" : "fan_down");
            }
            d->dot_deg += dir * 5;
            hass_ctrl_visual(d);
            hass_indic_update(d);
            pm_shake();
        }
        /* 灯只有开/关: 转动无动作 */
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
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    if (!d->in_control) {
        hass_set_focus(d, d->focus, 0);
    } else {
        hass_ctrl_visual(d);
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
