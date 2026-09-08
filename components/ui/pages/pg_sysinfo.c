#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "wifi.h"
#include "mqtt.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_timer.h"

LV_FONT_DECLARE(lv_font_msyh_16);
LV_FONT_DECLARE(lv_font_montserrat_12);

/* ============================================================
 * 系统信息页
 * - 8 行: 名称(左) + 值(右, montserrat_12 限宽防叠加)
 * - 底部工厂测试入口整行可触摸
 * ============================================================ */

#define SYSINFO_ROWS 8

typedef struct {
    lv_obj_t *val[SYSINFO_ROWS];
} sysinfo_data_t;

/* 工厂测试入口点击 → 进入工厂测试页 */
static void sysinfo_factory_cb(lv_event_t *e)
{
    (void)e;
    pm_push(PAGE_FACTORY);
    pm_shake();
}

static void sysinfo_update(sysinfo_data_t *d)
{
    char buf[64];
    const esp_app_desc_t *app = esp_app_get_description();
    uint32_t uptime_s = esp_timer_get_time() / 1000000ULL;

    snprintf(buf, sizeof(buf), "v%s", app->version);
    lv_label_set_text(d->val[0], buf);

    char ip[16];
    wifi_get_ip_str(ip, sizeof(ip));
    lv_label_set_text(d->val[1], ip);

    bool m = mqtt_ha_is_connected();
    lv_label_set_text(d->val[2], m ? "ON" : "OFF");
    lv_obj_set_style_text_color(d->val[2], lv_color_hex(m ? XK_COLOR_GREEN : XK_COLOR_FAINT), 0);

    snprintf(buf, sizeof(buf), "%u d %02u h", (unsigned)(uptime_s / 86400),
             (unsigned)((uptime_s % 86400) / 3600));
    lv_label_set_text(d->val[3], buf);

    int t = display_get_screen_timeout();
    if (t <= 0) {
        lv_label_set_text(d->val[4], "OFF");
    } else {
        snprintf(buf, sizeof(buf), "%d min", t / 60);
        lv_label_set_text(d->val[4], buf);
    }

    snprintf(buf, sizeof(buf), "%s %s", app->date, app->time);
    lv_label_set_text(d->val[5], buf);

    snprintf(buf, sizeof(buf), "http://%s", ip);
    lv_label_set_text(d->val[6], buf);
}

static void pg_sysinfo_create(page_t *p)
{
    sysinfo_data_t *d = calloc(1, sizeof(sysinfo_data_t));
    p->data = d;
    p->title = "\xE7\xB3\xBB\xE7\xBB\x9F";

    static const char *labels[SYSINFO_ROWS] = {
        "VERSION",
        "IP",
        "MQTT",
        "UPTIME",
        "SLEEP",
        "BUILD",
        "WEB CFG",
        "\xE5\xB7\xA5\xE5\x8E\x82\xE6\xB5\x8B\xE8\xAF\x95",  /* 工厂测试 */
    };
    /* 行距收紧: 8 行全部放下, 值列限宽+小字体防长字段叠加 */
    static const int ys[SYSINFO_ROWS] = { 30, 63, 96, 129, 162, 195, 228, 264 };

    for (int i = 0; i < SYSINFO_ROWS; i++) {
        lv_obj_t *l = lv_label_create(p->root);
        lv_obj_set_style_text_color(l, lv_color_hex(XK_COLOR_GRAY), 0);
        lv_obj_set_style_text_font(l, &lv_font_msyh_16, 0);
        lv_label_set_text(l, labels[i]);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 14, ys[i]);

        lv_obj_t *v = lv_label_create(p->root);
        lv_obj_set_style_text_color(v, lv_color_hex(XK_COLOR_TEXT), 0);
        lv_obj_set_style_text_font(v, &lv_font_montserrat_12, 0);
        lv_label_set_text(v, "");
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_set_width(v, 118);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -14, ys[i] + 2);
        d->val[i] = v;
    }

    /* 工厂测试入口: 整行可触摸 */
    lv_obj_t *entry = lv_obj_create(p->root);
    lv_obj_remove_style_all(entry);
    lv_obj_set_size(entry, 240, 30);
    lv_obj_set_pos(entry, 0, ys[7] - 4);
    lv_obj_add_flag(entry, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(entry, sysinfo_factory_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    sysinfo_update(d);
    motor_set_mode(MOTOR_MODE_UNBOUND_NO_DETENTS, 0, 0);
}

static void pg_sysinfo_destroy(page_t *p)
{
    free(p->data);
    p->data = NULL;
}

static void pg_sysinfo_on_rotate(page_t *p, int32_t steps)
{
}

static void pg_sysinfo_on_back(page_t *p)
{
    pm_pop();
}

static void pg_sysinfo_on_tick(page_t *p)
{
    sysinfo_update((sysinfo_data_t *)p->data);
}

const page_ops_t pg_sysinfo_ops = {
    .create = pg_sysinfo_create,
    .destroy = pg_sysinfo_destroy,
    .on_rotate = pg_sysinfo_on_rotate,
    .on_back = pg_sysinfo_on_back,
    .on_resume = NULL,
    .on_tick = pg_sysinfo_on_tick,
};
