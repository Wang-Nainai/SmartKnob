#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "page_mgr.h"
#include "wifi.h"

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * SoftAP 配网页 (WiFi 超时未连上时由框架自动弹出)
 * 屏幕显示热点名/二维码/IP, 手机或电脑连接热点后
 * 浏览器打开管理页改 WiFi; 配网成功后热点自动关闭、本页自动退出。
 * ============================================================ */

static void pg_apcfg_create(page_t *p)
{
    p->title = "\xE9\x85\x8D\xE7\xBD\x91\xE6\xA8\xA1\xE5\xBC\x8F";  /* 配网模式 */

    lv_obj_t *title = lv_label_create(p->root);
    lv_obj_set_style_text_color(title, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(title, &lv_font_msyh_16, 0);
    lv_label_set_text(title, "WiFi \xE6\x9C\xAA\xE8\xBF\x9E\xE6\x8E\xA5\xEF\xBC\x8C\xE5\xB7\xB2\xE5\xBC\x80\xE5\x90\xAF\xE9\x85\x8D\xE7\xBD\x91\xE7\x83\xAD\xE7\x82\xB9");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    /* 二维码: 扫码直达管理页 */
    char url[48];
    char ip[16];
    wifi_ap_get_ip_str(ip, sizeof(ip));
    snprintf(url, sizeof(url), "http://%s", ip);
    lv_obj_t *qr = lv_qrcode_create(p->root);
    lv_obj_set_size(qr, 150, 150);
    lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 58);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_update(qr, url, strlen(url));

    /* 热点信息 */
    char buf[64];
    lv_obj_t *ap = lv_label_create(p->root);
    lv_obj_set_style_text_color(ap, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(ap, &lv_font_msyh_16, 0);
    snprintf(buf, sizeof(buf), "\xE7\x83\xAD\xE7\x82\xB9\x3A %s", wifi_ap_get_ssid());
    lv_label_set_text(ap, buf);
    lv_obj_align(ap, LV_ALIGN_TOP_MID, 0, 218);

    lv_obj_t *web = lv_label_create(p->root);
    lv_obj_set_style_text_color(web, lv_color_hex(XK_COLOR_BLUE), 0);
    lv_obj_set_style_text_font(web, &lv_font_montserrat_14, 0);
    lv_label_set_text(web, url);
    lv_obj_align(web, LV_ALIGN_TOP_MID, 0, 246);

    lv_obj_t *hint = lv_label_create(p->root);
    lv_obj_set_style_text_color(hint, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_msyh_16, 0);
    lv_label_set_text(hint, "\xE8\xBF\x9E\xE6\x8E\xA5\xE7\x83\xAD\xE7\x82\xB9\xE6\x89\xAB\xE7\xA0\x81\xE6\x94\xB9 WiFi\xEF\xBC\x8C\xE6\x88\x90\xE5\x8A\x9F\xE5\x90\x8E\xE8\x87\xAA\xE5\x8A\xA8\xE5\x85\xB3\xE9\x97\xAD");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    lv_obj_remove_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
}

static void pg_apcfg_destroy(page_t *p)
{
}

static void pg_apcfg_on_rotate(page_t *p, int32_t steps)
{
}

static void pg_apcfg_on_back(page_t *p)
{
    /* 用户手动关闭提示页(热点仍在, 状态栏可继续配网) */
    extern void ui_apcfg_set_dismissed(void);
    ui_apcfg_set_dismissed();
    pm_pop();
}

static void pg_apcfg_on_tick(page_t *p)
{
}

const page_ops_t pg_apcfg_ops = {
    .create = pg_apcfg_create,
    .destroy = pg_apcfg_destroy,
    .on_rotate = pg_apcfg_on_rotate,
    .on_back = pg_apcfg_on_back,
    .on_tick = pg_apcfg_on_tick,
    .on_resume = NULL,
};