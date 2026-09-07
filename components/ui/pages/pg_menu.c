#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * X-Knob 风格主菜单 (无限循环)
 * - 内容复制 3 份(18 行), 滚过末尾无缝接回首行, 首尾相连
 *   (副本间像素完全相同, 跳变无感)
 * - 触摸可上下滑动浏览; 旋转 = 移动焦点(居中跟随);
 *   点击 = 进入(滑动 >20px 不算点击)
 * - 左侧图标列: 聚焦 220→70 显式宽度动画(180ms 缓出),
 *   旧行展开 + 新行收窄同时进行, 右侧 2px 主题色边
 * - 右侧灰色多行功能描述(仅聚焦行)
 * ============================================================ */

#define ITEM_H        100
#define ICON_W_OPEN   220   /* 未聚焦: 图标列占满 */
#define ICON_W_FOCUS  70    /* 聚焦: 收窄, 露出描述 + 右侧主题边 */
#define ANIM_MS       180
#define SWIPE_PX      20
#define COPY_N        3                          /* 内容复制份数 */
#define ROWS_N        (MENU_COUNT * COPY_N)      /* 18 行 */
#define COPY_H        (MENU_COUNT * ITEM_H)      /* 单份高度 600 */

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

typedef struct {
    int focus;
    int cur_row;   /* 上次聚焦动画所居中的行(沿旋转方向连贯滚动用) */
    lv_obj_t *rows[ROWS_N];
    lv_obj_t *icons[ROWS_N];
    lv_obj_t *infos[ROWS_N];
} menu_data_t;

static void menu_anim_width(lv_obj_t *icon, int32_t target)
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

static void menu_set_focus(menu_data_t *d, int idx, int dir)
{
    int n = (int)MENU_COUNT;
    if (idx < 0 || idx >= n) {
        return;
    }
    d->focus = idx;
    for (int k = 0; k < n; k++) {
        bool f = (k == idx);
        /* 三份副本同步(副本像素一致, 循环跳变才无感) */
        for (int c = 0; c < COPY_N; c++) {
            int r = c * n + k;
            /* 显式宽度动画: 同图标重复动画自动替换, 快速旋转安全 */
            menu_anim_width(d->icons[r], f ? ICON_W_FOCUS : ICON_W_OPEN);
            /* 主题色边: 仅聚焦行, 显式设置(不用状态样式) */
            lv_obj_set_style_border_width(d->icons[r], f ? 2 : 0, 0);
            /* 说明文字仅聚焦行显示 */
            if (f) {
                lv_obj_remove_flag(d->infos[r], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(d->infos[r], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    /* 目标行: 沿旋转方向取相邻副本 —— 系统→S-Dial 也继续向下滚 100px,
     * 全程连贯 (而不是回卷 600px 跳回顶部); 动画结束后由 wrap 归位 */
    int nr = d->cur_row + dir;
    if (dir == 0 || nr < 0 || nr >= ROWS_N || (nr % n) != idx) {
        nr = n + idx;
    }
    d->cur_row = nr;
    lv_obj_t *root = lv_obj_get_parent(d->rows[nr]);
    lv_obj_update_layout(root);
    int32_t vh = lv_obj_get_height(root);
    if (vh < ITEM_H) {
        vh = 3 * ITEM_H;   /* 布局未就绪兜底 */
    }
    /* 无内外边距: 行顶 = nr*ITEM_H, 聚焦行竖直居中 */
    lv_obj_scroll_to_y(root, (int32_t)nr * ITEM_H - (vh - ITEM_H) / 2, LV_ANIM_ON);
}

/* 滚动停止后归位到中间副本区间(下越界一退, 上越界一进):
 * 副本间像素相同, ANIM_OFF 跳变无感, 实现"首尾相连" */
static void menu_scroll_wrap_cb(lv_event_t *e)
{
    menu_data_t *d = lv_event_get_user_data(e);
    lv_obj_t *root = lv_event_get_target(e);
    int32_t y = lv_obj_get_scroll_y(root);
    int32_t ny = y;
    if (ny >= 2 * COPY_H - ITEM_H) {
        ny -= COPY_H;
    } else if (ny < COPY_H) {
        ny += COPY_H;
    }
    d->cur_row = (int)MENU_COUNT + d->focus;   /* 基准回到中间副本 */
    if (ny != y) {
        lv_obj_scroll_to_y(root, ny, LV_ANIM_OFF);
    }
}

/* 点击列表项 → 进入 (按下记录起点, 滑动超过阈值不算点击) */
static lv_point_t press_pt;

static void menu_row_press_cb(lv_event_t *e)
{
    lv_indev_get_point(lv_indev_active(), &press_pt);
}

static void menu_row_cb(lv_event_t *e)
{
    lv_point_t now;
    lv_indev_get_point(lv_indev_active(), &now);
    int dx = now.x - press_pt.x;
    int dy = now.y - press_pt.y;
    if (dx * dx + dy * dy > SWIPE_PX * SWIPE_PX) {
        return;   /* 滑动, 不是点击 */
    }
    int idx = (int)(intptr_t)lv_event_get_user_data(e) % (int)MENU_COUNT;
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
    /* 无上下外边距: 18 行纯周期排列, 循环跳变才能像素级无缝 */
    lv_obj_set_scrollbar_mode(p->root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(p->root, menu_scroll_wrap_cb, LV_EVENT_SCROLL_END, d);

    for (int i = 0; i < ROWS_N; i++) {
        const menu_item_t *it = &items[i % (int)MENU_COUNT];

        /* 行容器 */
        lv_obj_t *row = lv_obj_create(p->root);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 220, ITEM_H);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, menu_row_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(row, menu_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        d->rows[i] = row;

        /* 左侧图标列 (聚焦收窄 + 右侧主题边, X-Knob signature) */
        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, ICON_W_OPEN, ITEM_H);
        /* 关键: 图标默认 CLICKABLE 且铺满整行置顶, 会吃掉所有点按
         * → 行收不到 CLICKED, 点选失效; 必须关闭让点击穿透到行 */
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_align(icon, LV_ALIGN_LEFT_MID, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(icon, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(icon, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_side(icon, LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_color(icon, lv_color_hex(XK_COLOR_ACCENT), 0);
        lv_obj_set_style_border_post(icon, true, 0);

        /* 图标 + 名称 (纵向堆叠) */
        lv_obj_t *img = lv_label_create(icon);
        lv_obj_set_style_text_color(img, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(img, &lv_font_montserrat_26, 0);
        lv_label_set_text(img, it->icon);

        lv_obj_t *name = lv_label_create(icon);
        lv_obj_set_style_text_color(name, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, &lv_font_msyh_16, 0);
        lv_label_set_text(name, it->name);
        d->icons[i] = icon;

        /* 右侧灰色描述 (仅聚焦行显示) */
        lv_obj_t *info = lv_label_create(row);
        lv_obj_set_style_text_color(info, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(info, &lv_font_msyh_16, 0);
        lv_label_set_text(info, it->desc);
        lv_obj_align(info, LV_ALIGN_LEFT_MID, ICON_W_FOCUS + 10, 0);
        lv_obj_add_flag(info, LV_OBJ_FLAG_HIDDEN);
        d->infos[i] = info;

        lv_obj_move_foreground(icon);
    }

    menu_set_focus(d, 0, 0);
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
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
    /* 无级循环: 1-2-3-4-5-6-1-2-3 连续旋转 */
    d->focus = ((d->focus + steps) % n + n) % n;
    menu_set_focus(d, d->focus, (int)steps);
}

static void pg_menu_on_back(page_t *p)
{
    /* root page, nothing to pop */
}

static void pg_menu_on_resume(page_t *p)
{
    /* 从子页返回: 恢复菜单浏览手感 + 滚动回焦点行 */
    menu_data_t *d = p->data;
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
    menu_set_focus(d, d->focus, 0);
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
    .on_resume = pg_menu_on_resume,
};
