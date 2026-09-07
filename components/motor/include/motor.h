#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOTOR_MODE_UNBOUND_NO_DETENTS = 0,        /* 无边界无制动 */
    MOTOR_MODE_BOUND_NO_DETENTS,              /* 有边界无制动 0-10 */
    MOTOR_MODE_MULTI_TURN_NO_DETENTS,         /* 多圈无制动 0-72 */
    MOTOR_MODE_ON_OFF,                        /* 开关模式 */
    MOTOR_MODE_AUTO_RETURN_CENTER,            /* 自动回中 */
    MOTOR_MODE_FINE_NO_DETENTS,               /* 精细无制动 0-255 */
    MOTOR_MODE_FINE_DETENTS,                  /* 精细有制动 0-255 */
    MOTOR_MODE_COARSE_STRONG_DETENTS,         /* 粗略强制动 0-31 */
    MOTOR_MODE_COARSE_WEAK_DETENTS,           /* 粗略弱制动 0-31 */
    MOTOR_MODE_MAGNETIC_DETENTS,              /* 磁性制动 */
    MOTOR_MODE_RETURN_CENTER_WITH_DETENTS,    /* 回中带制动 */
    MOTOR_MODE_UNBOUNDED_DETENTS,             /* 无边界棘轮(列表浏览: 无限转+档位感) */
    MOTOR_MODE_COUNT,
} motor_mode_t;

#define MOTOR_MODE_UNBOUND_FINE_DETENTS MOTOR_MODE_FINE_DETENTS
#define MOTOR_MODE_UNBOUND_COARSE_DETENTS MOTOR_MODE_COARSE_STRONG_DETENTS
#define MOTOR_MODE_SUPER_DIAL MOTOR_MODE_UNBOUND_NO_DETENTS

esp_err_t motor_init(void);
motor_mode_t motor_get_mode(void);
void motor_set_mode(motor_mode_t mode, int32_t num_positions, int32_t init_position);
void motor_set_mode_range(motor_mode_t mode, int32_t min_position, int32_t max_position,
                          int32_t init_position);
void motor_set_position(int32_t position);
int32_t motor_get_position(void);
bool motor_is_ready(void);
void motor_set_position_cb(void (*cb)(int32_t position, void *ctx), void *ctx);
void motor_shake(int strength, int delay_ms);
void motor_disable(void);
int motor_get_mode_count(void);
const char *motor_mode_name(int mode);
float motor_get_angle_offset_deg(void);
/* 模式/位置重置序列号: 每次模式切换或位置强制设置时递增,
 * 供 input 组件检测并静默重同步(防页面切换后的幽灵旋转) */
uint32_t motor_get_mode_seq(void);

#ifdef __cplusplus
}
#endif