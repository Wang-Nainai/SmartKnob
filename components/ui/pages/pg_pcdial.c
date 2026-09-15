#include <stdio.h>
#include <stdlib.h>
#include "page_mgr.h"
#include "motor.h"
#include "blehid.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_msyh_16);

/* NVS persistence helpers from smartknob_ui.c */
bool ui_nvs_load_i32(const char *key, int32_t *out);
bool ui_nvs_save_i32(const char *key, int32_t value);

/* ============================================================
 * S-Dial 电脑控制 (仿 X-Knob Surface Dial)
 * BLE HID 连接电脑, Windows 免驱。两种主机协议 (长按模式键切换):
 *   Surface Dial 模式 (默认, Win10 1903+ 原生支持):
 *     旋转 = 系统拨号盘(音量/滚动由 Windows 处理)
 *     单击中 = 静音切换, 长按中 = 弹系统圆盘菜单 (系统语义)
 *   媒体键模式 (Android 兼容):
 *     旋转 = 音量加减 / 鼠标滚轮, 点击 = 播放/暂停, 底部 = 切歌
 * ============================================================ */

typedef enum {
    PC_MODE_VOLUME = 0,
    PC_MODE_SCROLL,
    PC_MODE_COUNT,
} pc_mode_t;

typedef struct {
    pc_mode_t mode;
    bool dial_mode;     /* true=Surface Dial 原生协议(Windows), false=媒体键(兼容) */
    lv_obj_t *label_ble;
    lv_obj_t *btn_mode;
    lv_obj_t *label_mode;
    lv_obj_t *btn_play;
    lv_obj_t *label_play;
    lv_obj_t *btn_prev;
    lv_obj_t *btn_next;
} pc_data_t;

static void pc_mode_refresh(pc_data_t *d);

static bool pc_dial_mode_get(void)
{
    int32_t v = 1;
    ui_nvs_load_i32("pcdial_dial", &v);
    return v != 0;
}

static void pc_dial_mode_save(bool dial)
{
    ui_nvs_save_i32("pcdial_dial", dial ? 1 : 0);
}

static void pc_ble_status_refresh(pc_data_t *d)
{
    if (blehid_is_connected()) {
        lv_label_set_text(d->label_ble, "\xE8\x93\x9D\xE7\x89\x99\x3A \xE5\xB7\xB2\xE8\xBF\x9E\xE6\x8E\xA5");
        lv_obj_set_style_text_color(d->label_ble, lv_color_hex(XK_COLOR_GREEN), 0);
    } else {
        lv_label_set_text(d->label_ble, "\xE8\x93\x9D\xE7\x89\x99\x3A \xE6\x9C\xAA\xE8\xBF\x9E\xE6\x8E\xA5");
        lv_obj_set_style_text_color(d->label_ble, lv_color_hex(XK_COLOR_GRAY), 0);
    }
}

static void pc_mode_refresh(pc_data_t *d)
{
    if (d->dial_mode) {
        lv_label_set_text(d->label_mode, "\xE4\xB8\xBB\xE6\x9C\xBA\x3A \xE8\xA1\xA8\xE7\x9B\x98"); /* 主机: 表盘 */
    } else {
        lv_label_set_text(d->label_mode, "\xE4\xB8\xBB\xE6\x9C\xBA\x3A \xE5\xAA\x92\xE4\xBD\x93\xE9\x94\xAE"); /* 主机: 媒体键 */
    }
}

static void pc_apply_rotate(pc_data_t *d, int32_t steps)
{
    if (!blehid_is_connected()) {
        return;
    }
    /* 节流: consumer_send 内部有 8ms 延时且在 LVGL 任务执行,
     * 快转时限制发送频率避免 UI 卡顿/音量飞转 */
    static uint32_t last_send_tick = 0;
    uint32_t now = lv_tick_get();
    if (now - last_send_tick < 40) {
        return;
    }
    last_send_tick = now;

    if (d->dial_mode) {
        /* Surface Dial 原生: 系统拨号盘, 音量/滚动由 Windows 语义处理 */
        blehid_dial_rotate(steps);
        return;
    }
    if (d->mode == PC_MODE_VOLUME) {
        /* Windows 每个音量报告 = 2 格; 单事件最多 2 次 */
        int n = steps > 0 ? steps : -steps;
        if (n > 2) n = 2;
        for (int i = 0; i < n; i++) {
            blehid_consumer_send(steps > 0 ? HID_CONSUMER_VOLUME_UP : HID_CONSUMER_VOLUME_DOWN);
        }
    } else {
        for (int i = 0; i < steps; i++) {
            blehid_mouse_scroll(steps > 0 ? 1 : -1);
        }
    }
}

/* 模式切换按钮: 单击=音量/滚轮, 长按=Surface Dial/媒体键(主机协议) */
static void pc_mode_btn_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target(e);
    page_t *p = (page_t *)lv_obj_get_user_data(obj);
    pc_data_t *d = p->data;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        d->dial_mode = !d->dial_mode;
        pc_dial_mode_save(d->dial_mode);
        pc_mode_refresh(d);
        pm_shake();
    } else if (code == LV_EVENT_SHORT_CLICKED || code == LV_EVENT_CLICKED) {
        if (d->dial_mode) {
            return;   /* 表盘模式下 Windows 管语义, 无需本地模式 */
        }
        d->mode = (d->mode + 1) % PC_MODE_COUNT;
        pc_mode_refresh(d);
        pm_shake();
    }
}

/* 中间播放/暂停 (媒体键模式); Surface Dial 模式下中间按压=系统菜单选择 */
static void pc_tap_cb(lv_event_t *e)
{
    (void)e;
    page_t *p = NULL;
    if (blehid_is_connected()) {
        blehid_consumer_send(HID_CONSUMER_PLAY_PAUSE);
        pm_shake();
    }
}

static void pc_prev_cb(lv_event_t *e)
{
    (void)e;
    if (blehid_is_connected()) {
        blehid_consumer_send(HID_CONSUMER_SCAN_PREV);
        pm_shake();
    }
}

static void pc_next_cb(lv_event_t *e)
{
    (void)e;
    if (blehid_is_connected()) {
        blehid_consumer_send(HID_CONSUMER_SCAN_NEXT);
        pm_shake();
    }
}

static lv_obj_t *pc_button_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                  lv_coord_t w, lv_coord_t h, const char *text,
                                  lv_event_cb_t cb, page_t *p)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(XK_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(XK_COLOR_BORDER), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(btn, p);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(label, &lv_font_msyh_16, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void pg_pcdial_create(page_t *p)
{
    pc_data_t *d = calloc(1, sizeof(pc_data_t));
    p->data = d;
    d->mode = PC_MODE_VOLUME;
    d->dial_mode = pc_dial_mode_get();
    p->title = "S-Dial";

    d->label_ble = lv_label_create(p->root);
    lv_obj_set_style_text_font(d->label_ble, &lv_font_msyh_16, 0);
    lv_obj_align(d->label_ble, LV_ALIGN_TOP_MID, 0, 30);
    pc_ble_status_refresh(d);

    d->btn_mode = pc_button_create(p->root, 50, 56, 140, 36,
                                   "", pc_mode_btn_cb, p);
    d->label_mode = lv_obj_get_child(d->btn_mode, 0);
    pc_mode_refresh(d);

    /* 中间播放区 (116px, 不遮挡上下文字) */
    d->btn_play = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->btn_play);
    lv_obj_set_size(d->btn_play, 116, 116);
    lv_obj_align(d->btn_play, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_bg_color(d->btn_play, lv_color_hex(XK_COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(d->btn_play, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d->btn_play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(d->btn_play, lv_color_hex(XK_COLOR_ACCENT), 0);
    lv_obj_set_style_border_width(d->btn_play, 2, 0);
    lv_obj_set_user_data(d->btn_play, p);
    lv_obj_add_event_cb(d->btn_play, pc_tap_cb, LV_EVENT_CLICKED, NULL);

    d->label_play = lv_label_create(d->btn_play);
    lv_obj_set_style_text_color(d->label_play, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(d->label_play, &lv_font_montserrat_48, 0);
    lv_label_set_text(d->label_play, LV_SYMBOL_PLAY);
    lv_obj_center(d->label_play);

    /* 上一首 / 下一首 */
    d->btn_prev = pc_button_create(p->root, 10, 246, 105, 40,
                                   "\xE4\xB8\x8A\xE4\xB8\x80\xE9\xA6\x96", pc_prev_cb, p);
    d->btn_next = pc_button_create(p->root, 125, 246, 105, 40,
                                   "\xE4\xB8\x8B\xE4\xB8\x80\xE9\xA6\x96", pc_next_cb, p);

    /* 配对提示 */
    lv_obj_t *hint = lv_label_create(p->root);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE8\x93\x9D\xE7\x89\x99\xE8\xAE\xBE\xE7\xBD\xAE\xE4\xB8\xAD\xE6\x90\x9C\xE7\xB4\xA2 SmartKnob"
                             "\n\xE9\x95\xBF\xE6\x8C\x89\xE6\xA8\xA1\xE5\xBC\x8F\xE9\x94\xAE\xE5\x88\x87\xE6\x8D\xA2\xE4\xB8\xBB\xE6\x9C\xBA\xE5\x8D\x8F\xE8\xAE\xAE"); /* 长按模式键切换主机协议 */
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    /* 棘轮手感: 旋转一格一咔哒(齿轮感), 一格=一次音量/滚轮动作 */
    motor_set_mode(MOTOR_MODE_UNBOUNDED_DETENTS, 0, 0);
}

static void pg_pcdial_destroy(page_t *p)
{
    free(p->data);
    p->data = NULL;
}

static void pg_pcdial_on_rotate(page_t *p, int32_t steps)
{
    pc_apply_rotate((pc_data_t *)p->data, steps);
}

static void pg_pcdial_on_back(page_t *p)
{
    pm_pop();
}

static void pg_pcdial_on_tick(page_t *p)
{
    pc_ble_status_refresh((pc_data_t *)p->data);
}

/* 触摸手势(LVGL 原生 GESTURE 事件, 由 smartknob_ui 路由):
 * 横滑=音量加减, 竖滑=鼠标滚轮 */
void pg_pcdial_gesture(lv_dir_t dir)
{
    if (!blehid_is_connected()) {
        return;
    }
    /* 节流: 手势连发时限制发送频率 */
    static uint32_t last_gesture_tick = 0;
    uint32_t now = lv_tick_get();
    if (now - last_gesture_tick < 150) {
        return;
    }
    last_gesture_tick = now;

    switch (dir) {
    case LV_DIR_RIGHT:
        blehid_consumer_send(HID_CONSUMER_VOLUME_UP);
        pm_shake();
        break;
    case LV_DIR_LEFT:
        blehid_consumer_send(HID_CONSUMER_VOLUME_DOWN);
        pm_shake();
        break;
    case LV_DIR_TOP:
        blehid_mouse_scroll(2);
        break;
    case LV_DIR_BOTTOM:
        blehid_mouse_scroll(-2);
        break;
    default:
        break;
    }
}

const page_ops_t pg_pcdial_ops = {
    .create = pg_pcdial_create,
    .destroy = pg_pcdial_destroy,
    .on_rotate = pg_pcdial_on_rotate,
    .on_back = pg_pcdial_on_back,
    .on_resume = NULL,
    .on_tick = pg_pcdial_on_tick,
};