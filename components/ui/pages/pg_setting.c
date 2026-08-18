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

typedef struct {
    int focus;
    int edit_item;          /* -1 = list view, else 0/1 */
    int32_t brightness;
    int32_t timeout_min;
    lv_obj_t *rows[2];
    lv_obj_t *hint;
    lv_obj_t *scale;
    lv_obj_t *needle;
    lv_obj_t *label_value;
    lv_obj_t *label_unit;
    lv_timer_t *timer;
} setting_data_t;

#define SET_BRIGHTNESS 0
#define SET_TIMEOUT    1

static void setting_show_edit(setting_data_t *d, int item)
{
    d->edit_item = item;
    lv_obj_add_flag(d->rows[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->rows[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d->scale, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d->label_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d->label_unit, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_clear_flag(d->rows[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d->rows[1], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(d->hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->scale, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->label_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(d->label_unit, LV_OBJ_FLAG_HIDDEN);
    motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);
    pm_shake();
}

static void setting_refresh_focus(setting_data_t *d)
{
    for (int i = 0; i < 2; i++) {
        if (i == d->focus) {
            lv_obj_set_style_border_color(d->rows[i], lv_color_hex(XK_COLOR_RED), 0);
            lv_obj_set_style_border_width(d->rows[i], 2, 0);
            lv_obj_set_style_bg_color(d->rows[i], lv_color_hex(XK_COLOR_PANEL), 0);
            lv_obj_set_style_bg_opa(d->rows[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_border_width(d->rows[i], 0, 0);
            lv_obj_set_style_bg_opa(d->rows[i], LV_OPA_TRANSP, 0);
        }
    }
}

/* 触摸点击设置项 → 进入编辑 */
static void setting_row_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    page_t *p = (page_t *)lv_obj_get_user_data(obj);
    setting_data_t *d = p->data;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (d->edit_item < 0) {
        setting_show_edit(d, idx);
    }
}

/* 编辑视图: 触摸点击 → 保存退出 */
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
    if (d->edit_item == SET_BRIGHTNESS) {
        d->brightness = pos;
        snprintf(buf, sizeof(buf), "%ld", (long)pos);
        lv_label_set_text(d->label_value, buf);
        lv_scale_set_line_needle_value(d->scale, d->needle, 95, pos);
        display_set_brightness(pos);
    } else {
        d->timeout_min = pos;
        snprintf(buf, sizeof(buf), "%ld", (long)pos);
        lv_label_set_text(d->label_value, buf);
        lv_scale_set_line_needle_value(d->scale, d->needle, 95, pos);
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

    d->hint = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(d->hint, &lv_font_msyh_16, 0);
    lv_label_set_text(d->hint, "\xE6\x85\xA2\xE8\xBD\xAC\xE9\x80\x89\xE6\x8B\xA9 \xC2\xB7 \xE5\xBF\xAB\xE6\x97\x8B\xE8\xBF\x9B\xE5\x85\xA5");
    lv_obj_align(d->hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    static const char *titles[2] = {
        "\xE4\xBA\xAE\xE5\xBA\xA6",
        "\xE7\x86\x84\xE5\xB1\x8F\xE6\x97\xB6\xE9\x95\xBF",
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *row = lv_obj_create(p->root);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 240, 54);
        lv_obj_set_pos(row, 0, 60 + i * 54);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(XK_COLOR_RED), 0);
        lv_obj_set_style_border_post(row, true, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, p);
        lv_obj_add_event_cb(row, setting_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *title = lv_label_create(row);
        lv_obj_set_style_text_color(title, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_msyh_16, 0);
        lv_label_set_text(title, titles[i]);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 46, 0);

        lv_obj_t *val = lv_label_create(row);
        lv_obj_set_style_text_color(val, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(val, &lv_font_msyh_16, 0);
        if (i == SET_BRIGHTNESS) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%ld %%", (long)d->brightness);
            lv_label_set_text(val, buf);
        } else {
            char buf[24];
            snprintf(buf, sizeof(buf), "%ld \xE5\x88\x86\xE9\x92\x9F", (long)d->timeout_min);
            lv_label_set_text(val, buf);
        }
        lv_obj_align(val, LV_ALIGN_RIGHT_MID, -12, 0);

        d->rows[i] = row;
    }
    setting_refresh_focus(d);

    d->scale = lv_scale_create(p->root);
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
    lv_obj_add_flag(d->scale, LV_OBJ_FLAG_HIDDEN);

    static lv_point_precise_t needle_points[2] = { {0, 0}, {0, 0} };
    d->needle = lv_line_create(d->scale);
    lv_line_set_points_mutable(d->needle, needle_points, 2);
    lv_obj_set_style_line_width(d->needle, 8, 0);
    lv_obj_set_style_line_rounded(d->needle, true, 0);
    lv_obj_set_style_line_color(d->needle, lv_color_hex(XK_COLOR_ORANGE), 0);
    lv_scale_set_post_draw(d->scale, true);
    lv_scale_set_line_needle_value(d->scale, d->needle, 95, 0);

    d->label_value = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_26, 0);
    lv_label_set_text(d->label_value, "0");
    lv_obj_align(d->label_value, LV_ALIGN_CENTER, 0, 80);
    lv_obj_add_flag(d->label_value, LV_OBJ_FLAG_HIDDEN);

    d->label_unit = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_unit, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_unit, &lv_font_msyh_16, 0);
    lv_label_set_text(d->label_unit, "%");
    lv_obj_align(d->label_unit, LV_ALIGN_CENTER, 0, 120);
    lv_obj_add_flag(d->label_unit, LV_OBJ_FLAG_HIDDEN);

    d->timer = lv_timer_create(setting_timer_cb, 100, d);
    motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);

    /* 编辑视图: 整页点击保存退出 */
    lv_obj_add_flag(p->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->root, setting_edit_tap_cb, LV_EVENT_CLICKED, p);
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
        setting_refresh_focus(d);
    }
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

static void pg_setting_on_tick(page_t *p)
{
}

const page_ops_t pg_setting_ops = {
    .create = pg_setting_create,
    .destroy = pg_setting_destroy,
    .on_rotate = pg_setting_on_rotate,
    .on_back = pg_setting_on_back,
    .on_tick = pg_setting_on_tick,
};