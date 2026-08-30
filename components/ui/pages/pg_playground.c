#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"

LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

typedef struct {
    int mode;
    lv_obj_t *scale;
    lv_obj_t *needle;
    lv_obj_t *arc;
    lv_obj_t *label_value;
    lv_obj_t *label_mode;
    lv_timer_t *timer;
} pg_data_t;

/* X-Knob geometry: bound arc from 200 to 340 degrees, starts at 120 */
#define ARC_START_ROTATION 120
#define SCALE_LEFT_BOUND_DEG 200
#define SCALE_ANGLE_RANGE 140

static void pg_apply_mode(pg_data_t *d)
{
    int m = d->mode;
    int count = motor_get_mode_count();

    char buf[48];
    snprintf(buf, sizeof(buf), "%s   %d / %d", motor_mode_name(m), m + 1, count);
    lv_label_set_text(d->label_mode, buf);

    switch (m) {
    case MOTOR_MODE_ON_OFF: {
        lv_scale_set_total_tick_count(d->scale, 3);
        lv_scale_set_major_tick_every(d->scale, 1);
        lv_scale_set_range(d->scale, 0, 1);
        lv_scale_set_angle_range(d->scale, 60);
        lv_scale_set_rotation(d->scale, 240);
        break;
    }
    case MOTOR_MODE_UNBOUND_NO_DETENTS:
    case MOTOR_MODE_MULTI_TURN_NO_DETENTS:
    case MOTOR_MODE_AUTO_RETURN_CENTER: {
        lv_scale_set_total_tick_count(d->scale, 73);
        lv_scale_set_major_tick_every(d->scale, 1);
        lv_scale_set_range(d->scale, 0, 72);
        lv_scale_set_angle_range(d->scale, 360);
        lv_scale_set_rotation(d->scale, 270);
        break;
    }
    default: {
        int32_t max = 31;
        int32_t min = 0;
        if (m == MOTOR_MODE_BOUND_NO_DETENTS) max = 10;
        if (m == MOTOR_MODE_FINE_NO_DETENTS || m == MOTOR_MODE_FINE_DETENTS) max = 255;
        if (m == MOTOR_MODE_RETURN_CENTER_WITH_DETENTS) { min = -6; max = 6; }
        lv_scale_set_total_tick_count(d->scale, 13);
        lv_scale_set_major_tick_every(d->scale, 1);
        lv_scale_set_range(d->scale, min, max);
        lv_scale_set_angle_range(d->scale, SCALE_ANGLE_RANGE);
        lv_scale_set_rotation(d->scale, SCALE_LEFT_BOUND_DEG);
        break;
    }
    }

    motor_set_mode(m, 0, 0);
}

static void pg_playground_timer(lv_timer_t *t)
{
    pg_data_t *d = lv_timer_get_user_data(t);
    int32_t pos = motor_get_position();
    float off = motor_get_angle_offset_deg();
    int m = motor_get_mode();

    int32_t needle_val;
    switch (m) {
    case MOTOR_MODE_ON_OFF:
        needle_val = pos;
        break;
    case MOTOR_MODE_UNBOUND_NO_DETENTS:
    case MOTOR_MODE_MULTI_TURN_NO_DETENTS:
    case MOTOR_MODE_AUTO_RETURN_CENTER:
        needle_val = pos % 72;
        if (needle_val < 0) needle_val += 72;
        break;
    default:
        needle_val = pos;
        break;
    }
    lv_scale_set_line_needle_value(d->scale, d->needle, 95, needle_val);

    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", (long)pos);
    lv_label_set_text(d->label_value, buf);

    /* out-of-bounds red arc (X-Knob BoundZeroView) */
    if (off != 0) {
        int32_t start, end;
        if (pos <= 0) {
            start = SCALE_LEFT_BOUND_DEG - ARC_START_ROTATION - (int32_t)off;
            end = SCALE_LEFT_BOUND_DEG - ARC_START_ROTATION;
        } else {
            start = SCALE_LEFT_BOUND_DEG + SCALE_ANGLE_RANGE - ARC_START_ROTATION;
            end = SCALE_LEFT_BOUND_DEG + SCALE_ANGLE_RANGE - ARC_START_ROTATION - (int32_t)off;
        }
        lv_arc_set_angles(d->arc, start, end);
    } else {
        lv_arc_set_angles(d->arc, 0, 0);
    }
}

/* 触摸点击整页 → 切换到下一种手感模式 */
static void pg_tap_cb(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);
    pg_data_t *d = p->data;
    int count = motor_get_mode_count();
    d->mode = (d->mode + 1) % count;
    pg_apply_mode(d);
    pm_shake();
}

static void pg_playground_create(page_t *p)
{
    pg_data_t *d = calloc(1, sizeof(pg_data_t));
    p->data = d;
    d->mode = MOTOR_MODE_COARSE_STRONG_DETENTS;
    p->title = "\xE6\x89\x8B\xE6\x84\x9F";

    lv_obj_t *hint = lv_label_create(p->root);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE7\x82\xB9\xE5\x87\xBB\xE5\x88\x87\xE6\x8D\xA2\xE6\xA8\xA1\xE5\xBC\x8F");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    d->label_mode = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_mode, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_mode, &lv_font_msyh_16, 0);
    lv_obj_align(d->label_mode, LV_ALIGN_TOP_MID, 0, 28);

    /* X-Knob style round scale */
    d->scale = lv_scale_create(p->root);
    lv_obj_set_pos(d->scale, 0, 50);
    lv_obj_set_size(d->scale, 240, 240);
    lv_obj_set_style_bg_color(d->scale, lv_color_hex(XK_COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(d->scale, lv_color_make(64, 0, 64), 0);
    lv_obj_set_style_bg_grad_dir(d->scale, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(d->scale, LV_RADIUS_CIRCLE, 0);
    lv_scale_set_mode(d->scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_label_show(d->scale, false);

    /* minor ticks: red */
    lv_obj_set_style_length(d->scale, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_width(d->scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(d->scale, lv_color_hex(XK_COLOR_RED), LV_PART_ITEMS);
    /* major ticks: red, longer */
    lv_obj_set_style_length(d->scale, 14, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(d->scale, 3, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(d->scale, lv_color_hex(XK_COLOR_RED), LV_PART_INDICATOR);
    /* main arc */
    lv_obj_set_style_arc_color(d->scale, lv_color_hex(XK_COLOR_RED), LV_PART_MAIN);
    lv_obj_set_style_arc_width(d->scale, 2, LV_PART_MAIN);

    lv_scale_set_total_tick_count(d->scale, 41);
    lv_scale_set_major_tick_every(d->scale, 1);
    lv_scale_set_range(d->scale, 0, 360);
    lv_scale_set_angle_range(d->scale, 360);
    lv_scale_set_rotation(d->scale, 270);

    /* needle (blue line) */
    static lv_point_precise_t needle_points[2] = { {0, 0}, {0, 0} };
    d->needle = lv_line_create(d->scale);
    lv_line_set_points_mutable(d->needle, needle_points, 2);
    lv_obj_set_style_line_width(d->needle, 8, 0);
    lv_obj_set_style_line_rounded(d->needle, true, 0);
    lv_obj_set_style_line_color(d->needle, lv_color_hex(XK_COLOR_BLUE), 0);
    lv_scale_set_post_draw(d->scale, true);
    lv_scale_set_line_needle_value(d->scale, d->needle, 95, 0);
    /* 关键: scale 默认可点击且点击不冒泡, 会吞掉整页"点击切换模式" */
    lv_obj_remove_flag(d->scale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);

    /* out-of-bounds red arc overlay */
    d->arc = lv_arc_create(p->root);
    lv_obj_set_pos(d->arc, 0, 50);
    lv_obj_set_size(d->arc, 240, 240);
    lv_obj_remove_style(d->arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_color(d->arc, lv_color_hex(XK_COLOR_RED), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(d->arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(d->arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(d->arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_arc_set_bg_angles(d->arc, 0, 0);
    lv_obj_clear_flag(d->arc, LV_OBJ_FLAG_CLICKABLE);

    d->label_value = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->label_value, "0");
    lv_obj_align(d->label_value, LV_ALIGN_CENTER, 0, 60);

    pg_apply_mode(d);
    d->timer = lv_timer_create(pg_playground_timer, 50, d);

    /* 整页点击切换模式 (页面根容器) */
    lv_obj_add_flag(p->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->root, pg_tap_cb, LV_EVENT_CLICKED, p);
}

static void pg_playground_destroy(page_t *p)
{
    pg_data_t *d = p->data;
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        free(d);
    }
    p->data = NULL;
}

static void pg_playground_on_rotate(page_t *p, int32_t steps)
{
    /* 位置由电机力反馈控制, 这里只负责显示 */
}

static void pg_playground_on_back(page_t *p)
{
    pm_pop();
}

static void pg_playground_on_tick(page_t *p)
{
}

static void pg_playground_on_resume(page_t *p)
{
    /* 重新应用当前手感模式(子页/返回链路可能改过电机模式) */
    pg_apply_mode((pg_data_t *)p->data);
}

const page_ops_t pg_playground_ops = {
    .create = pg_playground_create,
    .destroy = pg_playground_destroy,
    .on_rotate = pg_playground_on_rotate,
    .on_back = pg_playground_on_back,
    .on_tick = pg_playground_on_tick,
    .on_resume = pg_playground_on_resume,
};