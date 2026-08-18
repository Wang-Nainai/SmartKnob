#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"

extern const page_ops_t pg_env_ops;

void ui_env_get(uint16_t *co2, float *temp, float *rh, uint8_t *has_data);

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

typedef struct {
    uint16_t co2;
    float temp;
    float rh;
    uint8_t has_data;
    lv_obj_t *scale;
    lv_obj_t *needle;
    lv_obj_t *label_value;
    lv_obj_t *label_level;
    lv_obj_t *label_sub;
    lv_timer_t *timer;
} env_data_t;

static uint32_t env_level_color(uint16_t co2)
{
    if (co2 < 450)  return 0x00C864;
    if (co2 < 800)  return 0x2ECC40;
    if (co2 < 1000) return 0x9ACD32;
    if (co2 < 1500) return 0xFFC000;
    if (co2 < 2500) return 0xFF7A00;
    if (co2 < 5000) return 0xFF3B30;
    return 0xE7001F;
}

static const char *env_level_name(uint16_t co2)
{
    if (co2 < 450)  return "\xE6\xB8\x85\xE6\x96\xB0";
    if (co2 < 800)  return "\xE4\xBC\x98\xE7\xA7\x80";
    if (co2 < 1000) return "\xE8\x89\xAF\xE5\xA5\xBD";
    if (co2 < 1500) return "\xE8\xBE\x83\xE5\xB7\xAE";
    if (co2 < 2500) return "\xE4\xB8\xA5\xE9\x87\x8D";
    if (co2 < 5000) return "\xE5\x8D\xB1\xE9\x99\xA9";
    return "\xE6\x9E\x81\xE6\xAF\x92";
}

static void env_timer_cb(lv_timer_t *t)
{
    env_data_t *d = lv_timer_get_user_data(t);
    char buf[48];

    if (d->has_data) {
        uint16_t pct = d->co2 > 5000 ? 100 : (uint16_t)((uint32_t)d->co2 * 100 / 5000);
        int32_t val = (int32_t)pct * 72 / 100;
        lv_scale_set_line_needle_value(d->scale, d->needle, 95, val);

        snprintf(buf, sizeof(buf), "%u", d->co2);
        lv_label_set_text(d->label_value, buf);
        lv_obj_set_style_text_color(d->label_value, lv_color_hex(env_level_color(d->co2)), 0);

        lv_label_set_text(d->label_level, env_level_name(d->co2));
        lv_obj_set_style_text_color(d->label_level, lv_color_hex(env_level_color(d->co2)), 0);

        snprintf(buf, sizeof(buf), "\xE6\xB8\xA9\xE5\xBA\xA6 %.1f\xB0" "C    \xE6\xB9\xBF\xE5\xBA\xA6 %.0f%%",
                 d->temp, d->rh);
        lv_label_set_text(d->label_sub, buf);
    } else {
        lv_scale_set_line_needle_value(d->scale, d->needle, 95, 0);
        lv_label_set_text(d->label_value, "--");
        lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_label_set_text(d->label_level, "\xE7\xAD\x89\xE5\xBE\x85\xE6\x95\xB0\xE6\x8D\xAE");
        lv_obj_set_style_text_color(d->label_level, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_label_set_text(d->label_sub, "SCD40");
    }
}

static void pg_env_create(page_t *p)
{
    env_data_t *d = calloc(1, sizeof(env_data_t));
    p->data = d;
    p->title = "\xE7\x8E\xAF\xE5\xA2\x83";

    lv_obj_t *hint = lv_label_create(p->root);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE5\x8F\x8D\xE8\xBD\xAC\xE8\xBF\x94\xE5\x9B\x9E");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    d->scale = lv_scale_create(p->root);
    lv_obj_set_pos(d->scale, 0, 50);
    lv_obj_set_size(d->scale, 240, 240);
    lv_obj_set_style_bg_color(d->scale, lv_color_hex(XK_COLOR_BG), 0);
    lv_obj_set_style_bg_grad_color(d->scale, lv_color_make(0, 48, 48), 0);
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
    lv_scale_set_range(d->scale, 0, 72);
    lv_scale_set_angle_range(d->scale, 360);
    lv_scale_set_rotation(d->scale, 270);

    static lv_point_precise_t needle_points[2] = { {0, 0}, {0, 0} };
    d->needle = lv_line_create(d->scale);
    lv_line_set_points_mutable(d->needle, needle_points, 2);
    lv_obj_set_style_line_width(d->needle, 8, 0);
    lv_obj_set_style_line_rounded(d->needle, true, 0);
    lv_obj_set_style_line_color(d->needle, lv_color_hex(XK_COLOR_GREEN), 0);
    lv_scale_set_post_draw(d->scale, true);
    lv_scale_set_line_needle_value(d->scale, d->needle, 95, 0);

    d->label_value = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->label_value, "--");
    lv_obj_align(d->label_value, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *unit = lv_label_create(p->root);
    lv_obj_set_style_text_color(unit, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_label_set_text(unit, "CO2 (ppm)");
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 110);

    d->label_level = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_level, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_level, &lv_font_msyh_16, 0);
    lv_label_set_text(d->label_level, "\xE7\xAD\x89\xE5\xBE\x85\xE6\x95\xB0\xE6\x8D\xAE");
    lv_obj_align(d->label_level, LV_ALIGN_CENTER, 0, 150);

    d->label_sub = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_sub, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_sub, &lv_font_msyh_16, 0);
    lv_label_set_text(d->label_sub, "SCD40");
    lv_obj_align(d->label_sub, LV_ALIGN_CENTER, 0, 190);

    d->timer = lv_timer_create(env_timer_cb, 1000, d);
    motor_set_mode(MOTOR_MODE_UNBOUND_NO_DETENTS, 0, 0);
}

static void pg_env_destroy(page_t *p)
{
    env_data_t *d = p->data;
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        free(d);
    }
    p->data = NULL;
}

static void pg_env_on_rotate(page_t *p, int32_t steps)
{
}

static void pg_env_on_back(page_t *p)
{
    pm_pop();
}

static void pg_env_on_tick(page_t *p)
{
    env_data_t *d = p->data;
    uint16_t co2;
    float temp, rh;
    uint8_t has;
    ui_env_get(&co2, &temp, &rh, &has);
    if (has) {
        d->co2 = co2;
        d->temp = temp;
        d->rh = rh;
        d->has_data = 1;
    }
}

const page_ops_t pg_env_ops = {
    .create = pg_env_create,
    .destroy = pg_env_destroy,
    .on_rotate = pg_env_on_rotate,
    .on_back = pg_env_on_back,
    .on_tick = pg_env_on_tick,
};