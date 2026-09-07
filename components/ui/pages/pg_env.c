#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"

void ui_env_get(uint16_t *co2, float *temp, float *rh, uint8_t *has_data);

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

typedef struct {
    uint16_t co2;
    float temp;
    float rh;
    uint8_t has_data;
    lv_obj_t *badge;        /* 等级徽章(圆角胶囊) */
    lv_obj_t *label_badge;
    lv_obj_t *label_value;  /* CO2 大数字 */
    lv_obj_t *bar;          /* CO2 彩色条 */
    lv_obj_t *label_t;      /* 温度值 */
    lv_obj_t *label_h;      /* 湿度值 */
    lv_timer_t *timer;
} env_data_t;

/* 等级 -> 颜色(绿色系到红色系) */
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
    if (co2 < 450)  return "\xE6\xB8\x85\xE6\x96\xB0";   /* 清新 */
    if (co2 < 800)  return "\xE4\xBC\x98\xE7\xA7\x80";   /* 优秀 */
    if (co2 < 1000) return "\xE8\x89\xAF\xE5\xA5\xBD";   /* 良好 */
    if (co2 < 1500) return "\xE8\xBE\x83\xE5\xB7\xAE";   /* 较差 */
    if (co2 < 2500) return "\xE4\xB8\xA5\xE9\x87\x8D";   /* 严重 */
    if (co2 < 5000) return "\xE5\x8D\xB1\xE9\x99\xA9";   /* 危险 */
    return "\xE6\x9E\x81\xE6\xAF\x92";                   /* 极毒 */
}

/* 圆角卡片: 名称 + 大数值 + 单位 */
static void env_card(lv_obj_t *parent, int x, const char *name,
                     lv_obj_t **value_out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 92, 104);
    lv_obj_set_pos(card, x, 176);
    lv_obj_set_style_bg_color(card, lv_color_hex(XK_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(XK_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *nm = lv_label_create(card);
    lv_obj_set_style_text_color(nm, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(nm, &lv_font_msyh_16, 0);
    lv_label_set_text(nm, name);
    lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *val = lv_label_create(card);
    lv_obj_set_style_text_color(val, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_26, 0);
    lv_label_set_text(val, "--");
    lv_obj_align(val, LV_ALIGN_BOTTOM_MID, 0, -34);

    *value_out = val;
}

static void env_timer_cb(lv_timer_t *t)
{
    env_data_t *d = lv_timer_get_user_data(t);
    char buf[48];

    if (d->has_data) {
        uint32_t col = env_level_color(d->co2);

        lv_obj_set_style_bg_color(d->badge, lv_color_hex(col), 0);
        lv_label_set_text(d->label_badge, env_level_name(d->co2));

        snprintf(buf, sizeof(buf), "%u", d->co2);
        lv_label_set_text(d->label_value, buf);
        lv_obj_set_style_text_color(d->label_value, lv_color_hex(col), 0);

        lv_bar_set_value(d->bar, d->co2 > 5000 ? 5000 : d->co2, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(d->bar, lv_color_hex(col), LV_PART_INDICATOR);

        snprintf(buf, sizeof(buf), "%.1f", d->temp);
        lv_label_set_text(d->label_t, buf);
        snprintf(buf, sizeof(buf), "%.0f", d->rh);
        lv_label_set_text(d->label_h, buf);
    } else {
        lv_obj_set_style_bg_color(d->badge, lv_color_hex(XK_COLOR_FAINT), 0);
        lv_label_set_text(d->label_badge, "\xE7\xAD\x89\xE5\xBE\x85\xE6\x95\xB0\xE6\x8D\xAE");
        lv_label_set_text(d->label_value, "--");
        lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_bar_set_value(d->bar, 0, LV_ANIM_OFF);
        lv_label_set_text(d->label_t, "--");
        lv_label_set_text(d->label_h, "--");
    }
}

static void pg_env_create(page_t *p)
{
    env_data_t *d = calloc(1, sizeof(env_data_t));
    p->data = d;
    p->title = "\xE7\x8E\xAF\xE5\xA2\x83";

    /* ---- CO2 等级徽章(圆角胶囊) ---- */
    d->badge = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->badge);
    lv_obj_set_size(d->badge, 72, 26);
    lv_obj_set_pos(d->badge, 84, 30);
    lv_obj_set_style_bg_color(d->badge, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_bg_opa(d->badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d->badge, 13, 0);
    lv_obj_clear_flag(d->badge, LV_OBJ_FLAG_SCROLLABLE);

    d->label_badge = lv_label_create(d->badge);
    lv_obj_set_style_text_color(d->label_badge, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(d->label_badge, &lv_font_msyh_16, 0);
    lv_label_set_text(d->label_badge, "\xE7\xAD\x89\xE5\xBE\x85\xE6\x95\xB0\xE6\x8D\xAE");
    lv_obj_center(d->label_badge);

    /* ---- CO2 大数字 ---- */
    d->label_value = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->label_value, "--");
    lv_obj_align(d->label_value, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t *unit = lv_label_create(p->root);
    lv_obj_set_style_text_color(unit, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_label_set_text(unit, "CO2 / ppm");
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 126);

    /* ---- CO2 彩色条(0-5000ppm) ---- */
    d->bar = lv_bar_create(p->root);
    lv_obj_set_size(d->bar, 200, 8);
    lv_obj_set_pos(d->bar, 20, 146);
    lv_bar_set_range(d->bar, 0, 5000);
    lv_bar_set_value(d->bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(d->bar, 4, 0);
    lv_obj_set_style_bg_color(d->bar, lv_color_hex(XK_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d->bar, lv_color_hex(XK_COLOR_FAINT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(d->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(d->bar, 4, LV_PART_INDICATOR);

    /* ---- 温度 / 湿度 卡片 ---- */
    env_card(p->root, 20, "\xE6\xB8\xA9\xE5\xBA\xA6", &d->label_t);
    env_card(p->root, 128, "\xE6\xB9\xBF\xE5\xBA\xA6", &d->label_h);

    lv_obj_t *hint = lv_label_create(p->root);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE5\x8F\x8D\xE8\xBD\xAC\xE8\xBF\x94\xE5\x9B\x9E");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
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
    (void)p;
    (void)steps;
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
    .on_resume = NULL,
    .on_tick = pg_env_on_tick,
};