#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * X-Knob 风格主菜单
 * - 全屏纵向大列表(单项 100px), 滚动跟随焦点
 * - 左侧图标列: 聚焦时缩窄(220→70)并显示右侧 2px 红边,
 *   宽度过渡动画(overshoot 200ms) —— X-Knob MenuView 复刻
 * - 右侧灰色多行功能描述
 * - 旋转 = 移动焦点, 点击 = 进入
 * ============================================================ */

#define ITEM_H        100
#define ITEM_PAD      ((320 - ITEM_H) / 2)
#define ICON_W_OPEN   220   /* 未聚焦: 图标列占满, 遮住描述 */
#define ICON_W_FOCUS  70    /* 聚焦: 收窄, 露出描述 + 右侧红边 */

typedef struct {
    int focus;
    lv_obj_t *rows[6];
    lv_obj_t *icons[6];
} menu_data_t;

typedef struct {
    const char *icon;
    const char *name;
    const char *desc;
    page_id_t page;
} menu_item_t;

static const menu_item_t items[] = {
    { LV_SYMBOL_VOLUME_MAX, "S-Dial",
      "\xE7\x94\xB5\xE8\x84\x91\xE6\x8E\xA7\xE5\x88\xB6\n"
      "\xE9\x9F\xB3\xE9\x87\x8F \xC2\xB7 \xE6\xBB\x9A\xE8\xBD\xAE \xC2\xB7 \xE6\x92\xAD\xE6\x94\xBE", PAGE_PCDIAL },
    { LV_SYMBOL_KEYBOARD, "\xE6\x89\x8B\xE6\x84\x9F",
      "11 \xE7\xA7\x8D\xE6\x89\x8B\xE6\x84\x9F\xE6\xA8\xA1\xE5\xBC\x8F\n"
      "\xE7\x82\xB9\xE5\x87\xBB\xE5\x88\x87\xE6\x8D\xA2 \xC2\xB7 \xE6\x97\x8B\xE8\xBD\xAC\xE6\xB5\x8B\xE8\xAF\x95", PAGE_PLAYGROUND },
    { LV_SYMBOL_HOME, "\xE6\x99\xBA\xE8\x83\xBD\xE5\xAE\xB6\xE5\xB1\x85",
      "\xE7\x81\xAF\xE5\x85\x89 \xC2\xB7 \xE7\xA9\xBA\xE8\xB0\x83\n"
      "\xE9\xA3\x8E\xE6\x89\x87 \xC2\xB7 \xE6\xB4\x97\xE8\xA1\xA3\xE6\x9C\xBA", PAGE_HASS },
    { LV_SYMBOL_TINT, "\xE7\x8E\xAF\xE5\xA2\x83",
      "CO2 \xE6\xB5\x93\xE5\xBA\xA6\n"
      "\xE6\xB8\xA9\xE5\xBA\xA6 \xC2\xB7 \xE6\xB9\xBF\xE5\xBA\xA6", PAGE_ENV },
    { LV_SYMBOL_SETTINGS, "\xE8\xAE\xBE\xE7\xBD\xAE",
      "\xE4\xBA\xAE\xE5\xBA\xA6\n"
      "\xE7\x86\x84\xE5\xB1\x8F\xE6\x97\xB6\xE9\x95\xBF", PAGE_SETTING },
    { LV_SYMBOL_LIST, "\xE7\xB3\xBB\xE7\xBB\x9F",
      "\xE5\x9B\xBA\xE4\xBB\xB6\xE4\xBF\xA1\xE6\x81\xAF\n"
      "\xE7\xBD\x91\xE7\xBB\x9C\xE7\x8A\xB6\xE6\x80\x81 \xC2\xB7 \xE5\xB7\xA5\xE5\x8E\x82\xE6\xB5\x8B\xE8\xAF\x95", PAGE_SYSINFO },
};
#define MENU_COUNT (sizeof(items) / sizeof(items[0]))

static void menu_set_focus(menu_data_t *d, int idx)
{
    if (idx < 0 || idx >= (int)MENU_COUNT) {
        return;
    }
    for (int i = 0; i < (int)MENU_COUNT; i++) {
        if (i == idx) {
            lv_obj_add_state(d->icons[i], LV_STATE_FOCUSED);
        } else {
            lv_obj_remove_state(d->icons[i], LV_STATE_FOCUSED);
        }
    }
    lv_obj_scroll_to_view(d->rows[idx], LV_ANIM_ON);
}

/* 点击列表项 → 进入 */
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
    p->title = "SmartKnob";

    lv_obj_set_flex_flow(p->root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(p->root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(p->root, ITEM_PAD, 0);

    for (int i = 0; i < (int)MENU_COUNT; i++) {
        /* 行容器 */
        lv_obj_t *row = lv_obj_create(p->root);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 220, ITEM_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, menu_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        d->rows[i] = row;

        /* 左侧图标列 (聚焦收窄 + 红右边界, X-Knob signature) */
        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, ICON_W_OPEN, ITEM_H);
        lv_obj_set_style_bg_color(icon, lv_color_hex(XK_COLOR_BG), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_align(icon, LV_ALIGN_LEFT_MID, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(icon, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(icon, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        /* 聚焦态样式: 收窄 + 右侧红边 */
        lv_obj_set_style_width(icon, ICON_W_FOCUS, LV_STATE_FOCUSED);
        lv_obj_set_style_border_side(icon, LV_BORDER_SIDE_RIGHT, LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(icon, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(icon, lv_color_hex(XK_COLOR_RED), LV_STATE_FOCUSED);

        /* 宽度过渡动画 (X-Knob: overshoot 200ms) */
        static lv_style_transition_dsc_t trans;
        static const lv_style_prop_t props[] = { LV_STYLE_WIDTH, LV_STYLE_PROP_INV };
        lv_style_transition_dsc_init(&trans, props, lv_anim_path_overshoot, 200, 0, NULL);
        lv_obj_set_style_transition(icon, &trans, LV_STATE_FOCUSED);
        lv_obj_set_style_transition(icon, &trans, 0);

        /* 图标 + 名称 (纵向堆叠) */
        lv_obj_t *img = lv_label_create(icon);
        lv_obj_set_style_text_color(img, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(img, &lv_font_montserrat_26, 0);
        lv_label_set_text(img, items[i].icon);

        lv_obj_t *name = lv_label_create(icon);
        lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
        lv_label_set_text(name, items[i].name);
        d->icons[i] = icon;

        /* 右侧灰色描述 (聚焦时从缩窄的图标列后露出) */
        lv_obj_t *info = lv_label_create(row);
        lv_obj_set_style_text_color(info, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(info, &lv_font_msyh_16, 0);
        lv_label_set_text(info, items[i].desc);
        lv_obj_align(info, LV_ALIGN_LEFT_MID, ICON_W_FOCUS + 5, 0);

        lv_obj_move_foreground(icon);
    }

    menu_set_focus(d, 0);
    motor_set_mode(MOTOR_MODE_COARSE_STRONG_DETENTS, 0, 0);
}

static void pg_menu_destroy(page_t *p)
{
    free(p->data);
    p->data = NULL;
}

static void pg_menu_on_rotate(page_t *p, int32_t steps)
{
    menu_data_t *d = p->data;
    int n = (int)MENU_COUNT;
    d->focus = (d->focus + steps) % n;
    if (d->focus < 0) {
        d->focus += n;
    }
    menu_set_focus(d, d->focus);
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