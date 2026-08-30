#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "display.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_msyh_16);

/* NVS persistence helpers from smartknob_ui.c */
bool ui_nvs_load_i32(const char *key, int32_t *out);
void ui_nvs_save_i32(const char *key, int32_t value);

/* ============================================================
 * X-Knob 风格设置页
 * - 列表: 与主菜单同款"聚焦展开"行(100px)
 * - 编辑: 全屏环形刻度, 旋转调节, 点击保存
 * ============================================================ */

#define ROW_H 100
#define LIST_PAD ((320 - ROW_H) / 2)
#define ICON_W_OPEN   220
#define ICON_W_FOCUS  70

typedef struct {
    int focus;
    int edit_item;          /* -1 = 列表, 否则 0/1 */
    int32_t brightness;
    int32_t timeout_min;
    lv_obj_t *list;         /* 设置列表滚动容器 */
    lv_obj_t *rows[2];
    lv_obj_t *icons[2];
    lv_obj_t *val_labels[2];
    lv_obj_t *edit_scr;
    lv_obj_t *scale;
    lv_obj_t *needle;
    lv_obj_t *label_value;
    lv_obj_t *label_unit;
    lv_timer_t *timer;
} setting_data_t;

#define SET_BRIGHTNESS 0
#define SET_TIMEOUT    1

static void setting_refresh_rows(setting_data_t *d)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld %%", (long)d->brightness);
    lv_label_set_text(d->val_labels[SET_BRIGHTNESS], buf);
    snprintf(buf, sizeof(buf), "%ld \xE5\x88\x86\xE9\x92\x9F", (long)d->timeout_min);
    lv_label_set_text(d->val_labels[SET_TIMEOUT], buf);

    for (int i = 0; i < 2; i++) {
        if (i == d->focus) {
            lv_obj_add_state(d->icons[i], LV_STATE_FOCUSED);
        } else {
            lv_obj_remove_state(d->icons[i], LV_STATE_FOCUSED);
        }
    }
    lv_obj_scroll_to_view(d->rows[d->focus], LV_ANIM_ON);
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
        lv_label_set_text(d->label_unit, "\xE5\x88\x86\xE9\x92\x9F");
        motor_set_mode_range(MOTOR_MODE_FINE_DETENTS, 0, 30, init);
    }
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
    setting_refresh_rows(d);
    motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);
    pm_shake();
}

/* 点击设置项 → 进入编辑 */
static void setting_row_cb(lv_event_t *e)
{
    lv_obj_t *row = lv_event_get_current_target(e);
    page_t *p = (page_t *)lv_obj_get_user_data(row);
    setting_data_t *d = p->data;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (d->edit_item < 0) {
        d->focus = idx;
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
    if (d->edit_item < 0) return;

    int32_t pos = motor_get_position();
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", (long)pos);
    lv_label_set_text(d->label_value, buf);
    lv_scale_set_line_needle_value(d->scale, d->needle, 95, pos);
    if (d->edit_item == SET_BRIGHTNESS) {
        d->brightness = pos;
        display_set_brightness(pos);   /* 实时预览 */
    } else {
        d->timeout_min = pos;
    }
}

static void pg_setting_create(page_t *p)
{
    setting_data_t *d = calloc(1, sizeof(setting_data_t));
    p->data = d;
    d->focus = 0;
    d->edit_item = -1;
    d->brightness = display_get_brightness();
    d->timeout_min = display_get_screen_timeout() / 60;
    p->title = "\xE8\xAE\xBE\xE7\xBD\xAE";

    static const char *names[2] = {
        "\xE4\xBA\xAE\xE5\xBA\xA6",                 /* 亮度 */
        "\xE7\x86\x84\xE5\xB1\x8F\xE6\x97\xB6\xE9\x95\xBF", /* 熄屏时长 */
    };
    static const char *icons[2] = {
        LV_SYMBOL_DOWN,
        LV_SYMBOL_BELL,
    };
    static const char *descs[2] = {
        "\xE5\xB1\x8F\xE5\xB9\x95\xE8\x83\x8C\xE5\x85\x89" "\n10 - 100 %",
        "\xE8\x87\xAA\xE5\x8A\xA8\xE7\x86\x84\xE5\xB1\x8F" "\n0 - 30 \xE5\x88\x86\xE9\x92\x9F",
    };

    /* 列表: 独立滚动容器(flex), 编辑视图作为根上浮层, 互不影响 */
    lv_obj_t *list = lv_obj_create(p->root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 240, 320);
    lv_obj_set_pos(list, 0, 0);
    d->list = list;
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(list, LIST_PAD, 0);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 220, ROW_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, p);
        lv_obj_add_event_cb(row, setting_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        d->rows[i] = row;

        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, ICON_W_OPEN, ROW_H);
        lv_obj_set_style_bg_color(icon, lv_color_hex(XK_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_align(icon, LV_ALIGN_LEFT_MID, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(icon, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(icon, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_set_style_width(icon, ICON_W_FOCUS, LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(icon, LV_BORDER_SIDE_RIGHT, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(icon, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(icon, lv_color_hex(XK_COLOR_RED), LV_STATE_FOCUSED);

        static lv_style_transition_dsc_t trans;
        static const lv_style_prop_t props[] = { LV_STYLE_WIDTH, LV_STYLE_PROP_INV };
        lv_style_transition_dsc_init(&trans, props, lv_anim_path_overshoot, 200, 0, NULL);
        lv_obj_set_style_transition(icon, &trans, LV_STATE_FOCUSED);
        lv_obj_set_style_transition(icon, &trans, 0);

        lv_obj_t *img = lv_label_create(icon);
        lv_obj_set_style_text_color(img, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(img, &lv_font_montserrat_26, 0);
        lv_label_set_text(img, icons[i]);

        lv_obj_t *name = lv_label_create(icon);
        lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
        lv_label_set_text(name, names[i]);
        d->icons[i] = icon;

        /* 当前值 (右侧, 灰) */
        lv_obj_t *val = lv_label_create(row);
        lv_obj_set_style_text_color(val, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(val, &lv_font_msyh_16, 0);
        lv_obj_align(val, LV_ALIGN_LEFT_MID, ICON_W_FOCUS + 5, -26);
        d->val_labels[i] = val;

        lv_obj_t *desc = lv_label_create(row);
        lv_obj_set_style_text_color(desc, lv_color_hex(XK_COLOR_FAINT), 0);
        lv_obj_set_style_text_font(desc, &lv_font_msyh_16, 0);
        lv_label_set_text(desc, descs[i]);
        lv_obj_align(desc, LV_ALIGN_LEFT_MID, ICON_W_FOCUS + 5, -4);

        lv_obj_move_foreground(icon);
    }

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

    d->scale = lv_scale_create(d->edit_scr);
    lv_obj_set_pos(d->scale, 0, 50);
    lv_obj_set_size(d->scale, 240, 240);
    lv_obj_set_style_bg_color(d->scale, lv_color_hex(XK_COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(d->scale, lv_color_make(48, 48, 0), 0);
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
    lv_scale_set_total_tick_count(d->scale, 41);
    lv_scale_set_major_tick_every(d->scale, 1);
    lv_scale_set_range(d->scale, 0, 100);
    lv_scale_set_angle_range(d->scale, 270);
    lv_scale_set_rotation(d->scale, 225);

    static lv_point_precise_t needle_points[2] = { {0, 0}, {0, 0} };
    d->needle = lv_line_create(d->scale);
    lv_line_set_points_mutable(d->needle, needle_points, 2);
    lv_obj_set_style_line_width(d->needle, 8, 0);
    lv_obj_set_style_line_rounded(d->needle, true, 0);
    lv_obj_set_style_line_color(d->needle, lv_color_hex(XK_COLOR_ORANGE), 0);
    lv_scale_set_post_draw(d->scale, true);
    lv_scale_set_line_needle_value(d->scale, d->needle, 95, 0);
    /* scale 默认可点击且不冒泡, 会吞掉编辑视图的"点击=保存" */
    lv_obj_remove_flag(d->scale, LV_OBJ_FLAG_CLICKABLE);

    d->label_value = lv_label_create(d->edit_scr);
    lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_26, 0);
    lv_label_set_text(d->label_value, "0");
    lv_obj_align(d->label_value, LV_ALIGN_CENTER, 0, 70);

    d->label_unit = lv_label_create(d->edit_scr);
    lv_obj_set_style_text_color(d->label_unit, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_unit, &lv_font_msyh_16, 0);
    lv_label_set_text(d->label_unit, "%");
    lv_obj_align(d->label_unit, LV_ALIGN_CENTER, 0, 110);

    lv_obj_t *hint = lv_label_create(d->edit_scr);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE6\x97\x8B\xE8\xBD\xAC\xE8\xB0\x83\xE8\x8A\x82 \xC2\xB7 \xE7\x82\xB9\xE5\x87\xBB\xE4\xBF\x9D\xE5\xAD\x98");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    d->timer = lv_timer_create(setting_timer_cb, 100, d);
    motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);
    setting_refresh_rows(d);
}

static void pg_setting_destroy(page_t *p)
{
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
        d->focus = (d->focus + steps) % 2;
        if (d->focus < 0) {
            d->focus += 2;
        }
        setting_refresh_rows(d);
    }
    /* 编辑模式: 值由电机档位控制, timer 读取 */
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
        motor_set_mode_range(MOTOR_MODE_FINE_DETENTS, 0, 30, d->timeout_min);
    } else {
        motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);
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
};