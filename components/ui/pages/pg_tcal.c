#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "page_mgr.h"
#include "display.h"
#include "motor.h"

LV_FONT_DECLARE(lv_font_montserrat_26);
LV_FONT_DECLARE(lv_font_msyh_16);

/* ============================================================
 * 触摸四点校准向导
 * 电阻屏板轴与屏幕的 轴序/镜像/缩放/偏移 无法预先确定,
 * 开机(未校准时)依次点击 4 个角标, 最小二乘拟合仿射变换:
 *   x' = a*rx + b*ry + c    y' = d*rx + e*ry + f
 * 拟合残差过大自动重来, 通过后存 NVS, 之后每次开机直接生效。
 * 旋钮累计转 12 格可跳过校准(使用出厂默认映射)。
 * ============================================================ */

typedef struct {
    int target;                 /* 当前目标点 0..3 */
    int16_t rx[4], ry[4];       /* 采样的板轴原始值 */
    bool prev_touched;
    int32_t skip_steps;         /* 旋钮跳过累计 */
    lv_obj_t *dot;              /* 十字准星 */
    lv_obj_t *label_tip;
    lv_timer_t *timer;          /* 成功展示后自动进入主页 */
    bool done;                  /* 已完成, 等待切换 */
} tcal_data_t;

static const lv_point_t targets[4] = {
    {28, 42}, {212, 42}, {212, 278}, {28, 278}
};

static const char TXT_TITLE[]   = "\xE8\xA7\xA6\xE6\x91\xB8\xE6\xA0\xA1\xE5\x87\x86"; /* 触摸校准 */
static const char TXT_TIP[]     = "\xE8\xAF\xB7\xE4\xBE\x9D\xE6\xAC\xA1\xE7\x82\xB9\xE5\x87\xBB 4 \xE4\xB8\xAA\xE5\x9C\x86\xE5\x9C\x88\xE7\x9A\x84\xE4\xB8\xAD\xE5\xBF\x83"; /* 请依次点击 4 个圆圈的中心 */
static const char TXT_OK[]      = "\xE6\xA0\xA1\xE5\x87\x86\xE5\xAE\x8C\xE6\x88\x90"; /* 校准完成 */
static const char TXT_RETRY[]   = "\xE6\xA0\xA1\xE5\x87\x86\xE5\xA4\xB1\xE8\xB4\xA5, \xE8\xAF\xB7\xE9\x87\x8D\xE6\x96\xB0\xE7\x82\xB9\xE5\x87\xBB"; /* 校准失败, 请重新点击 */
static const char TXT_SKIP[]    = "\xE6\x97\x8B\xE9\x92\xA5\xE8\xBD\xAC 12 \xE6\xA0\xBC\xE5\x8F\xAF\xE8\xB7\xB3\xE8\xBF\x87"; /* 旋钮转 12 格可跳过 */

/* 解 3x3 线性方程组(克莱姆法则), A 非奇异返回 true */
static bool solve3(const float A[3][3], const float b[3], float x[3])
{
    float det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1])
              - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0])
              + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (det < 1e-6f && det > -1e-6f) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        float M[3][3];
        memcpy(M, A, sizeof(M));
        for (int r = 0; r < 3; r++) {
            M[r][k] = b[r];
        }
        float dk = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
                 - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                 + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
        x[k] = dk / det;
    }
    return true;
}

/* 最小二乘仿射拟合 + 残差检查(任一点 >30px 视为失败) */
static bool cal_fit(const int16_t rx[4], const int16_t ry[4], float m[6])
{
    float Sxx = 0, Sxy = 0, Syy = 0, Sx = 0, Sy = 0;
    float Trxsx = 0, Trxsy = 0, Trysx = 0, Trysy = 0, Ssx = 0, Ssy = 0;
    for (int i = 0; i < 4; i++) {
        float x = (float)rx[i], y = (float)ry[i];
        Sxx += x * x;
        Sxy += x * y;
        Syy += y * y;
        Sx  += x;
        Sy  += y;
    }
    for (int i = 0; i < 4; i++) {
        float x = (float)rx[i], y = (float)ry[i];
        Trxsx += x * targets[i].x;
        Trxsy += x * targets[i].y;
        Trysx += y * targets[i].x;
        Trysy += y * targets[i].y;
        Ssx += targets[i].x;
        Ssy += targets[i].y;
    }
    const float A[3][3] = {
        { Sxx, Sxy, Sx  },
        { Sxy, Syy, Sy  },
        { Sx,  Sy,  4.0f },
    };
    float bx[3] = { Trxsx, Trysx, Ssx };
    float by[3] = { Trxsy, Trysy, Ssy };
    if (!solve3(A, bx, &m[0]) || !solve3(A, by, &m[3])) {
        return false;
    }
    /* 残差检查 */
    for (int i = 0; i < 4; i++) {
        float px = m[0] * rx[i] + m[1] * ry[i] + m[2];
        float py = m[3] * rx[i] + m[4] * ry[i] + m[5];
        float ex = px - targets[i].x;
        float ey = py - targets[i].y;
        if (ex * ex + ey * ey > 30.0f * 30.0f) {
            return false;
        }
    }
    return true;
}

static void tcal_dot_move(tcal_data_t *d)
{
    lv_obj_set_pos(d->dot, targets[d->target].x - 18, targets[d->target].y - 18);
}

static void tcal_timer_cb(lv_timer_t *t)
{
    page_t *p = (page_t *)lv_timer_get_user_data(t);
    pm_replace(PAGE_MENU);
    pm_shake();
    (void)p;
}

static void tcal_finish(page_t *p)
{
    tcal_data_t *d = p->data;
    float m[6];
    if (cal_fit(d->rx, d->ry, m)) {
        display_touch_cal_set(m);
        display_touch_cal_save(m);
        lv_label_set_text(d->label_tip, TXT_OK);
        d->done = true;
        d->timer = lv_timer_create(tcal_timer_cb, 900, p);
        lv_timer_set_repeat_count(d->timer, 1);
    } else {
        d->target = 0;
        tcal_dot_move(d);
        lv_label_set_text(d->label_tip, TXT_RETRY);
    }
}

static void pg_tcal_create(page_t *p)
{
    tcal_data_t *d = calloc(1, sizeof(tcal_data_t));
    p->data = d;
    p->title = TXT_TITLE;

    lv_obj_t *title = lv_label_create(p->root);
    lv_obj_set_style_text_color(title, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_msyh_16, 0);
    lv_label_set_text(title, TXT_TITLE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* 十字准星: 白圈 + 十字 */
    d->dot = lv_obj_create(p->root);
    lv_obj_remove_style_all(d->dot);
    lv_obj_set_size(d->dot, 36, 36);
    lv_obj_set_style_radius(d->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_color(d->dot, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_border_width(d->dot, 2, 0);
    lv_obj_set_style_bg_opa(d->dot, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(d->dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cross = lv_label_create(d->dot);
    lv_obj_set_style_text_color(cross, lv_color_hex(XK_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(cross, &lv_font_montserrat_26, 0);
    lv_label_set_text(cross, "+");
    lv_obj_center(cross);

    d->label_tip = lv_label_create(p->root);
    lv_obj_set_style_text_color(d->label_tip, lv_color_hex(XK_COLOR_GRAY), 0);
    lv_obj_set_style_text_font(d->label_tip, &lv_font_msyh_16, 0);
    lv_label_set_text(d->label_tip, TXT_TIP);
    lv_obj_align(d->label_tip, LV_ALIGN_BOTTOM_MID, 0, -40);

    lv_obj_t *skip = lv_label_create(p->root);
    lv_obj_set_style_text_color(skip, lv_color_hex(XK_COLOR_FAINT), 0);
    lv_obj_set_style_text_font(skip, &lv_font_msyh_16, 0);
    lv_label_set_text(skip, TXT_SKIP);
    lv_obj_align(skip, LV_ALIGN_BOTTOM_MID, 0, -14);

    tcal_dot_move(d);
    motor_set_mode(MOTOR_MODE_ON_OFF, 0, 0);   /* 校准期间关闭力反馈 */
}

static void pg_tcal_destroy(page_t *p)
{
    tcal_data_t *d = p->data;
    if (d) {
        if (d->timer) lv_timer_del(d->timer);
        free(d);
    }
    p->data = NULL;
}

static void pg_tcal_on_rotate(page_t *p, int32_t steps)
{
    tcal_data_t *d = p->data;
    d->skip_steps += steps > 0 ? steps : -steps;
    if (d->skip_steps >= 12 && !d->done) {
        pm_replace(PAGE_MENU);
        pm_shake();
    }
}

static void pg_tcal_on_tick(page_t *p)
{
    tcal_data_t *d = p->data;
    if (d->done) {
        return;
    }
    uint16_t z1, z2, rx, ry;
    bool touched = display_touch_get_raw(&z1, &z2, &rx, &ry);
    if (touched && !d->prev_touched && rx > 50 && rx < 4045 && ry > 50 && ry < 4045) {
        d->rx[d->target] = (int16_t)rx;
        d->ry[d->target] = (int16_t)ry;
        d->target++;
        if (d->target >= 4) {
            tcal_finish(p);
        } else {
            tcal_dot_move(d);
        }
    }
    d->prev_touched = touched;
}

static void pg_tcal_on_back(page_t *p)
{
    (void)p;   /* 校准中不可退出, 防止误触半途而废 */
}

const page_ops_t pg_tcal_ops = {
    .create = pg_tcal_create,
    .destroy = pg_tcal_destroy,
    .on_rotate = pg_tcal_on_rotate,
    .on_back = pg_tcal_on_back,
    .on_resume = NULL,
    .on_tick = pg_tcal_on_tick,
};
