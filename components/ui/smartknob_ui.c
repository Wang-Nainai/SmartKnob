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

static const char *TAG = "smartknob_ui";

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ---------------- page table ---------------- */

extern const page_ops_t pg_startup_ops;
extern const page_ops_t pg_menu_ops;
extern const page_ops_t pg_playground_ops;
extern const page_ops_t pg_hass_ops;
extern const page_ops_t pg_env_ops;
extern const page_ops_t pg_setting_ops;
extern const page_ops_t pg_sysinfo_ops;

static const page_ops_t *const page_ops_table[PAGE_COUNT] = {
    [PAGE_STARTUP]    = &pg_startup_ops,
    [PAGE_MENU]       = &pg_menu_ops,
    [PAGE_PLAYGROUND] = &pg_playground_ops,
    [PAGE_HASS]       = &pg_hass_ops,
    [PAGE_ENV]        = &pg_env_ops,
    [PAGE_SETTING]    = &pg_setting_ops,
    [PAGE_SYSINFO]    = &pg_sysinfo_ops,
};

/* ---------------- page stack ---------------- */

#define PM_MAX_DEPTH 8
static page_t *pm_stack[PM_MAX_DEPTH];
static int pm_depth = 0;
static bool pm_animating = false;
static lv_obj_t *pm_screen = NULL;

static lv_obj_t *sb_wifi;
static lv_obj_t *sb_mqtt;
static lv_obj_t *sb_time;

static void knob_resync(void);

bool pm_busy(void)
{
    return pm_animating;
}

page_t *pm_top(void)
{
    return pm_depth > 0 ? pm_stack[pm_depth - 1] : NULL;
}

void pm_shake(void)
{
#if CONFIG_MOTOR_ENABLE
    motor_shake(2, 25);
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

static void pm_pop_anim_done(lv_anim_t *a)
{
    page_t *p = (page_t *)a->user_data;
    pm_animating = false;
    pm_delete_page(p);
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
    return p;
}

void pm_push(page_id_t id)
{
    if (pm_animating) {
        return;
    }
    if (pm_depth >= PM_MAX_DEPTH) {
        return;
    }
    page_t *old = pm_top();
    page_t *p = pm_create_page(id);
    if (!p) {
        return;
    }
    pm_stack[pm_depth++] = p;

    lv_obj_set_style_opa(p->root, LV_OPA_TRANSP, 0);
    lv_obj_set_x(p->root, 240);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p->root);
    lv_anim_set_values(&a, 240, 0);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_start(&a);

    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, p->root);
    lv_anim_set_values(&a2, 0, LV_OPA_COVER);
    lv_anim_set_time(&a2, 300);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a2, anim_opa_cb);
    lv_anim_set_ready_cb(&a2, pm_anim_done);
    lv_anim_start(&a2);

    pm_animating = true;
    knob_resync();
    ESP_LOGI(TAG, "push page %d (depth %d)", id, pm_depth);
}

void pm_replace(page_id_t id)
{
    if (pm_depth > 0) {
        page_t *p = pm_stack[pm_depth - 1];
        pm_stack[pm_depth - 1] = NULL;
        pm_depth--;
        pm_delete_page(p);
    }
    pm_push(id);
}

void pm_pop(void)
{
    if (pm_animating || pm_depth <= 1) {
        return;
    }
    page_t *p = pm_stack[pm_depth - 1];
    pm_depth--;
    pm_stack[pm_depth] = NULL;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p->root);
    lv_anim_set_values(&a, 0, 240);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_user_data(&a, p);
    lv_anim_start(&a);

    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, p->root);
    lv_anim_set_values(&a2, LV_OPA_COVER, 0);
    lv_anim_set_time(&a2, 250);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&a2, anim_opa_cb);
    lv_anim_set_ready_cb(&a2, pm_pop_anim_done);
    lv_anim_set_user_data(&a2, p);
    lv_anim_start(&a2);

    pm_animating = true;
    knob_resync();
    ESP_LOGI(TAG, "pop page (depth %d)", pm_depth);
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
    lv_obj_set_style_text_color(sb_wifi, lv_color_hex(w ? XK_COLOR_GREEN : XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_color(sb_mqtt, lv_color_hex(m ? XK_COLOR_BLUE : XK_COLOR_FAINT), 0);
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

    sb_wifi = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_wifi, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(sb_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(sb_wifi, LV_SYMBOL_WIFI " WiFi");
    lv_obj_align(sb_wifi, LV_ALIGN_LEFT_MID, 6, 0);

    sb_mqtt = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_mqtt, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(sb_mqtt, &lv_font_montserrat_14, 0);
    lv_label_set_text(sb_mqtt, LV_SYMBOL_BLUETOOTH " MQTT");
    lv_obj_align(sb_mqtt, LV_ALIGN_LEFT_MID, 76, 0);

    sb_time = lv_label_create(bar);
    lv_obj_set_style_text_color(sb_time, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(sb_time, &lv_font_montserrat_14, 0);
    lv_label_set_text(sb_time, "--:--");
    lv_obj_align(sb_time, LV_ALIGN_RIGHT_MID, -6, 0);
}

/* ---------------- knob gestures (no button: flick = click) ---------------- */

#define BURST_WINDOW_MS  250
#define FLICK_THRESHOLD  3
#define HOLD_OFF_MS      350

static int32_t last_knob_pos = 0;
static uint32_t burst_start_tick = 0;
static int32_t burst_delta = 0;
static uint32_t holdoff_until = 0;
static uint32_t last_rotate_tick = 0;

/* Page transitions change motor mode/position; re-sync gesture tracking */
static void knob_resync(void)
{
#if CONFIG_MOTOR_ENABLE
    if (motor_is_ready()) {
        last_knob_pos = motor_get_position();
    }
#endif
    burst_delta = 0;
}

static void knob_timer_cb(lv_timer_t *t)
{
#if CONFIG_MOTOR_ENABLE
    if (!motor_is_ready()) return;

    int32_t pos = motor_get_position();
    if (pos == last_knob_pos) return;

    uint32_t now = lv_tick_get();
    int32_t d = pos - last_knob_pos;
    last_knob_pos = pos;
    display_notify_activity();

    if (now < holdoff_until) return;

    if (now - burst_start_tick > BURST_WINDOW_MS) {
        burst_delta = 0;
    }
    burst_start_tick = now;
    burst_delta += d;

    page_t *top = pm_top();
    if (!top) return;

    if (burst_delta >= FLICK_THRESHOLD) {
        /* fast CW flick = confirm (X-Knob "click") */
        holdoff_until = now + HOLD_OFF_MS;
        burst_delta = 0;
        ESP_LOGI(TAG, "flick CW -> confirm");
        if (top->ops->on_confirm) top->ops->on_confirm(top);
    } else if (burst_delta <= -FLICK_THRESHOLD) {
        /* fast CCW flick = back (X-Knob "long press") */
        holdoff_until = now + HOLD_OFF_MS;
        burst_delta = 0;
        ESP_LOGI(TAG, "flick CCW -> back");
        if (top->ops->on_back) top->ops->on_back(top);
    } else {
        /* slow rotation = move focus / adjust value */
        if (now - last_rotate_tick > 30) {
            last_rotate_tick = now;
            if (top->ops->on_rotate) top->ops->on_rotate(top, d > 0 ? 1 : -1);
        }
    }
#else
    (void)t;
#endif
}

/* ---------------- periodic refresh ---------------- */

static void refresh_timer_cb(lv_timer_t *t)
{
    status_bar_update();
    page_t *top = pm_top();
    if (top && top->ops->on_tick) {
        top->ops->on_tick(top);
    }
}

/* ---------------- NVS persistence (brightness / timeout) ---------------- */

bool ui_nvs_load_i32(const char *key, int32_t *out)
{
    nvs_handle_t h;
    if (nvs_open("appcfg", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_i32(h, key, out);
    nvs_close(h);
    return err == ESP_OK;
}

void ui_nvs_save_i32(const char *key, int32_t value)
{
    nvs_handle_t h;
    if (nvs_open("appcfg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- env data bridge (SCD40 task -> UI, no LVGL in task) ---------------- */

static volatile uint16_t env_co2 = 0;
static volatile float env_temp = 0.0f;
static volatile float env_rh = 0.0f;
static volatile uint8_t env_has_data = 0;

void smartknob_ui_set_env(uint16_t co2_ppm, float temp_c, float humidity_pct)
{
    env_co2 = co2_ppm;
    env_temp = temp_c;
    env_rh = humidity_pct;
    env_has_data = 1;
}

void ui_env_get(uint16_t *co2, float *temp, float *rh, uint8_t *has_data)
{
    *co2 = env_co2;
    *temp = env_temp;
    *rh = env_rh;
    *has_data = env_has_data;
}

/* ---------------- init ---------------- */

void smartknob_ui_init(void)
{
    display_lvgl_lock();

    pm_screen = lv_screen_active();
    lv_obj_clean(pm_screen);
    lv_obj_set_style_bg_color(pm_screen, lv_color_hex(XK_COLOR_BG), 0);

    status_bar_create();

    /* restore persisted settings */
    int32_t brightness = 80, timeout = 30;
    ui_nvs_load_i32("brightness", &brightness);
    ui_nvs_load_i32("timeout", &timeout);
    display_set_brightness(brightness);
    display_set_screen_timeout(timeout);

    lv_timer_create(refresh_timer_cb, 500, NULL);
#if CONFIG_MOTOR_ENABLE
    lv_timer_create(knob_timer_cb, 20, NULL);
#endif

    /* stack starts at startup page, then replaced by menu */
    page_t *p = pm_create_page(PAGE_STARTUP);
    if (p) {
        pm_stack[pm_depth++] = p;
    }

    display_lvgl_unlock();

    ESP_LOGI(TAG, "SmartKnob UI initialized (X-Knob style)");
}