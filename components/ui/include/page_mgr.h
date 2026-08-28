#pragma once

#include "lvgl.h"
#include <stdint.h>

typedef enum {
    PAGE_STARTUP = 0,
    PAGE_MENU,
    PAGE_PCDIAL,
    PAGE_PLAYGROUND,
    PAGE_HASS,
    PAGE_ENV,
    PAGE_SETTING,
    PAGE_SYSINFO,
    PAGE_FACTORY,
    PAGE_COUNT,
} page_id_t;

typedef struct page page_t;

/* 页面生命周期与输入回调。
 * 输入模型(见 docs/ARCHITECTURE.md §3):
 *   - 旋钮旋转 → on_rotate(steps): 浏览/调节 (steps>0 顺时针)
 *   - 触摸返回按钮 → on_back
 *   - 页面内控件触摸 → 页面自行注册 LVGL 事件 (进入/确认/切换)
 * 不再依赖"快旋确认/反快旋返回"手势(易误判, 见 BUG-003)。 */
typedef struct {
    void (*create)(page_t *p);
    void (*destroy)(page_t *p);
    void (*on_rotate)(page_t *p, int32_t steps);
    void (*on_back)(page_t *p);
    void (*on_tick)(page_t *p);
} page_ops_t;

typedef struct page {
    const page_ops_t *ops;
    lv_obj_t *root;        /* 页面根容器(由框架创建) */
    const char *title;     /* 顶部状态栏显示的标题 */
    void *data;            /* 页面私有数据 */
} page_t;

/* Page stack */
void pm_init(void);
void pm_push(page_id_t id);
void pm_replace(page_id_t id);
void pm_pop(void);
page_t *pm_top(void);
bool pm_busy(void);
void pm_shake(void);
int pm_depth(void);

/* Common X-Knob color scheme */
#define XK_COLOR_BG       0x000000
#define XK_COLOR_PANEL    0x141414
#define XK_COLOR_BORDER   0x262626
#define XK_COLOR_TEXT     0xFFFFFF
#define XK_COLOR_GRAY     0x999999
#define XK_COLOR_FAINT    0x555555
#define XK_COLOR_RED      0xFF0000
#define XK_COLOR_GREEN    0x00C800
#define XK_COLOR_BLUE     0x3399FF
#define XK_COLOR_ORANGE   0xFF9F0A