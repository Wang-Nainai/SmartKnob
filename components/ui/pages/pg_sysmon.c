#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "page_mgr.h"
#include "sysmon.h"

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * 系统监控页 (类任务管理器)
 * 点按 = 切换 概览/任务表; 概览页旋转 = 进入任务表; 任务表旋转 = 滚动
 * 数据: sysmon 组件 1Hz 采样(FreeRTOS 运行时统计), 本页 1s 读快照刷新
 * ============================================================ */

#define TASK_ROWS   16   /* visible rows (virtual scroll window); data cap = SYSMON_MAX_TASKS */

typedef struct {
    bool task_list_shown;
    int scroll_off;                /* task list virtual scroll offset (rows) */
    lv_obj_t *ov_view;
    lv_obj_t *task_view;
    lv_obj_t *bar[5];                  /* CPU0 CPU1 ALL INT PSRAM */
    lv_obj_t *bar_val[5];
    lv_obj_t *info;
    lv_obj_t *task_lab[TASK_ROWS][4];  /* 每行: 名称/CPU/栈/核+状态 */
    lv_timer_t *timer;
    sysmon_snapshot_t snap;
} sysmon_data_t;

static const char *state_str(uint8_t s)
{
    static const char *names[] = {"R", "RDY", "BLK", "SUS", "DEL", "?"};
    return names[s < 5 ? s : 5];
}

static void sysmon_timer_cb(lv_timer_t *t);

static lv_obj_t *bar_row(lv_obj_t *parent, int y, const char *name,
                         lv_obj_t **bar_out, lv_obj_t **val_out)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_label_set_text(l, name);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 4, y);

    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 140, 12);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 52, y + 2);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x232a36), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(XK_COLOR_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *v = lv_label_create(parent);
    lv_obj_set_style_text_color(v, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_label_set_text(v, "0%");
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -6, y);

    *bar_out = bar;
    *val_out = v;
    return bar;
}

static void pg_sysmon_set_view(sysmon_data_t *d, bool list_shown)
{
    d->task_list_shown = list_shown;
    if (list_shown) {
        lv_obj_add_flag(d->ov_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(d->task_view, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(d->task_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(d->ov_view, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pg_sysmon_tap_cb(lv_event_t *e)
{
    page_t *p = (page_t *)lv_event_get_user_data(e);
    sysmon_data_t *d = p->data;
    pg_sysmon_set_view(d, !d->task_list_shown);
    pm_shake();
}

static void pg_sysmon_create(page_t *p)
{
    sysmon_data_t *d = calloc(1, sizeof(sysmon_data_t));
    p->data = d;
    p->title = "\xE7\xB3\xBB\xE7\xBB\x9F\xE7\x9B\x91\xE6\x8E\xA7";   /* 系统监控 */

    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(p->root, pg_sysmon_tap_cb, LV_EVENT_CLICKED, p);
    lv_obj_set_user_data(p->root, p);

    /* ---- 概览视图 ---- */
    d->ov_view = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->ov_view);
    lv_obj_set_size(d->ov_view, 240, 320);
    lv_obj_set_pos(d->ov_view, 0, 0);
    lv_obj_remove_flag(d->ov_view, LV_OBJ_FLAG_SCROLLABLE);

    static const char *cpu_names[3] = {"CPU0", "CPU1", "ALL"};
    for (int i = 0; i < 3; i++) {
        bar_row(d->ov_view, 22 + i * 30, cpu_names[i], &d->bar[i], &d->bar_val[i]);
        lv_obj_set_style_bg_color(d->bar[i], lv_color_hex(XK_COLOR_RED), LV_PART_INDICATOR);
    }
    static const char *mem_names[2] = {"INT", "PSRAM"};
    for (int i = 0; i < 2; i++) {
        bar_row(d->ov_view, 122 + i * 30, mem_names[i], &d->bar[3 + i], &d->bar_val[3 + i]);
        lv_obj_set_style_bg_color(d->bar[3 + i], lv_color_hex(XK_COLOR_GREEN), LV_PART_INDICATOR);
    }

    d->info = lv_label_create(d->ov_view);
    lv_obj_set_style_text_color(d->info, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->info, &lv_font_montserrat_14, 0);
    lv_label_set_text(d->info, "-");
    lv_obj_align(d->info, LV_ALIGN_TOP_LEFT, 4, 190);

    lv_obj_t *tip = lv_label_create(d->ov_view);
    lv_obj_set_style_text_color(tip, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(tip, &lv_font_msyh_16, 0);
    lv_label_set_text(tip, "\xE7\x82\xB9\xE5\x87\xBB\xE6\x9F\xA5\xE7\x9C\x8B\xE4\xBB\xBB\xE5\x8A\xA1\xE5\x88\x97\xE8\xA1\xA8");
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* ---- 任务表视图 ---- */
    d->task_view = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->task_view);
    lv_obj_set_size(d->task_view, 240, 320);
    lv_obj_set_pos(d->task_view, 0, 0);
    lv_obj_add_flag(d->task_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_all(d->task_view, 4, 0);

    lv_obj_t *head = lv_label_create(d->task_view);
    lv_obj_set_style_text_color(head, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_14, 0);
    lv_label_set_text(head, "TASK         CPU   STK  C ST");
    lv_obj_set_pos(head, 0, 0);

    /* 只创建可见窗口的行控件(虚拟滚动), 避免一次性创建 40x4 个控件
     * 挤爆内部内存(LVGL 控件堆在内部 SRAM, 实测 160 个 label 直接崩溃) */
    for (int i = 0; i < TASK_ROWS; i++) {
        lv_obj_t *l0 = lv_label_create(d->task_view);
        lv_obj_set_style_text_color(l0, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(l0, &lv_font_montserrat_14, 0);
        lv_label_set_text(l0, "");
        lv_obj_set_pos(l0, 0, 20 + i * 18);
        lv_obj_set_width(l0, 86);
        lv_label_set_long_mode(l0, LV_LABEL_LONG_DOT);

        lv_obj_t *l1 = lv_label_create(d->task_view);
        lv_obj_set_style_text_color(l1, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_14, 0);
        lv_label_set_text(l1, "");
        lv_obj_set_pos(l1, 90, 20 + i * 18);
        lv_obj_set_width(l1, 42);

        lv_obj_t *l2 = lv_label_create(d->task_view);
        lv_obj_set_style_text_color(l2, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_14, 0);
        lv_label_set_text(l2, "");
        lv_obj_set_pos(l2, 134, 20 + i * 18);
        lv_obj_set_width(l2, 52);

        lv_obj_t *l3 = lv_label_create(d->task_view);
        lv_obj_set_style_text_color(l3, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(l3, &lv_font_montserrat_14, 0);
        lv_label_set_text(l3, "");
        lv_obj_set_pos(l3, 188, 20 + i * 18);
        lv_obj_set_width(l3, 46);

        lv_obj_add_flag(l0, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(l1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(l2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(l3, LV_OBJ_FLAG_HIDDEN);

        d->task_lab[i][0] = l0;
        d->task_lab[i][1] = l1;
        d->task_lab[i][2] = l2;
        d->task_lab[i][3] = l3;
    }

    d->timer = lv_timer_create(sysmon_timer_cb, 1000, d);
    sysmon_set_enabled(true);
}

static void pg_sysmon_destroy(page_t *p)
{
    sysmon_data_t *d = p->data;
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        free(d);
    }
    p->data = NULL;
    sysmon_set_enabled(false);   /* 退出即停止采样任务, 零常驻开销 */
}

static void pg_sysmon_on_rotate(page_t *p, int32_t steps)
{
    sysmon_data_t *d = p->data;
    if (!d->task_list_shown) {
        pg_sysmon_set_view(d, true);   /* 概览页旋转进入任务表 */
        return;
    }
    /* 虚拟滚动: 移动数据窗口, 下一拍刷新即重绘 */
    int max_off = (int)d->snap.task_count - TASK_ROWS;
    if (max_off < 0) {
        max_off = 0;
    }
    d->scroll_off += (int)steps;
    if (d->scroll_off > max_off) d->scroll_off = max_off;
    if (d->scroll_off < 0) d->scroll_off = 0;
}

static void pg_sysmon_on_back(page_t *p)
{
    pm_pop();
}

static void sysmon_timer_cb(lv_timer_t *t)
{
    sysmon_data_t *d = (sysmon_data_t *)lv_timer_get_user_data(t);
    char buf[96];

    sysmon_get_snapshot(&d->snap);
    sysmon_snapshot_t *s = &d->snap;

    lv_bar_set_value(d->bar[0], s->cpu0, LV_ANIM_OFF);
    lv_bar_set_value(d->bar[1], s->cpu1, LV_ANIM_OFF);
    lv_bar_set_value(d->bar[2], s->cpu_total, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%u%%", s->cpu0);
    lv_label_set_text(d->bar_val[0], buf);
    snprintf(buf, sizeof(buf), "%u%%", s->cpu1);
    lv_label_set_text(d->bar_val[1], buf);
    snprintf(buf, sizeof(buf), "%u%%", s->cpu_total);
    lv_label_set_text(d->bar_val[2], buf);

    uint32_t int_used = (s->int_total > s->int_free) ? s->int_total - s->int_free : 0;
    uint32_t ps_used = (s->ps_total > s->ps_free) ? s->ps_total - s->ps_free : 0;
    int int_pct = s->int_total ? (int)(int_used * 100 / s->int_total) : 0;
    int ps_pct = s->ps_total ? (int)(ps_used * 100 / s->ps_total) : 0;
    lv_bar_set_value(d->bar[3], int_pct, LV_ANIM_OFF);
    lv_bar_set_value(d->bar[4], ps_pct, LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d%%", int_pct);
    lv_label_set_text(d->bar_val[3], buf);
    snprintf(buf, sizeof(buf), "%d%%", ps_pct);
    lv_label_set_text(d->bar_val[4], buf);

    snprintf(buf, sizeof(buf), "INT %u/%uKB  PS %u/%uKB\nMIN FREE %uKB",
             (unsigned)(s->int_free / 1024), (unsigned)(s->int_total / 1024),
             (unsigned)(s->ps_free / 1024), (unsigned)(s->ps_total / 1024),
             (unsigned)(s->min_free / 1024));
    lv_label_set_text(d->info, buf);

    /* 虚拟滚动: 只渲染可见窗口 */
    for (int i = 0; i < TASK_ROWS; i++) {
        lv_obj_t **row = d->task_lab[i];
        int ti = i + d->scroll_off;
        if (ti < s->task_count) {
            char b0[24], b1[12], b2[12], b3[12];
            snprintf(b0, sizeof(b0), "%.12s", s->tasks[ti].name);
            snprintf(b1, sizeof(b1), "%u%%", s->tasks[ti].cpu);
            snprintf(b2, sizeof(b2), "%lu", (unsigned long)s->tasks[ti].stack_min);
            if (s->tasks[ti].core < 0) {
                snprintf(b3, sizeof(b3), "-  %s", state_str(s->tasks[ti].state));
            } else {
                snprintf(b3, sizeof(b3), "C%d %s", s->tasks[ti].core, state_str(s->tasks[ti].state));
            }
            lv_label_set_text(row[0], b0);
            lv_label_set_text(row[1], b1);
            lv_label_set_text(row[2], b2);
            lv_label_set_text(row[3], b3);
            for (int k = 0; k < 4; k++) {
                lv_obj_clear_flag(row[k], LV_OBJ_FLAG_HIDDEN);
            }
            bool low_stack = s->tasks[ti].stack_min < 1000;
            lv_obj_set_style_text_color(row[2],
                lv_color_hex(low_stack ? XK_COLOR_RED : XK_COLOR_TEXT), 0);
        } else {
            for (int k = 0; k < 4; k++) {
                lv_obj_add_flag(row[k], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

const page_ops_t pg_sysmon_ops = {
    .create = pg_sysmon_create,
    .destroy = pg_sysmon_destroy,
    .on_rotate = pg_sysmon_on_rotate,
    .on_back = pg_sysmon_on_back,
    .on_tick = NULL,
    .on_resume = NULL,
};
