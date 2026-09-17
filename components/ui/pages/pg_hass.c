#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "page_mgr.h"
#include "motor.h"
#include "mqtt.h"
#include "hass_cfg.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * 智能家居页 (无限循环, 与主菜单同架构)
 * - 设备列表来自 Web 配置 (hass_cfg, dev1..devN), 上限 6 台
 * - N 设备 x 3 副本环形列表, 触摸可滑, 焦点跟随, 旋转连贯循环
 * - 控制视图 (Apple Home / watchOS 风格):
 *   灯: 开/关两档, 旋转与点击同效 (圆点停开位/关位)
 *   空调: 点图标圆底切温度/风速模式, 旋转=每度一档/风速象限
 *   温度=比例填充弧, 开/关与风速=圆点定位 (填充与圆点互斥)
 * ============================================================ */

#define COPY_N        3
#define ROWS_MAX      (HASS_MAX_DEVICES * COPY_N)
#define ITEM_H        100
#define ICON_W_OPEN   220
#define ICON_W_FOCUS  70
#define ANIM_MS       180
#define SWIPE_PX      20

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
    lv_obj_t *rows[ROWS_MAX];
    lv_obj_t *icons[ROWS_MAX];
    lv_obj_t *infos[ROWS_MAX];
    lv_obj_t *ctrl_scr;     /* 控制视图 */
    lv_obj_t *dial;         /* 圆环: 暗色轨道 */
    lv_obj_t *dot;          /* 旋转指示圆点 (主题深蓝, 绕环移动) */
    lv_obj_t *bezel;        /* 外围细刻度圈 (X-Knob 语言) */
    lv_obj_t *icon_circle;  /* 中央图标圆底 */
    lv_obj_t *icon;         /* 设备图标 */
    lv_obj_t *label_state;  /* 已开启/已关闭 */
    lv_obj_t *label_name;   /* 设备名 */
    lv_obj_t *label_hint;   /* 底部操作提示(按设备变化) */
    int num;                                       /* 已配置设备数 1..6 */
    hass_device_cfg_t devs[HASS_MAX_DEVICES];      /* 设备列表(Web 配置) */
    bool dev_on[HASS_MAX_DEVICES];  /* 本地模拟的开/关状态 */
    int ac_temp[HASS_MAX_DEVICES];  /* 空调本地模拟温度 16..30 (按设备独立) */
    int ac_fan[HASS_MAX_DEVICES];   /* 空调风速 0自动 1低 2中 3高 (按设备独立) */
    int ac_mode[HASS_MAX_DEVICES];  /* 空调调节模式 0=温度 1=风速 (按设备独立) */
    lv_timer_t *timer;      /* 结算发布: 旋转停止 250ms 后发一次绝对值 */
    uint32_t last_move_tick;
    int last_pub_temp[HASS_MAX_DEVICES];  /* 上次已发布的值(去重, 按设备独立) */
    int last_pub_fan[HASS_MAX_DEVICES];
} hass_data_t;

static hass_data_t *s_hass;   /* 行回调取实例用 (单实例页面) */

/* 设备类型判定/图标 */
static bool hass_is_ac(hass_data_t *d, int idx)
{
    return d->devs[idx].type == HASS_TYPE_AC;
}

static const char *hass_icon_by_type(const hass_device_cfg_t *dev)
{
    return dev->type == HASS_TYPE_AC ? LV_SYMBOL_TINT : LV_SYMBOL_EYE_OPEN;
}

/* 设备 level 通道键: dev<序号>_temp / dev<序号>_fan */
static void hass_level_key(hass_data_t *d, int idx, bool fan, char *out, size_t size)
{
    snprintf(out, size, "dev%d_%s", idx + 1, fan ? "fan" : "temp");
}

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
    if (idx < 0 || idx >= d->num) {
        return;
    }
    d->focus = idx;
    for (int k = 0; k < d->num; k++) {
        bool f = (k == idx);
        for (int c = 0; c < COPY_N; c++) {
            int r = c * d->num + k;
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
    if (idx < 0 || idx >= d->num) {
        return;
    }
    hass_focus_visual(d, idx);
    int nr = d->cur_row + dir;
    if (dir == 0 || nr < 0 || nr >= ROWS_MAX || (nr % d->num) != idx) {
        nr = d->num + idx;
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
    if (nr >= ROWS_MAX) {
        nr = ROWS_MAX - 1;
    }
    int idx = nr % d->num;
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
    int32_t base = (int32_t)d->num * ITEM_H - (vh - ITEM_H) / 2;
    int32_t y = lv_obj_get_scroll_y(root);
    int32_t ny = y;
    if (ny >= base + d->num * ITEM_H) {
        ny -= d->num * ITEM_H;
    } else if (ny < base) {
        ny += d->num * ITEM_H;
    }
    d->cur_row = d->num + d->focus;
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

/* 空调电机模式: 温度 16..30 -> 15 个档位 (0..14), 风速 4 档 (0..3)
 * 状态按设备槽位独立(多台空调不互串) */
static void hass_ac_motor_mode(hass_data_t *d)
{
    int f = d->focus;
    if (d->ac_mode[f] == 0) {
        motor_set_mode_range(MOTOR_MODE_ADJUSTER, 0, 14, d->ac_temp[f] - 16);
    } else {
        motor_set_mode_range(MOTOR_MODE_ADJUSTER, 0, 3, d->ac_fan[f]);
    }
}

/* 结算发布: 空调节值(温度/风速)在旋转静止 250ms 后发布一次绝对值,
 * 惯性滑过若干档也只发终值, HA 不会收到一串中间值.
 * 断线时不能标记已发布 —— 否则重连后该值永远丢失 */
static void hass_flush_pending(hass_data_t *d)
{
    int f = d->focus;
    if (!mqtt_ha_is_connected()) {
        return;
    }
    if (!hass_is_ac(d, f)) {
        return;
    }
    char key[16];
    if (d->ac_mode[f] == 0) {
        if (d->ac_temp[f] != d->last_pub_temp[f]) {
            hass_level_key(d, f, false, key, sizeof(key));
            mqtt_ha_publish_level(key, d->ac_temp[f]);
            d->last_pub_temp[f] = d->ac_temp[f];
        }
    } else {
        if (d->ac_fan[f] != d->last_pub_fan[f]) {
            hass_level_key(d, f, true, key, sizeof(key));
            mqtt_ha_publish_level(key, d->ac_fan[f]);
            d->last_pub_fan[f] = d->ac_fan[f];
        }
    }
}

static void hass_timer_cb(lv_timer_t *t)
{
    hass_data_t *d = lv_timer_get_user_data(t);
    if (!d->in_control || !hass_is_ac(d, d->focus)) {
        return;
    }
    if ((uint32_t)(lv_tick_get() - d->last_move_tick) < 250) {
        return;   /* 还在转 */
    }
    hass_flush_pending(d);
}

/* 指示圆点: 仅用于"纯位置指示"(灯开/关, 空调风速象限).
 * 有填充弧的状态(空调温度)由弧本身指示, 不放点避免叠加冲突 */
static void hass_update_dot(hass_data_t *d)
{
    int f = d->focus;
    if (!hass_is_ac(d, f)) {
        hass_dot_at(d, d->dev_on[f] ? 300 : 240);
        lv_obj_clear_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
    } else if (d->ac_mode[f] == 1) {
        hass_dot_at(d, d->ac_fan[f] * 90);
        lv_obj_clear_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 控制视图状态刷新: 状态字/图标圆底/圆环填充/提示 联动 */
static void hass_ctrl_visual(hass_data_t *d)
{
    int f = d->focus;
    bool on = d->dev_on[f];
    bool is_ac = hass_is_ac(d, f);
    lv_label_set_text(d->label_name, d->devs[f].name);
    lv_label_set_text(d->label_state, on ? "\xE5\xB7\xB2\xE5\xBC\x80\xE5\x90\xAF"   /* 已开启 */
                                         : "\xE5\xB7\xB2\xE5\x85\xB3\xE9\x97\xAD"); /* 已关闭 */
    lv_obj_set_style_text_color(d->label_state, lv_color_hex(on ? XK_COLOR_TEXT : XK_COLOR_GRAY), 0);
    lv_obj_set_style_bg_color(d->icon_circle, lv_color_hex(on ? XK_COLOR_ACCENT : 0x1C1C1E), 0);
    lv_obj_set_style_text_color(d->icon, lv_color_hex(on ? XK_COLOR_TEXT : 0x8E8E93), 0);
    if (is_ac) {
        if (d->ac_mode[f] == 0) {
            /* 温度: 圆环按 16..30 比例填充, 30° = 满环 */
            int deg = (d->ac_temp[f] - 16) * 360 / 14;
            if (deg > 360) deg = 360;
            lv_obj_set_style_arc_color(d->dial, lv_color_hex(on ? XK_COLOR_ACCENT : 0x3A3A3A), LV_PART_INDICATOR);
            lv_arc_set_angles(d->dial, 0, deg);
            lv_obj_set_style_text_font(d->icon, &lv_font_montserrat_48, 0);
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", d->ac_temp[f]);
            lv_label_set_text(d->icon, buf);
        } else {
            /* 风速: 不填充, 圆点停在四个象限位置 */
            lv_arc_set_angles(d->dial, 0, 0);
            lv_obj_set_style_text_font(d->icon, &lv_font_msyh_16, 0);
            lv_label_set_text(d->icon, ac_fan_names[d->ac_fan[f]]);
        }
        lv_label_set_text(d->label_hint, "\xE7\x82\xB9\xE5\x9B\xBE\xE6\xA0\x87\xE5\x88\x87\xE6\x8D\xA2\xE6\xB8\xA9\xE5\xBA\xA6/\xE9\xA3\x8E\xE9\x80\x9F"); /* 点图标切换温度/风速 */
    } else {
        /* 灯只有开/关: 圆点停在"开"位(300°)或"关"位(240°) */
        lv_arc_set_angles(d->dial, 0, 0);
        lv_obj_set_style_text_font(d->icon, &lv_font_montserrat_48, 0);
        lv_label_set_text(d->icon, hass_icon_by_type(&d->devs[d->focus]));
        lv_label_set_text(d->label_hint, "\xE7\x82\xB9\xE5\x87\xBB\xE5\xBC\x80\xE5\x85\xB3 \xC2\xB7 \xE6\x97\x8B\xE8\xBD\xAC\xE5\x90\x8C\xE6\x95\x88"); /* 点击开关 · 旋转同效 */
    }
    hass_update_dot(d);
}

static void hass_show_control(hass_data_t *d, bool ctrl)
{
    d->in_control = ctrl;
    if (ctrl) {
        hass_ctrl_visual(d);
        lv_obj_clear_flag(d->ctrl_scr, LV_OBJ_FLAG_HIDDEN);
        if (hass_is_ac(d, d->focus)) {
            /* 温度/风速: 每度一个档位 */
            hass_ac_motor_mode(d);
        } else {
            /* 灯: 开/关两档, 初始档位=当前状态 */
            motor_set_mode_range(MOTOR_MODE_ON_OFF, 0, 1, d->dev_on[d->focus] ? 1 : 0);
        }
    } else {
        lv_obj_add_flag(d->ctrl_scr, LV_OBJ_FLAG_HIDDEN);
        hass_flush_pending(d);   /* 退出控制视图前把未结算的值发出去 */
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
    int idx = (int)(intptr_t)lv_event_get_user_data(e) % d->num;
    hass_focus_visual(d, idx);
    hass_show_control(d, true);
    pm_shake();
}

/* 开/关切换 (所有设备) */
static void hass_toggle_power(hass_data_t *d)
{
    bool on = !d->dev_on[d->focus];
    d->dev_on[d->focus] = on;
    mqtt_ha_publish_cmd(d->devs[d->focus].name, on ? "ON" : "OFF");
    mqtt_ha_publish_action(d->focus, on ? "on" : "off");
    /* 灯: 电机同步到对应档位, 圆点与状态一致 */
    if (!hass_is_ac(d, d->focus)) {
        motor_set_position(on ? 1 : 0);
    }
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
    if (hass_is_ac(d, d->focus)) {
        hass_flush_pending(d);   /* 切模式前把未结算的值发出去 */
        d->ac_mode[d->focus] ^= 1;
        hass_ctrl_visual(d);
        hass_ac_motor_mode(d);   /* 模式切换后电机档位跟着换 */
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
    /* 设备列表: Web 配置驱动 (缺省回退 卧室灯/客厅灯/过道灯/卧室空调) */
    d->num = hass_cfg_load(d->devs, HASS_MAX_DEVICES);
    for (int i = 0; i < d->num; i++) {
        d->dev_on[i] = false;
    }
    p->title = "\xE6\x99\xBA\xE8\x83\xBD\xE5\xAE\xB6\xE5\xB1\x85";

    /* 设备列表: 独立滚动容器(flex), 控制视图作为根上浮层, 互不影响 */
    lv_obj_t *list = lv_obj_create(p->root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 240, 320);
    lv_obj_set_pos(list, 0, 0);
    d->list = list;
    /* 无内外边距: N*3 行纯周期排列, 循环跳变才能像素级无缝 */
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(list, hass_scroll_follow_cb, LV_EVENT_SCROLL, d);
    lv_obj_add_event_cb(list, hass_scroll_wrap_cb, LV_EVENT_SCROLL_END, d);

    int rows_n = d->num * COPY_N;
    for (int i = 0; i < rows_n; i++) {
        int k = i % d->num;

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
        lv_label_set_text(img, hass_icon_by_type(&d->devs[k]));

        lv_obj_t *name = lv_label_create(icon);
        lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
        lv_label_set_text(name, d->devs[k].name);
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

    /* 圆环: 全周暗色轨道(圆头) + 填充指示(宽度必须打开, 颜色随状态在
     * hass_ctrl_visual 中设置; 圆点负责的位置指示见 hass_update_dot) */
    lv_obj_t *dial = lv_arc_create(d->ctrl_scr);
    d->dial = dial;
    lv_obj_set_size(dial, 204, 204);
    lv_obj_set_pos(dial, 18, 66);
    lv_arc_set_rotation(dial, 0);
    lv_arc_set_bg_angles(dial, 0, 360);
    lv_arc_set_range(dial, 0, 100);
    lv_arc_set_value(dial, 0);
    lv_obj_remove_style(dial, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(dial, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(dial, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(dial, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(dial, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(dial, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_set_style_arc_color(dial, lv_color_hex(XK_COLOR_ACCENT), LV_PART_INDICATOR);
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
    lv_label_set_text(icon, hass_icon_by_type(&d->devs[0]));
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
    /* 顶部设备名 (y=28: 与表盘顶 y=66 保持 16px 间距, 34 时仅 10px 贴边) */
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 28);

    /* 底部操作提示 (按设备变化, 在 hass_ctrl_visual 中更新) */
    lv_obj_t *hint = lv_label_create(d->ctrl_scr);
    d->label_hint = hint;
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);   /* 再往下一点: -10 时与圆盘底部间距过大 */

    for (int i = 0; i < d->num; i++) {
        d->ac_temp[i] = 26;
        d->ac_fan[i] = 0;
        d->ac_mode[i] = 0;
        d->last_pub_temp[i] = 26;
        d->last_pub_fan[i] = 0;
    }
    d->last_move_tick = 0;
    hass_ctrl_visual(d);
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
        if (hass_is_ac(d, d->focus)) {
            /* 空调: 电机档位即值, 界面实时跟随; MQTT 由 timer 结算后发布终值.
             * 快转瞬间端点制动尚未拉回, position 可短暂越界(15/-1/4),
             * 必须钳制 —— 否则 ac_temp 显示非法温度、ac_fan 越界索引
             * ac_fan_names[4] 数组 */
            int f = d->focus;
            int32_t pos = motor_get_position();
            if (d->ac_mode[f] == 0) {
                int t = 16 + pos;
                if (t < 16) t = 16;
                if (t > 30) t = 30;
                if (t != d->ac_temp[f]) {
                    d->ac_temp[f] = t;
                    hass_ctrl_visual(d);
                    d->last_move_tick = lv_tick_get();
                }
            } else {
                if (pos < 0) pos = 0;
                if (pos > 3) pos = 3;
                if (pos != d->ac_fan[f]) {
                    d->ac_fan[f] = pos;
                    hass_ctrl_visual(d);
                    d->last_move_tick = lv_tick_get();
                }
            }
        } else {
            /* 灯: 转动即控制开关 —— 顺时针=开, 逆时针=关 (电机两档, 位置即状态) */
            int32_t pos = motor_get_position();
            bool on = (pos != 0);
            if (on != d->dev_on[d->focus]) {
                d->dev_on[d->focus] = on;
                mqtt_ha_publish_cmd(d->devs[d->focus].name, on ? "ON" : "OFF");
                mqtt_ha_publish_action(d->focus, on ? "on" : "off");
                hass_ctrl_visual(d);
                pm_shake();
            }
        }
    } else {
        /* 无级循环: 按配置设备数循环 */
        d->focus = ((d->focus + steps) % d->num + d->num) % d->num;
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
    if (d->in_control && hass_is_ac(d, d->focus)) {
        /* 空调: 温度/风速档位恢复 */
        hass_ac_motor_mode(d);
    } else if (d->in_control) {
        /* 灯: 恢复开/关两档, 档位对齐当前状态 */
        motor_set_mode_range(MOTOR_MODE_ON_OFF, 0, 1, d->dev_on[d->focus] ? 1 : 0);
    } else {
        motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    }
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
