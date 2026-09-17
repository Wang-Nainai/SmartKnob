#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "page_mgr.h"
#include "motor.h"
#include "display.h"
#include "blehid.h"
#include "esp_system.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

/* NVS persistence helpers from smartknob_ui.c */
bool ui_nvs_load_i32(const char *key, int32_t *out);
void ui_nvs_save_i32(const char *key, int32_t value);

/* ============================================================
 * X-Knob 风格设置页 (无限循环, 与主菜单同架构)
 * - 4 项 x 3 副本环形列表: 亮度->熄屏时长->系统监控->蓝牙->亮度...
 *   触摸可滑, 焦点跟随, 旋转 220<->70 宽度动画 + 居中跟随
 * - 编辑: 全屏环形刻度, 旋转调节, 点击保存
 * ============================================================ */

#define SET_N         5
#define COPY_N        3
#define ROWS_N        (SET_N * COPY_N)
#define ITEM_H        100
#define ICON_W_OPEN   220
#define ICON_W_FOCUS  70
#define ANIM_MS       180
#define SWIPE_PX      20

#define SET_BRIGHTNESS 0
#define SET_TIMEOUT    1
#define SET_SYSMON     2
#define SET_BLE        3
#define SET_CAL        4

typedef struct {
    int focus;
    int cur_row;
    bool anim_lock;
    int edit_item;          /* -1 = 列表, 否则 SET_x */
    int32_t brightness;
    int32_t ble_feedback_until;
    int32_t timeout_min;
    lv_obj_t *list;
    lv_obj_t *rows[ROWS_N];
    lv_obj_t *icons[ROWS_N];
    lv_obj_t *val_labels[ROWS_N];
    lv_obj_t *desc_labels[ROWS_N];
    lv_obj_t *edit_scr;
    lv_obj_t *dial;         /* 填充弧 (值比例) */
    lv_obj_t *bezel;        /* 外围刻度圈 */
    lv_obj_t *label_value;
    lv_obj_t *label_unit;
    lv_timer_t *timer;
} setting_data_t;

static const char *set_names[SET_N] = {
    "\xE4\xBA\xAE\xE5\xBA\xA6",                         /* 亮度 */
    "\xE7\x86\x84\xE5\xB1\x8F\xE6\x97\xB6\xE9\x95\xBF", /* 熄屏时长 */
    "\xE7\xB3\xBB\xE7\xBB\x9F\xE7\x9B\x91\xE6\x8E\xA7", /* 系统监控 */
    "\xE8\x93\x9D\xE7\x89\x99",                         /* 蓝牙 */
    "\xE9\x87\x8D\xE6\x96\xB0\xE6\xA0\xA1\xE5\x87\x86", /* 重新校准 */
};
static const char *set_icons[SET_N] = {
    LV_SYMBOL_EYE_OPEN,   /* 亮度 */
    LV_SYMBOL_BELL,       /* 熄屏时长 */
    LV_SYMBOL_BARS,       /* 系统监控 */
    LV_SYMBOL_BLUETOOTH,  /* 蓝牙 */
    LV_SYMBOL_REFRESH,    /* 重新校准 */
};
static const char *set_descs[SET_N] = {
    "\xE5\xB1\x8F\xE5\xB9\x95\xE8\x83\x8C\xE5\x85\x89" "\n10 - 100 %",
    "\xE8\x87\xAA\xE5\x8A\xA8\xE7\x86\x84\xE5\xB1\x8F" "\n0 - 30 \xE5\x88\x86\xE9\x92\x9F",
    "CPU / RAM / \xE4\xBB\xBB\xE5\x8A\xA1\xE8\xA1\xA8",
    "\xE6\xB8\x85\xE9\x99\xA4\xE5\xB7\xB2\xE9\x85\x8D\xE5\xAF\xB9\xE8\xAE\xBE\xE5\xA4\x87",
    "\xE7\x94\xB5\xE6\x9C\xBA\xE9\x9B\xB6\xE4\xBD\x8D\xE6\xA0\xA1\xE5\x87\x86" "\n\xE5\x8F\x8C\xE5\x87\xBB\xE9\x87\x8D\xE5\x90\xAF\xE6\x89\xA7\xE8\xA1\x8C", /* 电机零位校准\n双击重启执行 */
};

static void setting_anim_width(lv_obj_t *icon, int32_t target)
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
static void setting_focus_visual(setting_data_t *d, int idx)
{
    if (idx < 0 || idx >= SET_N) {
        return;
    }
    d->focus = idx;
    for (int k = 0; k < SET_N; k++) {
        bool f = (k == idx);
        for (int c = 0; c < COPY_N; c++) {
            int r = c * SET_N + k;
            setting_anim_width(d->icons[r], f ? ICON_W_FOCUS : ICON_W_OPEN);
            lv_obj_set_style_border_width(d->icons[r], f ? 2 : 0, 0);
            if (f) {
                lv_obj_remove_flag(d->val_labels[r], LV_OBJ_FLAG_HIDDEN);
                lv_obj_remove_flag(d->desc_labels[r], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(d->val_labels[r], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(d->desc_labels[r], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

static void setting_set_focus(setting_data_t *d, int idx, int dir)
{
    if (idx < 0 || idx >= SET_N) {
        return;
    }
    setting_focus_visual(d, idx);
    int nr = d->cur_row + dir;
    if (dir == 0 || nr < 0 || nr >= ROWS_N || (nr % SET_N) != idx) {
        nr = SET_N + idx;
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
static void setting_scroll_follow_cb(lv_event_t *e)
{
    setting_data_t *d = lv_event_get_user_data(e);
    if (d->anim_lock || d->edit_item >= 0) {
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
    int idx = nr % SET_N;
    if (idx != d->focus) {
        setting_focus_visual(d, idx);
    }
}

/* 滚动停止后归位中间副本 (像素相同, 跳变无感) */
static void setting_scroll_wrap_cb(lv_event_t *e)
{
    setting_data_t *d = lv_event_get_user_data(e);
    d->anim_lock = false;
    lv_obj_t *root = lv_event_get_target(e);
    int32_t vh = lv_obj_get_height(root);
    if (vh < ITEM_H) {
        vh = 3 * ITEM_H;
    }
    int32_t base = (int32_t)SET_N * ITEM_H - (vh - ITEM_H) / 2;
    int32_t y = lv_obj_get_scroll_y(root);
    int32_t ny = y;
    if (ny >= base + SET_N * ITEM_H) {
        ny -= SET_N * ITEM_H;
    } else if (ny < base) {
        ny += SET_N * ITEM_H;
    }
    d->cur_row = SET_N + d->focus;
    if (ny != y) {
        lv_obj_scroll_to_y(root, ny, LV_ANIM_OFF);
    }
}

/* 值文本更新到三份副本 */
static void setting_set_val(setting_data_t *d, int item, const char *text)
{
    for (int c = 0; c < COPY_N; c++) {
        lv_label_set_text(d->val_labels[c * SET_N + item], text);
    }
}

static void setting_refresh_vals(setting_data_t *d)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld %%", (long)d->brightness);
    setting_set_val(d, SET_BRIGHTNESS, buf);
    if (d->timeout_min == 0) {
        setting_set_val(d, SET_TIMEOUT, "\xE5\xB8\xB8\xE4\xBA\xAE");   /* 常亮 */
    } else {
        snprintf(buf, sizeof(buf), "%ld \xE5\x88\x86\xE9\x92\x9F", (long)d->timeout_min);
        setting_set_val(d, SET_TIMEOUT, buf);
    }
    setting_set_val(d, SET_SYSMON, "\xE8\xBF\x9B\xE5\x85\xA5");   /* 进入 */
    setting_set_val(d, SET_BLE, "");
}

/* 编辑视图表盘刷新: 大值/单位/填充弧 联动 (填充弧即指示, 无圆点) */
static void setting_edit_visual(setting_data_t *d)
{
    int32_t val;
    int vmin, vmax;
    char buf[24];
    if (d->edit_item == SET_BRIGHTNESS) {
        val = d->brightness;
        vmin = 10;
        vmax = 100;
        lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_48, 0);
        snprintf(buf, sizeof(buf), "%ld", (long)val);
        lv_label_set_text(d->label_value, buf);
        lv_obj_clear_flag(d->label_unit, LV_OBJ_FLAG_HIDDEN);
    } else {
        val = d->timeout_min;
        vmin = 0;
        vmax = 30;
        if (val == 0) {
            /* 数字字体无中文字形, 常亮切中文字库 */
            lv_obj_set_style_text_font(d->label_value, &lv_font_msyh_16, 0);
            lv_label_set_text(d->label_value, "\xE5\xB8\xB8\xE4\xBA\xAE");
            lv_obj_add_flag(d->label_unit, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_48, 0);
            snprintf(buf, sizeof(buf), "%ld", (long)val);
            lv_label_set_text(d->label_value, buf);
            lv_obj_clear_flag(d->label_unit, LV_OBJ_FLAG_HIDDEN);
        }
    }
    int deg = (int)(((int64_t)(val - vmin) * 360) / (vmax - vmin));
    if (deg > 360) deg = 360;
    lv_arc_set_angles(d->dial, 0, deg);
}

static void setting_show_edit(setting_data_t *d, int item)
{
    d->edit_item = item;
    lv_obj_clear_flag(d->edit_scr, LV_OBJ_FLAG_HIDDEN);

    int32_t init = item == SET_BRIGHTNESS ? d->brightness : d->timeout_min;
    if (item == SET_BRIGHTNESS) {
        lv_label_set_text(d->label_unit, "%");
        motor_set_mode_range(MOTOR_MODE_FINE_DETENTS, 10, 100, init);
    } else {
        /* 31 档 × 8.23° ≈ 255° 全程: 细微模式(1°/档)只挤在 31° 里, 手感太差 */
        lv_label_set_text(d->label_unit, "\xE5\x88\x86\xE9\x92\x9F");
        motor_set_mode_range(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 30, init);
    }
    setting_edit_visual(d);
    pm_shake();
}

static void setting_exit_edit(setting_data_t *d, bool save)
{
    if (save) {
        if (d->edit_item == SET_BRIGHTNESS) {
            display_set_brightness(d->brightness);
            ui_nvs_save_i32("brightness", d->brightness);
        } else {
            display_set_screen_timeout(d->timeout_min * 60);
            ui_nvs_save_i32("timeout", d->timeout_min);
        }
    }
    d->edit_item = -1;
    lv_obj_add_flag(d->edit_scr, LV_OBJ_FLAG_HIDDEN);
    setting_refresh_vals(d);
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    pm_shake();
}

static setting_data_t *s_setting;   /* 行回调取实例用 (单实例页面) */

/* 点击设置项 (滑动 >20px 不算点击) */
static lv_point_t press_pt;

static void setting_row_press_cb(lv_event_t *e)
{
    lv_indev_get_point(lv_indev_active(), &press_pt);
}

static void cal_restart_timer_cb(lv_timer_t *t)
{
    (void)t;
    esp_restart();
}

static void setting_row_cb(lv_event_t *e)
{
    lv_point_t now;
    lv_indev_get_point(lv_indev_active(), &now);
    int dx = now.x - press_pt.x;
    int dy = now.y - press_pt.y;
    if (dx * dx + dy * dy > SWIPE_PX * SWIPE_PX) {
        return;   /* 滑动, 不是点击 */
    }
    setting_data_t *d = s_setting;
    if (d == NULL || d->edit_item >= 0) {
        return;
    }
    int idx = (int)(intptr_t)lv_event_get_user_data(e) % SET_N;
    setting_focus_visual(d, idx);
    if (idx == SET_SYSMON) {
        pm_push(PAGE_SYSMON);
        pm_shake();
    } else if (idx == SET_CAL) {
        /* 电机校准重学: 双击确认 -> 清 NVS -> 重启开机校准 */
        static int32_t cal_pending_until = 0;
        int32_t now = (int32_t)lv_tick_get();
        if (now < cal_pending_until) {
            cal_pending_until = 0;
            motor_clear_calibration();
            setting_set_val(d, SET_CAL,
                            "\xE6\xA0\xA1\xE5\x87\x86\xE4\xB8\xAD\xE5\xB0\x86\xE9\x87\x8D\xE5\x90\xAF"); /* 校准中即将重启 */
            /* 非阻塞延迟重启: 让提示先渲染, 不得在 LVGL 回调中 vTaskDelay */
            lv_timer_create(cal_restart_timer_cb, 600, NULL);
        } else {
            cal_pending_until = now + 4000;
            setting_set_val(d, SET_CAL,
                            "\xE5\x86\x8D\xE7\x82\xB9\xE4\xB8\x80\xE6\xAC\xA1\xE7\xA1\xAE\xE8\xAE\xA4"); /* 再点一次确认 */
        }
    } else if (idx == SET_BLE) {
        /* 清除蓝牙配对: 双击确认防误触 */
        static int32_t pending_until = 0;
        int32_t now = (int32_t)lv_tick_get();
        if (now < pending_until) {
            blehid_unpair_all();
            pending_until = 0;
            setting_set_val(d, SET_BLE, "\xE5\xB7\xB2\xE6\xB8\x85\xE9\x99\xA4"); /* 已清除 */
            d->ble_feedback_until = now + 3000;
            pm_shake();
        } else {
            pending_until = now + 4000;
            setting_set_val(d, SET_BLE,
                            "\xE5\x86\x8D\xE7\x82\xB9\xE4\xB8\x80\xE6\xAC\xA1\xE7\xA1\xAE\xE8\xAE\xA4");
        }
    } else {
        setting_show_edit(d, idx);
    }
}

/* 编辑视图点击 → 保存退出 */
static void setting_edit_tap_cb(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);
    setting_data_t *d = p->data;
    if (d->edit_item >= 0) {
        setting_exit_edit(d, true);
    }
}

static void setting_timer_cb(lv_timer_t *t)
{
    setting_data_t *d = lv_timer_get_user_data(t);
    /* 蓝牙清除反馈: 到时清掉提示文本 */
    if (d->ble_feedback_until != 0 && (int32_t)lv_tick_get() > d->ble_feedback_until) {
        setting_set_val(d, SET_BLE, "");
        d->ble_feedback_until = 0;
    }
    if (d->edit_item < 0) return;

    int32_t pos = motor_get_position();
    if (d->edit_item == SET_BRIGHTNESS) {
        /* 快转瞬间端点制动未拉回, position 可短暂越界(9/101):
         * 不钳制会把非法值写进 NVS 并显示 "9 %"/"101 %" */
        if (pos < 10) pos = 10;
        if (pos > 100) pos = 100;
        d->brightness = pos;
        display_set_brightness(pos);   /* 实时预览 */
    } else {
        if (pos < 0) pos = 0;
        if (pos > 30) pos = 30;
        d->timeout_min = pos;
    }
    setting_edit_visual(d);
}

static void pg_setting_create(page_t *p)
{
    setting_data_t *d = calloc(1, sizeof(setting_data_t));
    p->data = d;
    s_setting = d;
    d->focus = 0;
    d->edit_item = -1;
    d->brightness = display_get_brightness();
    d->timeout_min = display_get_screen_timeout() / 60;
    p->title = "\xE8\xAE\xBE\xE7\xBD\xAE";

    /* 列表: 独立滚动容器(flex), 编辑视图作为根上浮层, 互不影响 */
    lv_obj_t *list = lv_obj_create(p->root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 240, 320);
    lv_obj_set_pos(list, 0, 0);
    d->list = list;
    /* 无内外边距: 12 行纯周期排列, 循环跳变才能像素级无缝 */
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(list, setting_scroll_follow_cb, LV_EVENT_SCROLL, d);
    lv_obj_add_event_cb(list, setting_scroll_wrap_cb, LV_EVENT_SCROLL_END, d);

    for (int i = 0; i < ROWS_N; i++) {
        int k = i % SET_N;

        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 220, ITEM_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, setting_row_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, setting_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
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
        lv_label_set_text(img, set_icons[k]);

        lv_obj_t *name = lv_label_create(icon);
        lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
        lv_label_set_text(name, set_names[k]);
        d->icons[i] = icon;

        /* 当前值 (聚焦行右侧上部, 与描述同左缘对齐) */
        lv_obj_t *val = lv_label_create(row);
        lv_obj_set_style_text_color(val, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(val, &lv_font_msyh_16, 0);
        lv_label_set_text(val, "");
        lv_obj_align(val, LV_ALIGN_LEFT_MID, ICON_W_FOCUS + 10, -30);
        lv_obj_add_flag(val, LV_OBJ_FLAG_HIDDEN);
        d->val_labels[i] = val;

        /* 描述 (聚焦行右侧下部) */
        lv_obj_t *desc = lv_label_create(row);
        lv_obj_set_style_text_color(desc, lv_color_hex(XK_COLOR_FAINT), 0);
        lv_obj_set_style_text_font(desc, &lv_font_msyh_16, 0);
        lv_label_set_text(desc, set_descs[k]);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, ICON_W_FOCUS + 10, 2);
        lv_obj_add_flag(desc, LV_OBJ_FLAG_HIDDEN);
        d->desc_labels[i] = desc;

        lv_obj_move_foreground(icon);
    }

    setting_refresh_vals(d);

    /* ---- 编辑视图 (全屏环形) ---- */
    d->edit_scr = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->edit_scr);
    lv_obj_set_size(d->edit_scr, 240, 320);
    lv_obj_set_pos(d->edit_scr, 0, 0);
    lv_obj_set_style_bg_color(d->edit_scr, lv_color_hex(XK_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(d->edit_scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(d->edit_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->edit_scr, setting_edit_tap_cb, LV_EVENT_CLICKED, p);
    lv_obj_set_user_data(d->edit_scr, p);
    lv_obj_add_flag(d->edit_scr, LV_OBJ_FLAG_HIDDEN);

    /* ---- 编辑视图表盘 (与智能家居/手感页同款 Apple 风格) ----
     * 外圈刻度圈 -> 全周暗环+填充弧 -> 内层渐变圆 -> 主题蓝圆点 -> 中央大值 */

    d->bezel = lv_scale_create(d->edit_scr);
    lv_obj_set_size(d->bezel, 232, 232);
    lv_obj_set_pos(d->bezel, 4, 52);
    lv_scale_set_mode(d->bezel, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(d->bezel, false);
    lv_scale_set_total_tick_count(d->bezel, 73);
    lv_scale_set_major_tick_every(d->bezel, 6);
    lv_scale_set_range(d->bezel, 0, 72);
    lv_scale_set_angle_range(d->bezel, 360);
    lv_scale_set_rotation(d->bezel, 0);
    lv_obj_set_style_length(d->bezel, 5, LV_PART_ITEMS);
    lv_obj_set_style_line_width(d->bezel, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(d->bezel, lv_color_hex(0x2E2E2E), LV_PART_ITEMS);
    lv_obj_set_style_length(d->bezel, 10, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(d->bezel, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(d->bezel, lv_color_hex(0x5A5A5A), LV_PART_INDICATOR);
    /* scale 默认可点击且不冒泡, 会吞掉编辑视图的"点击=保存" */
    lv_obj_remove_flag(d->bezel, LV_OBJ_FLAG_CLICKABLE);

    d->dial = lv_arc_create(d->edit_scr);
    lv_obj_set_size(d->dial, 204, 204);
    lv_obj_set_pos(d->dial, 18, 66);
    lv_arc_set_rotation(d->dial, 0);
    lv_arc_set_bg_angles(d->dial, 0, 360);
    lv_arc_set_range(d->dial, 0, 100);
    lv_arc_set_value(d->dial, 0);
    lv_obj_remove_style(d->dial, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(d->dial, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(d->dial, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(d->dial, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(d->dial, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(d->dial, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_set_style_arc_color(d->dial, lv_color_hex(XK_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_remove_flag(d->dial, LV_OBJ_FLAG_CLICKABLE);

    /* 内层渐变圆 (下沉感) */
    lv_obj_t *inner = lv_obj_create(d->edit_scr);
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

    d->label_value = lv_label_create(d->edit_scr);
    lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->label_value, "0");
    lv_obj_align(d->label_value, LV_ALIGN_CENTER, 0, 0);

    d->label_unit = lv_label_create(d->edit_scr);
    lv_obj_set_style_text_color(d->label_unit, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_unit, &lv_font_msyh_16, 0);
    lv_label_set_text(d->label_unit, "%");
    lv_obj_align(d->label_unit, LV_ALIGN_CENTER, 0, 58);

    lv_obj_t *hint = lv_label_create(d->edit_scr);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE6\x97\x8B\xE8\xBD\xAC\xE8\xB0\x83\xE8\x8A\x82 \xC2\xB7 \xE7\x82\xB9\xE5\x87\xBB\xE4\xBF\x9D\xE5\xAD\x98");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    d->timer = lv_timer_create(setting_timer_cb, 100, d);
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    setting_set_focus(d, 0, 0);
}

static void pg_setting_destroy(page_t *p)
{
    s_setting = NULL;
    setting_data_t *d = p->data;
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        free(d);
    }
    p->data = NULL;
}

static void pg_setting_on_rotate(page_t *p, int32_t steps)
{
    setting_data_t *d = p->data;
    if (d->edit_item < 0) {
        /* 无级循环: 亮度->熄屏->监控->蓝牙->亮度... */
        d->focus = ((d->focus + steps) % SET_N + SET_N) % SET_N;
        setting_set_focus(d, d->focus, (int)steps);
    }
    /* 编辑模式: 值由电机档位控制, timer 读取 */
}

/* 编辑模式中快转是调值输入, 禁用甩动返回手势 */
static bool setting_flick_block(page_t *p)
{
    setting_data_t *d = p->data;
    return d->edit_item >= 0;
}

static void pg_setting_on_back(page_t *p)
{
    setting_data_t *d = p->data;
    if (d->edit_item >= 0) {
        setting_exit_edit(d, false);
    } else {
        pm_pop();
    }
}

static void pg_setting_on_resume(page_t *p)
{
    setting_data_t *d = p->data;
    if (d->edit_item == SET_BRIGHTNESS) {
        motor_set_mode_range(MOTOR_MODE_FINE_DETENTS, 10, 100, d->brightness);
    } else if (d->edit_item == SET_TIMEOUT) {
        motor_set_mode_range(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 30, d->timeout_min);
    } else {
        motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
        setting_set_focus(d, d->focus, 0);
    }
}

static void pg_setting_on_tick(page_t *p)
{
}

const page_ops_t pg_setting_ops = {
    .create = pg_setting_create,
    .destroy = pg_setting_destroy,
    .on_rotate = pg_setting_on_rotate,
    .on_back = pg_setting_on_back,
    .on_tick = pg_setting_on_tick,
    .on_resume = pg_setting_on_resume,
    .flick_block = setting_flick_block,
};
