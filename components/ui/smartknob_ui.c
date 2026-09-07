#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "display.h"
#include "smartknob_ui.h"
#include "page_mgr.h"
#include "mqtt.h"
#include "motor.h"
#include "wifi.h"
#include "input.h"
#include "app_state.h"
#include "blehid.h"

static const char *TAG = "smartknob_ui";

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ---------------- page table ---------------- */

extern const page_ops_t pg_startup_ops;
extern const page_ops_t pg_menu_ops;
extern const page_ops_t pg_pcdial_ops;
extern const page_ops_t pg_playground_ops;
extern const page_ops_t pg_hass_ops;
extern const page_ops_t pg_env_ops;
extern const page_ops_t pg_setting_ops;
extern const page_ops_t pg_sysinfo_ops;
extern const page_ops_t pg_factory_ops;
extern const page_ops_t pg_apcfg_ops;
extern const page_ops_t pg_sysmon_ops;

static const page_ops_t *const page_ops_table[PAGE_COUNT] = {
    [PAGE_STARTUP]    = &pg_startup_ops,
    [PAGE_MENU]       = &pg_menu_ops,
    [PAGE_PCDIAL]     = &pg_pcdial_ops,
    [PAGE_PLAYGROUND] = &pg_playground_ops,
    [PAGE_HASS]       = &pg_hass_ops,
    [PAGE_ENV]        = &pg_env_ops,
    [PAGE_SETTING]    = &pg_setting_ops,
    [PAGE_SYSINFO]    = &pg_sysinfo_ops,
    [PAGE_FACTORY]    = &pg_factory_ops,
    [PAGE_APCFG]      = &pg_apcfg_ops,
    [PAGE_SYSMON]     = &pg_sysmon_ops,
};

/* ---------------- page stack ---------------- */

#define PM_MAX_DEPTH 8
static page_t *pm_stack[PM_MAX_DEPTH];
static int pm_stack_depth = 0;
static bool pm_animating = false;
static lv_obj_t *pm_screen = NULL;

/* 状态栏部件 */
static lv_obj_t *sb_back_btn;
static lv_obj_t *sb_title;
static lv_obj_t *sb_wifi;
static lv_obj_t *sb_ble;
static lv_obj_t *sb_mqtt;
static lv_obj_t *sb_time;

static void status_bar_update(void);
static void page_gesture_cb(lv_event_t *e);
static void gesture_bubble_install(lv_obj_t *obj);

bool pm_busy(void)
{
    return pm_animating;
}

int pm_depth(void)
{
    return pm_stack_depth;
}

page_t *pm_top(void)
{
    return pm_stack_depth > 0 ? pm_stack[pm_stack_depth - 1] : NULL;
}

void pm_shake(void)
{
#if CONFIG_MOTOR_ENABLE
    motor_shake(1, 15);   /* 轻磕: (2,25) 实测过猛 */
#endif
}

static void anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
}

static void anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void pm_delete_page(page_t *p)
{
    if (!p) {
        return;
    }
    if (p->ops->destroy) {
        p->ops->destroy(p);
    }
    if (p->root) {
        lv_obj_delete(p->root);
    }
    free(p);
}

static void pm_anim_done(lv_anim_t *a)
{
    pm_animating = false;
}

static void pm_old_parallax_done(lv_anim_t *a)
{
    /* 视差页滑出后回位(被新页覆盖时不可见) */
    lv_obj_set_x((lv_obj_t *)a->var, 0);
}

static void pm_pop_anim_done(lv_anim_t *a)
{
    page_t *p = (page_t *)a->user_data;
    pm_animating = false;
    pm_delete_page(p);

    /* 恢复新栈顶页面的可见状态(如电机手感模式), 并同步旋钮基准 */
    page_t *top = pm_top();
    if (top && top->ops->on_resume) {
        top->ops->on_resume(top);
    }
    knob_input_reset();
}

static page_t *pm_create_page(page_id_t id)
{
    page_t *p = calloc(1, sizeof(page_t));
    if (!p) {
        return NULL;
    }
    p->ops = page_ops_table[id];
    p->root = lv_obj_create(pm_screen);
    lv_obj_remove_style_all(p->root);
    lv_obj_set_size(p->root, 240, 320);
    lv_obj_set_pos(p->root, 0, 0);
    lv_obj_set_style_bg_color(p->root, lv_color_hex(XK_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p->root, 0, 0);
    lv_obj_set_style_pad_all(p->root, 0, 0);
    if (p->ops->create) {
        p->ops->create(p);
    }
    lv_obj_add_event_cb(p->root, page_gesture_cb, LV_EVENT_GESTURE, NULL);
    gesture_bubble_install(p->root);
    return p;
}

void pm_push(page_id_t id)
{
    if (pm_animating) {
        return;
    }
    if (pm_stack_depth >= PM_MAX_DEPTH) {
        return;
    }
    page_t *old = pm_stack_depth > 0 ? pm_stack[pm_stack_depth - 1] : NULL;
    page_t *p = pm_create_page(id);
    if (!p) {
        return;
    }
    pm_stack[pm_stack_depth++] = p;

    /* 视差转场: 新页自右滑入, 旧页向左滑动 1/4 屏宽作纵深 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p->root);
    lv_anim_set_values(&a, 240, 0);
    lv_anim_set_time(&a, 320);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_start(&a);

    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, p->root);
    lv_anim_set_values(&a2, 0, LV_OPA_COVER);
    lv_anim_set_time(&a2, 320);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a2, anim_opa_cb);
    lv_anim_set_ready_cb(&a2, pm_anim_done);
    lv_anim_start(&a2);

    if (old) {
        lv_anim_t a3;
        lv_anim_init(&a3);
        lv_anim_set_var(&a3, old->root);
        lv_anim_set_values(&a3, 0, -60);
        lv_anim_set_time(&a3, 320);
        lv_anim_set_path_cb(&a3, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a3, anim_x_cb);
        lv_anim_set_ready_cb(&a3, pm_old_parallax_done);
        lv_anim_start(&a3);
    }

    pm_animating = true;
    knob_input_reset();          /* 清除旧输入残留 */
    status_bar_update();
    ESP_LOGI(TAG, "push page %d (depth %d)", id, pm_stack_depth);
}

void pm_replace(page_id_t id)
{
    if (pm_stack_depth > 0) {
page_t *p = pm_stack[pm_stack_depth - 1];
    pm_stack[pm_stack_depth - 1] = NULL;
    pm_stack_depth--;
    pm_delete_page(p);
    }
    pm_push(id);
}

void pm_pop(void)
{
    if (pm_animating || pm_stack_depth <= 1) {
        return;
    }
    page_t *p = pm_stack[pm_stack_depth - 1];
    pm_stack_depth--;
    pm_stack[pm_stack_depth] = NULL;
    page_t *under = pm_stack_depth > 0 ? pm_stack[pm_stack_depth - 1] : NULL;

    /* 视差转场(反向): 顶页滑出, 下层页从 -60 滑回原位 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p->root);
    lv_anim_set_values(&a, 0, 240);
    lv_anim_set_time(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_user_data(&a, p);
    lv_anim_start(&a);

    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, p->root);
    lv_anim_set_values(&a2, LV_OPA_COVER, 0);
    lv_anim_set_time(&a2, 260);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a2, anim_opa_cb);
    lv_anim_set_ready_cb(&a2, pm_pop_anim_done);
    lv_anim_set_user_data(&a2, p);
    lv_anim_start(&a2);

    if (under) {
        lv_anim_t a3;
        lv_anim_init(&a3);
        lv_anim_set_var(&a3, under->root);
        lv_anim_set_values(&a3, -60, 0);
        lv_anim_set_time(&a3, 260);
        lv_anim_set_path_cb(&a3, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a3, anim_x_cb);
        lv_anim_start(&a3);
    }

    pm_animating = true;
    knob_input_reset();          /* 清除旧输入残留 */
    status_bar_update();
    ESP_LOGI(TAG, "pop page (depth %d)", pm_stack_depth);
}

/* ---------------- status bar ---------------- */

static void status_bar_update(void)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    char buf[24];
    if (ti.tm_year > 120) {
        snprintf(buf, sizeof(buf), "%02d:%02d", ti.tm_hour, ti.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    lv_label_set_text(sb_time, buf);

    bool w = wifi_is_connected();
    bool m = mqtt_ha_is_connected();
    bool b = blehid_is_connected();
    lv_obj_set_style_text_color(sb_wifi, lv_color_hex(w ? XK_COLOR_GREEN : XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_color(sb_mqtt, lv_color_hex(m ? XK_COLOR_BLUE : XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_color(sb_ble, lv_color_hex(b ? XK_COLOR_GREEN : XK_COLOR_FAINT), 0);

    /* 返回按钮: 仅当页面栈深度 >1 时显示 */
    bool show_back = pm_stack_depth > 1;
    lv_obj_set_style_opa(sb_back_btn, show_back ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(sb_back_btn, LV_OBJ_FLAG_CLICKABLE);
    if (show_back) {
        lv_obj_add_flag(sb_back_btn, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 标题 */
    page_t *top = pm_top();
    const char *t = (top && top->title) ? top->title : "";
    lv_label_set_text(sb_title, t);
}

/* 状态栏返回按钮触摸回调 */
static void sb_back_cb(lv_event_t *e)
{
    if (pm_busy()) {
        return;   /* 转场动画期间不触发页面内部逻辑 */
    }
    page_t *top = pm_top();
    if (top && top->ops->on_back) {
        top->ops->on_back(top);
    }
    pm_shake();
}

static void status_bar_create(void)
{
    lv_obj_t *top = lv_layer_top();

    lv_obj_t *bar = lv_obj_create(top);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 22);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_90, 0);
    lv_obj_set_style_border_width(bar, 0, 0);

    sb_back_btn = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_back_btn, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(sb_back_btn, &lv_font_montserrat_14, 0);
    lv_label_set_text(sb_back_btn, LV_SYMBOL_LEFT);
    lv_obj_align(sb_back_btn, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_ext_click_area(sb_back_btn, 25);   /* 14px 符号太小, 大幅扩大触摸热区 */
    lv_obj_add_event_cb(sb_back_btn, sb_back_cb, LV_EVENT_CLICKED, NULL);

    sb_title = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_title, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(sb_title, &lv_font_msyh_16, 0);
    lv_label_set_text(sb_title, "");
    /* 左对齐+限宽: 避免长标题(如 SmartKnob)与右侧 WiFi/BLE/MQTT 图标重叠 */
    lv_obj_set_width(sb_title, 100);
    lv_label_set_long_mode(sb_title, LV_LABEL_LONG_DOT);
    lv_obj_align(sb_title, LV_ALIGN_LEFT_MID, 30, 0);

    sb_wifi = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_wifi, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(sb_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(sb_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(sb_wifi, LV_ALIGN_RIGHT_MID, -90, 0);

    sb_ble = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_ble, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(sb_ble, &lv_font_montserrat_14, 0);
    lv_label_set_text(sb_ble, LV_SYMBOL_BLUETOOTH);
    lv_obj_align(sb_ble, LV_ALIGN_RIGHT_MID, -66, 0);

    sb_mqtt = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_mqtt, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(sb_mqtt, &lv_font_montserrat_14, 0);
    lv_label_set_text(sb_mqtt, LV_SYMBOL_UPLOAD);
    lv_obj_align(sb_mqtt, LV_ALIGN_RIGHT_MID, -44, 0);

    sb_time = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_time, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(sb_time, &lv_font_montserrat_14, 0);
    lv_label_set_text(sb_time, "--:--");
    lv_obj_align(sb_time, LV_ALIGN_RIGHT_MID, -6, 0);

    lv_obj_add_event_cb(bar, page_gesture_cb, LV_EVENT_GESTURE, NULL);
    gesture_bubble_install(bar);
}

/* ---------------- 触摸手势 (LVGL 原生 GESTURE 事件) ----------------
 * 拖动超过阈值(20px)时 LVGL 发送 LV_EVENT_GESTURE:
 *   - S-Dial 页: 横滑=音量加减, 竖滑=鼠标滚轮
 *   - 其它页: 左右横滑 = 返回
 * 回调内 lv_indev_reset 取消本次按压, 手势绝不误触发 CLICKED */
static void page_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    display_notify_activity();
    if (pm_animating) {
        return;
    }
    page_t *top = pm_top();
    if (!top) {
        return;
    }
    if (top->ops == &pg_pcdial_ops) {
        extern void pg_pcdial_gesture(lv_dir_t dir);
        pg_pcdial_gesture(dir);
        lv_indev_reset(indev, NULL);
        return;
    }
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        if (top->ops->on_back) {
            top->ops->on_back(top);
            pm_shake();
        }
        lv_indev_reset(indev, NULL);
    }
}

/* 子控件开启手势冒泡, 使 GESTURE 事件汇聚到容器(页面根/状态栏) */
static void gesture_bubble_install(lv_obj_t *obj)
{
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        lv_obj_add_flag(c, LV_OBJ_FLAG_GESTURE_BUBBLE);
        gesture_bubble_install(c);
    }
}

/* ---------------- 旋钮输入消费 (LVGL 任务内) ---------------- */

static void input_timer_cb(lv_timer_t *t)
{
    (void)t;
    knob_event_t evt;
    bool woke = false;
    while (knob_input_get_event(&evt)) {
        if (!woke) {
            display_notify_activity();   /* 旋转唤醒/重置熄屏计时 */
            woke = true;
        }
        if (pm_animating) {
            continue;                    /* 页面切换动画期间丢弃输入 */
        }
        page_t *top = pm_top();
        if (!top) {
            continue;
        }
        if (evt.type == KNOB_EVENT_ROTATE && top->ops->on_rotate) {
            top->ops->on_rotate(top, evt.steps);
        }
    }
}

/* ---------------- periodic refresh ---------------- */

/* SoftAP 配网页状态(仅 LVGL 任务访问) */
static bool s_apcfg_dismissed = false;

void ui_apcfg_set_dismissed(void)
{
    s_apcfg_dismissed = true;
}

static void apcfg_poll(void)
{
    bool ap = wifi_ap_is_active();
    page_t *top = pm_top();
    bool on_ap_page = (top && top->ops == &pg_apcfg_ops);

    if (ap && !on_ap_page) {
        if (!s_apcfg_dismissed) {
            pm_push(PAGE_APCFG);   /* 热点开启: 自动弹配网页 */
        }
    } else if (!ap && on_ap_page) {
        pm_pop();                  /* 配网成功/热点关闭: 自动退出 */
    }
}

static void refresh_timer_cb(lv_timer_t *t)
{
    status_bar_update();
    apcfg_poll();
    page_t *top = pm_top();
    if (top && top->ops->on_tick) {
        top->ops->on_tick(top);
    }
}

/* ---------------- NVS persistence (brightness / timeout) ---------------- */

bool ui_nvs_load_i32(const char *key, int32_t *out)
{
    nvs_handle_t h;
    if (nvs_open("webcfg", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_i32(h, key, out);
    nvs_close(h);
    return err == ESP_OK;
}

void ui_nvs_save_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open("webcfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- env data (来自 app_state, 由 scd40 任务写入) ---------------- */

void ui_env_get(uint16_t *co2, float *temp, float *rh, uint8_t *has_data)
{
    app_env_t env;
    app_state_get_env(&env);
    *co2 = env.co2_ppm;
    *temp = env.temperature_c;
    *rh = env.humidity_pct;
    *has_data = env.has_data;
}

/* ---------------- init ---------------- */

void smartknob_ui_init(void)
{
    display_lvgl_lock();

    pm_screen = lv_screen_active();
    lv_obj_clean(pm_screen);
    lv_obj_set_style_bg_color(pm_screen, lv_color_hex(XK_COLOR_BG), 0);

    status_bar_create();
    status_bar_update();

    /* restore persisted settings */
    int32_t brightness = 100, timeout = 2;
    ui_nvs_load_i32("brightness", &brightness);
    ui_nvs_load_i32("timeout", &timeout);
    display_set_brightness(brightness);
    display_set_screen_timeout(timeout * 60);

    lv_timer_create(refresh_timer_cb, 500, NULL);
    lv_timer_create(input_timer_cb, 20, NULL);

    /* stack starts at startup page, then replaced by menu */
    page_t *p = pm_create_page(PAGE_STARTUP);
    if (p) {
        pm_stack[pm_stack_depth++] = p;
    }
    status_bar_update();

    display_lvgl_unlock();

    ESP_LOGI(TAG, "SmartKnob UI initialized (X-Knob style, touch+knob)");
}