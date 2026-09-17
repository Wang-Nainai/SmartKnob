#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "page_mgr.h"
#include "motor.h"

LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

typedef struct {
    int mode;
    lv_obj_t *track;      /* 全周暗色圆环 */
    lv_obj_t *range_arc;  /* 量程窗口弧(有界模式显示, 无界隐藏) */
    lv_obj_t *red_arc;    /* 越界红弧 */
    lv_obj_t *bezel;      /* 外围细刻度圈 */
    lv_obj_t *dot;        /* 主题蓝指示圆点 */
    lv_obj_t *label_value;
    lv_obj_t *label_mode;
    lv_timer_t *timer;
    int win_min;          /* 当前模式量程(档位) */
    int win_max;
    int win_deg0;         /* 量程窗口起始角(度, 0=12点顺时针) */
    int win_span;         /* 量程窗口角跨度 */
} pg_data_t;

/* 有界模式量程窗口: 从 200° 起顺时针 140° (X-Knob geometry) */
#define WIN_DEG0_BOUND 200
#define WIN_SPAN_BOUND 140

static void pg_refresh_fill(pg_data_t *d, int32_t pos);

static void pg_apply_mode(pg_data_t *d)
{
    int m = d->mode;
    int count = motor_get_mode_count();

    char buf[48];
    snprintf(buf, sizeof(buf), "%s   %d / %d", motor_mode_name(m), m + 1, count);
    lv_label_set_text(d->label_mode, buf);

    switch (m) {
    case MOTOR_MODE_ON_OFF: {
        d->win_min = 0;
        d->win_max = 1;
        d->win_deg0 = 240;
        d->win_span = 60;
        lv_arc_set_bg_angles(d->range_arc, 240, 300);
        break;
    }
    case MOTOR_MODE_ADJUSTER: {
        d->win_min = 0;
        d->win_max = 14;
        d->win_deg0 = 12;   /* 0 档在 12°, 每档 24°(物理角), 14 档到 348° */
        d->win_span = 336;
        lv_arc_set_bg_angles(d->range_arc, 12, 348);
        break;
    }
    case MOTOR_MODE_UNBOUND_NO_DETENTS:
    case MOTOR_MODE_MULTI_TURN_NO_DETENTS:
    case MOTOR_MODE_UNBOUNDED_DETENTS:
    case MOTOR_MODE_AUTO_RETURN_CENTER: {
        d->win_min = 0;
        d->win_max = 0;
        d->win_deg0 = 0;
        d->win_span = 360;
        lv_arc_set_bg_angles(d->range_arc, 0, 0);   /* 无界: 隐藏窗口弧 */
        break;
    }
    default: {
        int32_t max = 31;
        int32_t min = 0;
        if (m == MOTOR_MODE_BOUND_NO_DETENTS) max = 10;
        if (m == MOTOR_MODE_FINE_NO_DETENTS || m == MOTOR_MODE_FINE_DETENTS) max = 255;
        if (m == MOTOR_MODE_RETURN_CENTER_WITH_DETENTS) { min = -6; max = 6; }
        d->win_min = min;
        d->win_max = max;
        d->win_deg0 = WIN_DEG0_BOUND;
        d->win_span = WIN_SPAN_BOUND;
        lv_arc_set_bg_angles(d->range_arc, WIN_DEG0_BOUND,
                             WIN_DEG0_BOUND + WIN_SPAN_BOUND);
        break;
    }
    }

    /* 切模式瞬间按新窗口清场刷新, 不等 timer, 不留旧模式残影.
     * 电机命令是异步的, 瞬间的 offset 可能还是旧模式残值, 红弧强制清零 */
    pg_refresh_fill(d, 0);
    lv_arc_set_angles(d->red_arc, 0, 0);

    motor_set_mode(m, 0, 0);
}

/* 指示圆点绕环: 骑在轨道中线 (r=96), 0 = 12点方向 */
static void pg_dot_set(pg_data_t *d, int deg)
{
    deg = ((deg % 360) + 360) % 360;
    float rad = (float)deg * 0.01745329f;
    lv_obj_set_pos(d->dot, 120 + (int32_t)(96.0f * sinf(rad)) - 6,
                          168 - (int32_t)(96.0f * cosf(rad)) - 6);
}

/* 表盘指示统一入口 (定时器与切模式都只调这一个):
 * - 无界/回中: 圆点绕全周, 无窗口无填充无红弧
 * - 有界: 填充弧从窗口起点长到当前值; 红弧只在"真的越过边界"时出现
 *   (电机侧 offset = 当前档内偏角, 恒不为 0 —— 越界判定必须同时看
 *    位置是否停在边界档 AND 偏移方向朝界外, 且幅值封顶防绕圈)
 * 全部用 lv_arc_set_angles 直接设角, 无内建动画, 50ms 连发不冲突 */
static void pg_refresh_fill(pg_data_t *d, int32_t pos)
{
    float off = motor_get_angle_offset_deg();

    if (d->win_span == 360) {
        lv_arc_set_angles(d->range_arc, 0, 0);
        lv_arc_set_angles(d->red_arc, 0, 0);
        pg_dot_set(d, (int32_t)(pos % 72) * 5);
        lv_obj_clear_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* 有界: 填充弧 (窗口弧 main=背景, indicator 从窗口起点长出) */
    lv_obj_add_flag(d->dot, LV_OBJ_FLAG_HIDDEN);
    int32_t v = pos - d->win_min;
    if (v < 0) v = 0;
    int32_t vmax = d->win_max - d->win_min;
    if (v > vmax) v = vmax;
    int32_t end = d->win_deg0 + (int32_t)(((int64_t)v * d->win_span) / vmax);
    lv_arc_set_angles(d->range_arc, d->win_deg0, end);

    /* 红弧: 位置停在边界档 且 档内偏移朝界外 才算越界, 幅值封顶 30° */
    int32_t o = (int32_t)off;
    if (o > 30) o = 30;
    if (o < -30) o = -30;
    bool oob_min = (pos <= d->win_min && o > 0);
    bool oob_max = (pos >= d->win_max && o < 0);
    if (oob_min) {
        lv_arc_set_angles(d->red_arc, d->win_deg0 - o, d->win_deg0);
    } else if (oob_max) {
        lv_arc_set_angles(d->red_arc, d->win_deg0 + d->win_span,
                          d->win_deg0 + d->win_span - o);
    } else {
        lv_arc_set_angles(d->red_arc, 0, 0);
    }
}

static void pg_playground_timer(lv_timer_t *t)
{
    pg_data_t *d = lv_timer_get_user_data(t);
    int32_t pos = motor_get_position();

    pg_refresh_fill(d, pos);

    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", (long)pos);
    lv_label_set_text(d->label_value, buf);
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
    lv_obj_align(d->label_mode, LV_ALIGN_TOP_MID, 0, 34);

    /* ---- 表盘 (与智能家居页同款 Apple 风格) ----
     * 外圈刻度圈 -> 全周暗环 -> 内层渐变圆 -> 主题蓝指示点 -> 中央大数值 */

    /* 外围刻度圈 (装饰, 固定 73 根) */
    d->bezel = lv_scale_create(p->root);
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
    lv_obj_remove_flag(d->bezel, LV_OBJ_FLAG_CLICKABLE);

    /* 全周暗色圆环 */
    d->track = lv_arc_create(p->root);
    lv_obj_set_size(d->track, 204, 204);
    lv_obj_set_pos(d->track, 18, 66);
    lv_arc_set_rotation(d->track, 0);
    lv_arc_set_bg_angles(d->track, 0, 360);
    lv_arc_set_range(d->track, 0, 100);
    lv_arc_set_value(d->track, 0);
    lv_obj_remove_style(d->track, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(d->track, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(d->track, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(d->track, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(d->track, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
    lv_obj_remove_flag(d->track, LV_OBJ_FLAG_CLICKABLE);

    /* 量程窗口弧 (有界模式: main=量程背景弧, indicator=值填充弧, 同一根) */
    d->range_arc = lv_arc_create(p->root);
    lv_obj_set_size(d->range_arc, 204, 204);
    lv_obj_set_pos(d->range_arc, 18, 66);
    lv_arc_set_rotation(d->range_arc, 0);
    lv_arc_set_bg_angles(d->range_arc, 0, 0);
    lv_arc_set_range(d->range_arc, 0, 100);
    lv_arc_set_value(d->range_arc, 0);
    lv_obj_remove_style(d->range_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(d->range_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(d->range_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(d->range_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(d->range_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(d->range_arc, lv_color_hex(0x2A2A2C), LV_PART_MAIN);
    lv_obj_set_style_arc_color(d->range_arc, lv_color_hex(XK_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_remove_flag(d->range_arc, LV_OBJ_FLAG_CLICKABLE);

    /* 越界红弧: 出界时从量程窗口边缘溢出 (X-Knob 语言) */
    d->red_arc = lv_arc_create(p->root);
    lv_obj_set_size(d->red_arc, 204, 204);
    lv_obj_set_pos(d->red_arc, 18, 66);
    lv_arc_set_rotation(d->red_arc, 0);
    lv_arc_set_bg_angles(d->red_arc, 0, 0);
    lv_arc_set_range(d->red_arc, 0, 100);
    lv_arc_set_value(d->red_arc, 0);
    lv_obj_remove_style(d->red_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(d->red_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(d->red_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(d->red_arc, lv_color_hex(XK_COLOR_RED), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(d->red_arc, 0, LV_PART_MAIN);
    lv_obj_remove_flag(d->red_arc, LV_OBJ_FLAG_CLICKABLE);

    /* 内层渐变圆 (下沉感) */
    lv_obj_t *inner = lv_obj_create(p->root);
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

    /* 主题蓝指示圆点 */
    d->dot = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->dot);
    lv_obj_set_size(d->dot, 12, 12);
    lv_obj_set_style_bg_color(d->dot, lv_color_hex(XK_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(d->dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(d->dot, LV_OBJ_FLAG_CLICKABLE);

    /* 中央大数值 (表盘正中央) */
    d->label_value = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->label_value, "0");
    lv_obj_align(d->label_value, LV_ALIGN_CENTER, 0, 0);

    pg_apply_mode(d);
    d->timer = lv_timer_create(pg_playground_timer, 50, d);

    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
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