#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "display.h"

LV_FONT_DECLARE(lv_font_montserrat_32);

typedef struct {
    lv_timer_t *timer;
} startup_data_t;

static void anim_width_cb(void *obj, int32_t v)
{
    lv_obj_set_width((lv_obj_t *)obj, v);
}

static void startup_on_timer(lv_timer_t *t)
{
    LV_UNUSED(t);
    /* 未触摸校准过: 先进校准向导, 完成后由向导进入主页 */
    pm_replace(display_touch_cal_active() ? PAGE_MENU : PAGE_TCAL);
    pm_shake();
}

static void pg_startup_create(page_t *p)
{
    startup_data_t *d = calloc(1, sizeof(startup_data_t));
    p->data = d;
    p->title = "SmartKnob";

    /* X-Knob style: expanding underline + sliding logo text */
    lv_obj_t *cont = lv_obj_create(p->root);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, 110, 40);
    lv_obj_set_style_border_color(cont, lv_color_hex(XK_COLOR_ACCENT), 0);
    lv_obj_set_style_border_side(cont, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(cont, 3, 0);
    lv_obj_set_style_border_post(cont, true, 0);
    lv_obj_center(cont);

    lv_obj_t *label = lv_label_create(cont);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_label_set_text(label, "SmartKnob");

    /* 按文字实际宽度定容器, 避免左右裁切; 下划线从 0 展开到该宽度 */
    lv_obj_update_layout(label);
    int32_t w = lv_obj_get_width(label) + 8;
    lv_obj_set_width(cont, w);
    lv_obj_center(cont);

    /* underline expands */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cont);
    lv_anim_set_exec_cb(&a, anim_width_cb);
    lv_anim_set_values(&a, 0, w);
    lv_anim_set_time(&a, 500);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    /* logo slides in from the right */
    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, label);
    lv_anim_set_exec_cb(&a2, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a2, lv_obj_get_x(label) + 90, lv_obj_get_x(label));
    lv_anim_set_time(&a2, 500);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_out);
    lv_anim_start(&a2);

    d->timer = lv_timer_create(startup_on_timer, 2000, NULL);
    lv_timer_set_repeat_count(d->timer, 1);
}

static void pg_startup_destroy(page_t *p)
{
    startup_data_t *d = p->data;
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        free(d);
    }
    p->data = NULL;
}

const page_ops_t pg_startup_ops = {
    .create = pg_startup_create,
    .destroy = pg_startup_destroy,
    .on_rotate = NULL,
    .on_back = NULL,
    .on_resume = NULL,
    .on_tick = NULL,
};