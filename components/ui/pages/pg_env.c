#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "smartknob_ui.h"
#include "env_hist.h"
#include "widgets/chart/lv_chart_private.h"   /* ser->y_points/start_point 全量重建 */

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_16);
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
    lv_obj_t *chart;        /* 趋势图(单击切换度量) */
    lv_chart_series_t *ser;
    lv_obj_t *cap;          /* 图内角标 */
    uint8_t metric;         /* 0=CO2 1=温度(x10) 2=湿度(x10) */
    uint32_t last_seq;      /* 上次已同步的采样序号 */
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

/* 圆角卡片: 名称 + 数值 (加宽铺满两侧, 数值字号收敛一档) */
static void env_card(lv_obj_t *parent, int x, const char *name,
                     lv_obj_t **value_out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 104, 60);
    lv_obj_set_pos(card, x, 158);
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
    lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *val = lv_label_create(card);
    lv_obj_set_style_text_color(val, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_label_set_text(val, "--");
    /* 固定宽度+居中: 数值滚动过渡时新旧文本完全重合, 位数变化不位移 */
    lv_obj_set_width(val, 96);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(val, LV_ALIGN_BOTTOM_MID, 0, -6);

    *value_out = val;
}

/* ---- 趋势图度量: 单击循环 CO2 -> 温度 -> 湿度 ---- */
#define METRIC_CO2   0
#define METRIC_TEMP  1
#define METRIC_RH    2

static const char *metric_caption[3] = {"CO2 ppm / 2h", "Temp C / 2h", "RH % / 2h"};

static int32_t env_metric_value(uint8_t metric, int i)
{
    switch (metric) {
    case METRIC_TEMP:
        return env_hist_temp_x10_at(i);
    case METRIC_RH:
        return env_hist_rh_x10_at(i);
    default:
        return env_hist_co2_at(i);
    }
}

static void env_metric_clamp(uint8_t metric, int32_t *v)
{
    if (metric == METRIC_CO2) {
        if (*v < 400) *v = 400;
        if (*v > 2500) *v = 2500;
    } else if (metric == METRIC_TEMP) {
        if (*v < 50) *v = 50;     /* 5.0 C */
        if (*v > 400) *v = 400;   /* 40.0 C */
    } else {
        if (*v < 0) *v = 0;
        if (*v > 1000) *v = 1000; /* 100 % */
    }
}

/* 按当前度量重配量程/颜色/角标, 并用缓冲全量重建曲线 */
static void env_metric_apply(env_data_t *d)
{
    static const uint32_t col_fix[3] = {0, 0x3399FF, 0x00C8B4};
    static const int32_t rmin[3] = {400, 50, 0};
    static const int32_t rmax[3] = {2500, 400, 1000};

    lv_chart_set_range(d->chart, LV_CHART_AXIS_PRIMARY_Y, rmin[d->metric], rmax[d->metric]);
    uint32_t col = (d->metric == METRIC_CO2) ? env_level_color(d->co2)
                                             : col_fix[d->metric];
    lv_chart_set_series_color(d->chart, d->ser, lv_color_hex(col));
    lv_label_set_text(d->cap, metric_caption[d->metric]);

    int cnt = env_hist_count();
    for (int id = 0; id < ENV_HIST_N; id++) {
        if (id < cnt) {
            int32_t v = env_metric_value(d->metric, id);
            env_metric_clamp(d->metric, &v);
            d->ser->y_points[id] = v;
        } else {
            d->ser->y_points[id] = LV_CHART_POINT_NONE;
        }
    }
    d->ser->start_point = cnt % ENV_HIST_N;
    lv_chart_refresh(d->chart);
}

static void env_chart_click_cb(lv_event_t *e)
{
    env_data_t *d = lv_event_get_user_data(e);
    d->metric = (d->metric + 1) % 3;
    env_metric_apply(d);
    pm_shake();
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
        ui_label_roll(d->label_value, buf);
        lv_obj_set_style_text_color(d->label_value, lv_color_hex(col), 0);

        lv_bar_set_value(d->bar, d->co2 > 5000 ? 5000 : d->co2, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(d->bar, lv_color_hex(col), LV_PART_INDICATOR);
        if (d->metric == METRIC_CO2) {
            lv_chart_set_series_color(d->chart, d->ser, lv_color_hex(col));
        }

        snprintf(buf, sizeof(buf), "%.1f", d->temp);
        ui_label_roll(d->label_t, buf);
        snprintf(buf, sizeof(buf), "%.0f", d->rh);
        ui_label_roll(d->label_h, buf);
    } else {
        lv_obj_set_style_bg_color(d->badge, lv_color_hex(XK_COLOR_FAINT), 0);
        lv_label_set_text(d->label_badge, "\xE7\xAD\x89\xE5\xBE\x85\xE6\x95\xB0\xE6\x8D\xAE");
        lv_label_set_text(d->label_value, "--");
        lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_bar_set_value(d->bar, 0, LV_ANIM_OFF);
        lv_label_set_text(d->label_t, "--");
        lv_label_set_text(d->label_h, "--");
    }

    /* 趋势图同步: 漏掉几个样本就补几个(补最新的, 按当前度量取值) */
    uint32_t seq = env_hist_seq();
    if (seq != d->last_seq) {
        int cnt = env_hist_count();
        uint32_t diff = seq - d->last_seq;
        d->last_seq = seq;
        if (cnt > 0) {
            int start = (diff > (uint32_t)cnt) ? cnt : (int)diff;
            for (int k = start; k > 0; k--) {
                int32_t v = env_metric_value(d->metric, cnt - k);
                env_metric_clamp(d->metric, &v);
                lv_chart_set_next_value(d->chart, d->ser, v);
            }
        }
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
    lv_obj_set_size(d->badge, 72, 24);
    lv_obj_set_pos(d->badge, 84, 32);
    lv_obj_set_style_bg_color(d->badge, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_bg_opa(d->badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d->badge, 12, 0);
    lv_obj_clear_flag(d->badge, LV_OBJ_FLAG_SCROLLABLE);

    d->label_badge = lv_label_create(d->badge);
    lv_obj_set_style_text_color(d->label_badge, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(d->label_badge, &lv_font_msyh_16, 0);
    lv_label_set_text(d->label_badge, "\xE7\xAD\x89\xE5\xBE\x85\xE6\x95\xB0\xE6\x8D\xAE");
    lv_obj_center(d->label_badge);

    /* ---- CO2 大数字 (固定宽度居中: 位数变化不位移, 滚动过渡重合) ---- */
    d->label_value = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_value, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_value, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->label_value, "--");
    lv_obj_set_width(d->label_value, 216);
    lv_obj_set_style_text_align(d->label_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(d->label_value, LV_ALIGN_TOP_MID, 0, 62);

    lv_obj_t *unit = lv_label_create(p->root);
    lv_obj_set_style_text_color(unit, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_label_set_text(unit, "CO2 / ppm");
    lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 120);

    /* ---- CO2 彩色条(0-5000ppm) ---- */
    d->bar = lv_bar_create(p->root);
    lv_obj_set_size(d->bar, 216, 8);
    lv_obj_set_pos(d->bar, 12, 142);
    lv_bar_set_range(d->bar, 0, 5000);
    lv_bar_set_value(d->bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(d->bar, 4, 0);
    lv_obj_set_style_bg_color(d->bar, lv_color_hex(XK_COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d->bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(d->bar, lv_color_hex(XK_COLOR_FAINT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(d->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(d->bar, 4, LV_PART_INDICATOR);

    /* ---- 温度 / 湿度 卡片 (加宽: 12px 边距 + 8px 间隙铺满) ---- */
    env_card(p->root, 12, "\xE6\xB8\xA9\xE5\xBA\xA6", &d->label_t);
    env_card(p->root, 124, "\xE6\xB9\xBF\xE5\xBA\xA6", &d->label_h);

    /* ---- CO2 趋势图 (2h @ 60s, 数据来自 env_hist) ---- */
    d->chart = lv_chart_create(p->root);
    lv_obj_set_size(d->chart, 216, 86);
    lv_obj_set_pos(d->chart, 12, 226);
    lv_chart_set_type(d->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(d->chart, ENV_HIST_N);
    lv_chart_set_update_mode(d->chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(d->chart, 3, 0);
    lv_chart_set_range(d->chart, LV_CHART_AXIS_PRIMARY_Y, 400, 2500);
    lv_obj_set_style_bg_color(d->chart, lv_color_hex(XK_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(d->chart, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(d->chart, lv_color_hex(XK_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(d->chart, 1, 0);
    lv_obj_set_style_radius(d->chart, 8, 0);
    lv_obj_set_style_pad_all(d->chart, 2, 0);
    lv_obj_set_style_line_color(d->chart, lv_color_hex(XK_COLOR_BORDER), LV_PART_MAIN);

    d->ser = lv_chart_add_series(d->chart, lv_color_hex(0x00C864), LV_CHART_AXIS_PRIMARY_Y);
    lv_obj_set_style_line_width(d->chart, 1, LV_PART_ITEMS);

    lv_obj_t *cap = lv_label_create(d->chart);
    lv_obj_set_style_text_color(cap, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_label_set_text(cap, metric_caption[METRIC_CO2]);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, 4, 2);
    d->cap = cap;

    /* 单击循环切换 CO2/温度/湿度 曲线 */
    lv_obj_add_flag(d->chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->chart, env_chart_click_cb, LV_EVENT_CLICKED, d);

    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);

    d->metric = METRIC_CO2;
    env_metric_apply(d);
    d->last_seq = env_hist_seq();

    d->timer = lv_timer_create(env_timer_cb, 1000, d);
    /* 有档位手感: 一格 = 切换一次趋势图度量 */
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
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
    env_data_t *d = p->data;
    if (steps == 0) {
        return;
    }
    /* 旋转(有段落感)切换度量: CO2 -> 温度 -> 湿度 循环 */
    d->metric = (uint8_t)(((d->metric + steps) % 3 + 3) % 3);
    env_metric_apply(d);
    pm_shake();
}

static void pg_env_on_back(page_t *p)
{
    pm_pop();
}

static void pg_env_on_resume(page_t *p)
{
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
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
    .on_resume = pg_env_on_resume,
    .on_tick = pg_env_on_tick,
};
