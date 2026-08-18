#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_msyh_16);

typedef struct {
    int focus;
    lv_obj_t *rows[5];
} menu_data_t;

typedef struct {
    const char *icon;
    const char *title;
    const char *sub;
    page_id_t page;
} menu_item_t;

static const menu_item_t items[] = {
    { LV_SYMBOL_KEYBOARD, "\xE6\x89\x8B\xE6\x84\x9F", "11 \xE7\xA7\x8D\xE6\xA8\xA1\xE5\xBC\x8F", PAGE_PLAYGROUND },
    { LV_SYMBOL_HOME,     "\xE6\x99\xBA\xE8\x83\xBD\xE5\xAE\xB6\xE5\xB1\x85", "MQTT \xE6\x8E\xA7\xE5\x88\xB6", PAGE_HASS },
    { LV_SYMBOL_TINT,     "\xE7\x8E\xAF\xE5\xA2\x83", "CO2 \xE7\x9B\x91\xE6\xB5\x8B", PAGE_ENV },
    { LV_SYMBOL_SETTINGS, "\xE8\xAE\xBE\xE7\xBD\xAE", "\xE4\xBA\xAE\xE5\xBA\xA6 \xC2\xB7 \xE7\x86\x84\xE5\xB1\x8F", PAGE_SETTING },
    { LV_SYMBOL_LIST,     "\xE7\xB3\xBB\xE7\xBB\x9F", "\xE7\x89\x88\xE6\x9C\xAC\xE4\xBF\xA1\xE6\x81\xAF", PAGE_SYSINFO },
};
#define MENU_COUNT (sizeof(items) / sizeof(items[0]))

static void menu_refresh(menu_data_t *d)
{
    for (int i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *row = d->rows[i];
        if (i == d->focus) {
            lv_obj_set_style_border_color(row, lv_color_hex(XK_COLOR_RED), 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(XK_COLOR_PANEL), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        }
    }
}

static void menu_move_focus(menu_data_t *d, int steps)
{
    int n = (int)MENU_COUNT;
    d->focus = (d->focus + steps) % n;
    if (d->focus < 0) {
        d->focus += n;
    }
    menu_refresh(d);
}

/* 触摸点击菜单项 → 进入对应页面 */
static void menu_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)MENU_COUNT) {
        return;
    }
    pm_push(items[idx].page);
    pm_shake();
}

static void pg_menu_create(page_t *p)
{
    menu_data_t *d = calloc(1, sizeof(menu_data_t));
    p->data = d;
    d->focus = 0;
    p->title = "SmartKnob";  /* 根页标题 */

    lv_obj_t *hint = lv_label_create(p->root);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE6\x97\x8B\xE8\xBD\xAC\xE9\x80\x89\xE6\x8B\xA9 \xC2\xB7 \xE7\x82\xB9\xE5\x87\xBB\xE8\xBF\x9B\xE5\x85\xA5");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

    for (int i = 0; i < MENU_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(p->root);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 240, 54);
        lv_obj_set_pos(row, 0, 22 + i * 54);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(XK_COLOR_RED), 0);
        lv_obj_set_style_border_post(row, true, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(XK_COLOR_PANEL), 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, menu_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon = lv_label_create(row);
        lv_obj_set_style_text_color(icon, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
        lv_label_set_text(icon, items[i].icon);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t *title = lv_label_create(row);
        lv_obj_set_style_text_color(title, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(title, &lv_font_msyh_16, 0);
        lv_label_set_text(title, items[i].title);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 46, 0);

        lv_obj_t *sub = lv_label_create(row);
        lv_obj_set_style_text_color(sub, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(sub, &lv_font_msyh_16, 0);
        lv_label_set_text(sub, items[i].sub);
        lv_obj_align(sub, LV_ALIGN_RIGHT_MID, -12, 0);

        if (i < MENU_COUNT - 1) {
            lv_obj_t *line = lv_obj_create(row);
            lv_obj_remove_style_all(line);
            lv_obj_set_size(line, 220, 1);
            lv_obj_set_style_bg_color(line, lv_color_hex(XK_COLOR_BORDER), 0);
            lv_obj_align(line, LV_ALIGN_BOTTOM_MID, 0, 0);
        }

        d->rows[i] = row;
    }

    motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);
    menu_refresh(d);
}

static void pg_menu_destroy(page_t *p)
{
    free(p->data);
    p->data = NULL;
}

static void pg_menu_on_rotate(page_t *p, int32_t steps)
{
    menu_move_focus((menu_data_t *)p->data, steps);
}

static void pg_menu_on_back(page_t *p)
{
    /* root page, nothing to pop */
}

static void pg_menu_on_tick(page_t *p)
{
}

const page_ops_t pg_menu_ops = {
    .create = pg_menu_create,
    .destroy = pg_menu_destroy,
    .on_rotate = pg_menu_on_rotate,
    .on_back = pg_menu_on_back,
    .on_tick = pg_menu_on_tick,
};