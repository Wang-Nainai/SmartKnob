#pragma once

#include "lvgl.h"

typedef enum {
    PAGE_STARTUP = 0,
    PAGE_MENU,
    PAGE_PLAYGROUND,
    PAGE_HASS,
    PAGE_ENV,
    PAGE_SETTING,
    PAGE_SYSINFO,
    PAGE_COUNT,
} page_id_t;

typedef struct page page_t;

typedef struct {
    void (*create)(page_t *p);
    void (*destroy)(page_t *p);
    void (*on_rotate)(page_t *p, int dir);
    void (*on_confirm)(page_t *p);
    void (*on_back)(page_t *p);
    void (*on_tick)(page_t *p);
} page_ops_t;

typedef struct page {
    const page_ops_t *ops;
    lv_obj_t *root;
    void *data;
} page_t;

/* Page stack */
void pm_init(void);
void pm_push(page_id_t id);
void pm_replace(page_id_t id);
void pm_pop(void);
page_t *pm_top(void);
bool pm_busy(void);
void pm_shake(void);

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