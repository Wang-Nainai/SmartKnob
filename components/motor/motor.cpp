#include "motor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_simplefoc.h"
#include "i2c_bus.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <string.h>

#if CONFIG_MOTOR_MT6701_INTERFACE_ABZ
#include "sensors/Encoder.h"
#endif

static const char *TAG = "motor";

/* ============================================================================
 * Phase 1 架构：单任务独占 BLDC 电机
 * ----------------------------------------------------------------------------
 * 规则：只有 motor_task 可以调用 BLDCMotor / BLDCDriver / Encoder /
 *       loopFOC() / move()。
 * 其它任务（LVGL/UI/MQTT/Web/SCD40）只能通过 motor_cmd_queue 投递命令，
 * 通过 volatile 快照读取状态。消除此前 motor_shake / motor_set_mode
 * 从 LVGL 线程直接操作 BLDCMotor 造成的竞态。
 * ==========================================================================*/

/* ---------------- 命令队列 ---------------- */

typedef enum {
    MOTOR_CMD_SET_MODE,        /* a1=mode  a2=num_positions(0=表默认)  a3=init_position */
    MOTOR_CMD_SET_MODE_RANGE,  /* a1=mode  a2=min_position  a3=max_position  a4=init_position */
    MOTOR_CMD_SET_POSITION,    /* a1=position */
    MOTOR_CMD_SHAKE,           /* a1=strength  a2=delay_ms */
    MOTOR_CMD_DISABLE,         /* 紧急停: move(0) 并暂停力反馈 */
} motor_cmd_type_t;

typedef struct {
    motor_cmd_type_t type;
    int32_t a1;
    int32_t a2;
    int32_t a3;
    int32_t a4;
} motor_cmd_t;

#define MOTOR_CMD_QUEUE_LEN 8
static QueueHandle_t s_cmd_queue = NULL;

/* ---------------- 硬件对象（仅 motor_task 访问） ---------------- */

#if CONFIG_MOTOR_MT6701_INTERFACE_ABZ
static Encoder sensor = Encoder(CONFIG_MOTOR_MT6701_ABZ_A_PIN, CONFIG_MOTOR_MT6701_ABZ_B_PIN,
                                CONFIG_MOTOR_MT6701_ABZ_PPR);
static void doA()
{
    sensor.handleA();
}
static void doB()
{
    sensor.handleB();
}
#elif CONFIG_MOTOR_MT6701_INTERFACE_SSI
static MT6701 sensor = MT6701(SPI3_HOST, (gpio_num_t)CONFIG_MOTOR_MT6701_SCL,
                              (gpio_num_t)CONFIG_MOTOR_MT6701_SDA, (gpio_num_t)-1,
                              (gpio_num_t)CONFIG_MOTOR_MT6701_CS);
#else
static MT6701 sensor = MT6701(I2C_NUM_1, (gpio_num_t)CONFIG_MOTOR_MT6701_SCL,
                              (gpio_num_t)CONFIG_MOTOR_MT6701_SDA);
#endif
static BLDCDriver3PWM driver = BLDCDriver3PWM(CONFIG_MOTOR_PWM_A, CONFIG_MOTOR_PWM_B,
                                              CONFIG_MOTOR_PWM_C, CONFIG_MOTOR_EN_PIN);
static BLDCMotor motor = BLDCMotor(CONFIG_MOTOR_POLE_PAIRS);

/* ---------------- 力反馈模式表（scottbez1 smartknob / X-Knob 移植） ---------------- */

typedef struct {
    int32_t position;
    int32_t min_position;
    int32_t max_position;
    float position_width_radians;
    float detent_strength_unit;
    float endstop_strength_unit;
    float snap_point;
    float snap_point_bias;
    uint32_t detent_positions_count;
    int32_t detent_positions[5];
    const char *text;
} knob_config_t;

static const knob_config_t knob_configs[MOTOR_MODE_COUNT] = {
    [MOTOR_MODE_UNBOUND_NO_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = -1,
        .position_width_radians = 10 * _PI / 180,
        .detent_strength_unit = 0, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "无边界和制动",
    },
    [MOTOR_MODE_BOUND_NO_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = 10,
        .position_width_radians = 10 * _PI / 180,
        .detent_strength_unit = 0, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "有边界无制动",
    },
    [MOTOR_MODE_MULTI_TURN_NO_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = 72,
        .position_width_radians = 10 * _PI / 180,
        .detent_strength_unit = 0, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "多圈无制动",
    },
    [MOTOR_MODE_ON_OFF] = {
        .position = 0,
        .min_position = 0, .max_position = 1,
        .position_width_radians = 60 * _PI / 180,
        .detent_strength_unit = 1, .endstop_strength_unit = 1,
        .snap_point = 0.55, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "开关模式",
    },
    [MOTOR_MODE_AUTO_RETURN_CENTER] = {
        .position = 0,
        .min_position = 0, .max_position = 0,
        .position_width_radians = 60 * _PI / 180,
        .detent_strength_unit = 0.01, .endstop_strength_unit = 0.6,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "自动回中",
    },
    [MOTOR_MODE_FINE_NO_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = 255,
        .position_width_radians = 1 * _PI / 180,
        .detent_strength_unit = 0, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "精细无制动",
    },
    [MOTOR_MODE_FINE_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = 255,
        .position_width_radians = 1 * _PI / 180,
        .detent_strength_unit = 1, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "精细有制动",
    },
    [MOTOR_MODE_COARSE_STRONG_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = 31,
        .position_width_radians = 8.225806452f * _PI / 180,
        .detent_strength_unit = 2, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "粗略强制动",
    },
    [MOTOR_MODE_COARSE_WEAK_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = 31,
        .position_width_radians = 8.225806452f * _PI / 180,
        .detent_strength_unit = 0.2, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "粗略弱制动",
    },
    [MOTOR_MODE_MAGNETIC_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = 31,
        .position_width_radians = 7 * _PI / 180,
        .detent_strength_unit = 2.5, .endstop_strength_unit = 1,
        .snap_point = 0.7, .snap_point_bias = 0,
        .detent_positions_count = 4, .detent_positions = {2, 10, 21, 22},
        .text = "磁性制动",
    },
    [MOTOR_MODE_RETURN_CENTER_WITH_DETENTS] = {
        .position = 0,
        .min_position = -6, .max_position = 6,
        .position_width_radians = 60 * _PI / 180,
        .detent_strength_unit = 1, .endstop_strength_unit = 1,
        .snap_point = 0.55, .snap_point_bias = 0.4,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "回中带制动",
    },
    [MOTOR_MODE_UNBOUNDED_DETENTS] = {
        .position = 0,
        .min_position = 0, .max_position = -1,   /* 无边界: 列表浏览用, 两个方向都能无限转 */
        .position_width_radians = 8.225806452f * _PI / 180,
        .detent_strength_unit = 2, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "无边界棘轮",
    },
    [MOTOR_MODE_ADJUSTER] = {
        .position = 0,
        .min_position = 0, .max_position = 14,
        .position_width_radians = (360.0f / 14) * _PI / 180,  /* 一整圈正好 14 档 */
        .detent_strength_unit = 2, .endstop_strength_unit = 1,
        .snap_point = 1.1, .snap_point_bias = 0,
        .detent_positions_count = 0, .detent_positions = {0}, .text = "\xE8\xB0\x83\xE8\x8A\x82\xE6\xA8\xA1\xE5\xBC\x8F", /* 调节模式 */
    },
};

/* ---------------- 控制状态（仅 motor_task 写，跨任务只读快照） ---------------- */

static knob_config_t motor_config;            /* 当前生效配置, 仅 motor_task 访问 */
static motor_mode_t current_mode = MOTOR_MODE_COARSE_STRONG_DETENTS;
static float current_detent_center = 0;       /* 仅 motor_task */
static float angle_to_detent_center = 0;      /* 仅 motor_task */

/* 跨任务只读快照: 单字对齐访问, Xtensa 上原子, 由 motor_task 更新 */
static volatile int32_t snap_position = 0;
static volatile int32_t snap_mode = MOTOR_MODE_COARSE_STRONG_DETENTS;
static volatile float snap_angle_offset_deg = 0.0f;
static volatile bool snap_ready = false;
/* 模式/位置被外部重置时递增: input 组件据此静默重同步(防幽灵旋转) */
static volatile uint32_t snap_seq = 0;

/* 力反馈算法常量（scottbez1 smartknob） */
static const float DEAD_ZONE_DETENT_PERCENT = 0.2;
static const float DEAD_ZONE_RAD = 1 * _PI / 180;
static const float IDLE_VELOCITY_EWMA_ALPHA = 0.001;
static const float IDLE_VELOCITY_RAD_PER_SEC = 0.05;
static const uint32_t IDLE_CORRECTION_DELAY_MILLIS = 500;
static const float IDLE_CORRECTION_MAX_ANGLE_RAD = 5 * _PI / 180;
static const float IDLE_CORRECTION_RATE_ALPHA = 0.0005;

static float idle_check_velocity_ewma = 0;
static uint32_t last_idle_start = 0;

/* shake 状态机（仅 motor_task 访问） */
typedef enum {
    SHAKE_IDLE,
    SHAKE_POS,
    SHAKE_NEG,
} shake_state_t;
static shake_state_t shake_state = SHAKE_IDLE;
static int shake_remaining = 0;
static int shake_strength = 0;
static int shake_delay = 0;
static bool shake_active = false;   /* 供命令去重: 正在抖 或 队列已有 SHAKE */

/* 急停开关: motor_disable() 后暂停力反馈与抖动, 直到下一次模式切换 */
static bool motor_control_enabled = true;

static void (*position_cb)(int32_t position, void *ctx) = NULL;
static void *position_cb_ctx = NULL;
static portMUX_TYPE s_cb_mux = portMUX_INITIALIZER_UNLOCKED;

static float CLAMP(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

/* ---------------- 跨任务只读接口 ---------------- */

bool motor_is_ready(void)
{
    return snap_ready;
}

int32_t motor_get_position(void)
{
    return snap_position;
}

motor_mode_t motor_get_mode(void)
{
    return (motor_mode_t)snap_mode;
}

float motor_get_angle_offset_deg(void)
{
    return snap_angle_offset_deg;
}

uint32_t motor_get_mode_seq(void)
{
    return snap_seq;
}

int motor_get_mode_count(void)
{
    return MOTOR_MODE_COUNT;
}

const char *motor_mode_name(int mode)
{
    if (mode < 0 || mode >= MOTOR_MODE_COUNT) {
        return "";
    }
    return knob_configs[mode].text;
}

/* ---------------- 命令投递（任意任务可调用, 非阻塞） ---------------- */

static void post_cmd(motor_cmd_t cmd)
{
    if (!s_cmd_queue) {
        return;
    }
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "motor cmd queue full, dropping type=%d", cmd.type);
    }
}

/* 发布位置变化（仅 motor_task 调用） */
static void publish_position(int32_t pos)
{
    void (*cb)(int32_t, void *) = NULL;
    void *ctx = NULL;
    portENTER_CRITICAL(&s_cb_mux);
    cb = position_cb;
    ctx = position_cb_ctx;
    portEXIT_CRITICAL(&s_cb_mux);
    if (cb) {
        cb(pos, ctx);
    }
}

/* ---------------- 命令执行（仅 motor_task 调用） ---------------- */

static void apply_mode_range(motor_mode_t mode, int32_t min_position, int32_t max_position,
                             int32_t init_position)
{
    knob_config_t cfg = knob_configs[mode];
    cfg.min_position = min_position;
    cfg.max_position = max_position;
    cfg.position = init_position;

    motor_config = cfg;
    current_mode = mode;
    current_detent_center = motor.shaft_angle;
    angle_to_detent_center = 0;

    motor_control_enabled = true;   /* 模式切换重新使能控制 */

    snap_mode = mode;
    snap_position = init_position;
    snap_angle_offset_deg = 0.0f;
    snap_seq = snap_seq + 1;

    ESP_LOGI(TAG, "mode=%d range=[%d..%d] pos=%d", mode, min_position, max_position, init_position);
}

static void apply_set_position(int32_t position)
{
    knob_config_t cfg = motor_config;
    int32_t num_positions = cfg.max_position - cfg.min_position + 1;
    if (num_positions > 0) {
        if (position < cfg.min_position) {
            position = cfg.min_position;
        }
        if (position > cfg.max_position) {
            position = cfg.max_position;
        }
    }
    cfg.position = position;
    motor_config = cfg;
    current_detent_center = motor.shaft_angle;
    angle_to_detent_center = 0;
    snap_position = position;
    snap_angle_offset_deg = 0.0f;
    snap_seq = snap_seq + 1;
    publish_position(position);
}

static void start_shake(int strength, int delay_ms)
{
    if (strength <= 0 || !motor_control_enabled) {
        return;
    }
    if (shake_active) {
        return;   /* 正在抖或已排队, 去重 */
    }
    /* 用户正在快转时不叠加振动, 避免对抗手感 */
    if (fabsf(motor.shaft_velocity) > 15.0f) {
        return;
    }
    motor.move((float)strength);   /* 先施加正脉冲 */
    shake_active = true;
    shake_state = SHAKE_POS;
    shake_remaining = delay_ms;
    shake_strength = strength;
    shake_delay = delay_ms;
    /* 抖动期间暂停怠速检测, 避免误判 */
    last_idle_start = 0;
    idle_check_velocity_ewma = 0;
}

static void process_command(const motor_cmd_t *cmd)
{
    switch (cmd->type) {
    case MOTOR_CMD_SET_MODE: {
        const knob_config_t *base = &knob_configs[cmd->a1];
        if (cmd->a2 > 0) {
            apply_mode_range((motor_mode_t)cmd->a1, base->min_position,
                             base->min_position + cmd->a2 - 1, cmd->a3);
        } else {
            apply_mode_range((motor_mode_t)cmd->a1, base->min_position, base->max_position,
                             cmd->a3);
        }
        break;
    }
    case MOTOR_CMD_SET_MODE_RANGE:
        apply_mode_range((motor_mode_t)cmd->a1, cmd->a2, cmd->a3, cmd->a4);
        break;
    case MOTOR_CMD_SET_POSITION:
        apply_set_position(cmd->a1);
        break;
    case MOTOR_CMD_SHAKE:
        start_shake(cmd->a1, cmd->a2);
        break;
    case MOTOR_CMD_DISABLE:
        motor_control_enabled = false;
        shake_state = SHAKE_IDLE;
        shake_active = false;
        motor.move(0);
        ESP_LOGW(TAG, "motor disabled");
        break;
    default:
        break;
    }
}

/* ---------------- 力反馈算法（仅 motor_task 调用） ---------------- */

static void haptic_update(void)
{
    idle_check_velocity_ewma = motor.shaft_velocity * IDLE_VELOCITY_EWMA_ALPHA
                               + idle_check_velocity_ewma * (1 - IDLE_VELOCITY_EWMA_ALPHA);
    if (fabsf(idle_check_velocity_ewma) > IDLE_VELOCITY_RAD_PER_SEC) {
        last_idle_start = 0;
    } else if (last_idle_start == 0) {
        last_idle_start = esp_timer_get_time() / 1000;
    }

    if (last_idle_start > 0
            && (esp_timer_get_time() / 1000) - last_idle_start > IDLE_CORRECTION_DELAY_MILLIS
            && fabsf(motor.shaft_angle - current_detent_center) < IDLE_CORRECTION_MAX_ANGLE_RAD) {
        current_detent_center = motor.shaft_angle * IDLE_CORRECTION_RATE_ALPHA
                                + current_detent_center * (1 - IDLE_CORRECTION_RATE_ALPHA);
    }

    knob_config_t cfg = motor_config;
    angle_to_detent_center = motor.shaft_angle - current_detent_center;

    float snap_point_radians = cfg.position_width_radians * cfg.snap_point;
    float bias_radians = cfg.position_width_radians * cfg.snap_point_bias;
    float snap_decrease = snap_point_radians
                          + (cfg.position <= 0 ? bias_radians : -bias_radians);
    float snap_increase = -snap_point_radians
                          + (cfg.position >= 0 ? -bias_radians : bias_radians);

    int32_t num_positions = cfg.max_position - cfg.min_position + 1;
    if (angle_to_detent_center > snap_decrease
            && (num_positions <= 0 || cfg.position > cfg.min_position)) {
        current_detent_center += cfg.position_width_radians;
        angle_to_detent_center -= cfg.position_width_radians;
        cfg.position--;
    } else if (angle_to_detent_center < snap_increase
               && (num_positions <= 0 || cfg.position < cfg.max_position)) {
        current_detent_center -= cfg.position_width_radians;
        angle_to_detent_center += cfg.position_width_radians;
        cfg.position++;
    }
    motor_config = cfg;

    float dead_zone_adjustment = CLAMP(
        angle_to_detent_center,
        fmaxf(-cfg.position_width_radians * DEAD_ZONE_DETENT_PERCENT, -DEAD_ZONE_RAD),
        fminf(cfg.position_width_radians * DEAD_ZONE_DETENT_PERCENT, DEAD_ZONE_RAD));

    bool out_of_bounds = num_positions > 0
                         && ((angle_to_detent_center > 0 && cfg.position == cfg.min_position)
                             || (angle_to_detent_center < 0 && cfg.position == cfg.max_position));
    motor.PID_velocity.limit = 10;
    motor.PID_velocity.P = out_of_bounds ? cfg.endstop_strength_unit * 4
                          : cfg.detent_strength_unit * 4;

    /* D factor scales with detent width (scottbez1 smartknob style) */
    const float derivative_lower_strength = cfg.detent_strength_unit * 0.08f;
    const float derivative_upper_strength = cfg.detent_strength_unit * 0.02f;
    const float derivative_position_width_lower = 3 * _PI / 180;
    const float derivative_position_width_upper = 8 * _PI / 180;
    const float raw = derivative_lower_strength
                      + (derivative_upper_strength - derivative_lower_strength)
                        / (derivative_position_width_upper - derivative_position_width_lower)
                        * (cfg.position_width_radians - derivative_position_width_lower);
    motor.PID_velocity.D = CLAMP(raw,
                                 fminf(derivative_lower_strength, derivative_upper_strength),
                                 fmaxf(derivative_lower_strength, derivative_upper_strength));

    if (fabsf(motor.shaft_velocity) > 60) {
        motor.move(0);
    } else {
        float input = -angle_to_detent_center + dead_zone_adjustment;
        if (!out_of_bounds && cfg.detent_positions_count > 0) {
            bool in_detent = false;
            for (uint32_t i = 0; i < cfg.detent_positions_count; i++) {
                if (cfg.detent_positions[i] == cfg.position) {
                    in_detent = true;
                    break;
                }
            }
            if (!in_detent) {
                input = 0;
            }
        }
        float torque = motor.PID_velocity(input);
        motor.move(torque);
    }

    /* 更新跨任务快照 + 回调 */
    snap_position = cfg.position;
    snap_angle_offset_deg = angle_to_detent_center * 180.0f / _PI;

    static int32_t last_published = 0;
    if (cfg.position != last_published) {
        last_published = cfg.position;
        publish_position(cfg.position);
    }
}

/* ---------------- FOC 校准持久化 (NVS) ----------------
 * MT6701 工作在 ABZ 增量模式(无 Z 索引): 编码器计数以上电瞬间转轴位置为零点,
 * 每次开机零参考都不同。zero_electric_angle 是"相对于本次开机计数零参考"的
 * 偏移, 跨开机保存必然错位 —— 曾导致部分开机换向错误、电机失控疯转。
 *
 * 因此: 只持久化 sensor_direction(接线方向, 硬件属性, 开机间恒定);
 * 零电角每次开机由 alignSensor() 重新锚定(保持 3pi/2 电角度 700ms 后测量)。
 * 开机时间 4.2s(全流程) -> 约 1.5s(跳过方向探测)。
 * 换电机/传感器后从设置页清除存档重学。 */
#define MOTOR_CAL_NS "mcal"
#define MOTOR_CAL_MAGIC 0x4B4D /* "MK" */
#define MOTOR_CAL_VER 2

typedef struct {
    uint16_t magic;
    uint8_t ver;
    int8_t dir; /* Direction::CW=+1 / CCW=-1 */
} motor_cal_t;

static bool motor_cal_load(int8_t *dir)
{
    nvs_handle_t h;
    if (nvs_open(MOTOR_CAL_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    motor_cal_t c;
    size_t sz = sizeof(c);
    bool ok = nvs_get_blob(h, "cal", &c, &sz) == ESP_OK && sz == sizeof(c);
    ok = ok && c.magic == MOTOR_CAL_MAGIC && c.ver == MOTOR_CAL_VER &&
         (c.dir == 1 || c.dir == -1);
    nvs_close(h);
    if (ok) {
        *dir = c.dir;
    }
    return ok;
}

static void motor_cal_save(int8_t dir)
{
    nvs_handle_t h;
    if (nvs_open(MOTOR_CAL_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    motor_cal_t c = { .magic = MOTOR_CAL_MAGIC, .ver = MOTOR_CAL_VER, .dir = dir };
    if (nvs_set_blob(h, "cal", &c, sizeof(c)) == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "FOC direction saved to NVS: %d", dir);
    }
    nvs_close(h);
}

static int8_t motor_cal_legacy_dir(void)
{
    /* 旧三键方案(zangle/dir/valid): 零电角因增量编码器不可跨开机复用, 只迁移方向 */
    nvs_handle_t h;
    if (nvs_open(MOTOR_CAL_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    int8_t dir = 0;
    uint8_t valid = 0;
    bool ok = nvs_get_u8(h, "valid", &valid) == ESP_OK && valid == 1 &&
              nvs_get_i8(h, "dir", &dir) == ESP_OK;
    nvs_close(h);
    return (ok && (dir == 1 || dir == -1)) ? dir : 0;
}

void motor_clear_calibration(void)
{
    nvs_handle_t h;
    if (nvs_open(MOTOR_CAL_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGW(TAG, "FOC calibration cleared, will recalibrate on next boot");
    }
}

/* ---------------- 电机主任务（唯一允许触碰 BLDC 硬件的任务） ---------------- */

static void motor_task(void *pvParameters)
{
    current_detent_center = motor.shaft_angle;

    while (1) {
        /* 1. 排空命令队列（非阻塞） */
        motor_cmd_t cmd;
        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            process_command(&cmd);
        }

        /* 2. FOC 电流环: 每周期都必须执行 */
        motor.loopFOC();

        /* 3. 抖动状态机 或 力反馈算法（互斥, 不并行） */
        if (!motor_control_enabled) {
            /* 急停状态: loopFOC 已执行, 但不施加任何目标力矩 */
        } else if (shake_state != SHAKE_IDLE) {
            if (shake_remaining > 0) {
                shake_remaining--;
            } else {
                if (shake_state == SHAKE_POS) {
                    shake_state = SHAKE_NEG;
                    shake_remaining = shake_delay;
                    motor.move(-shake_strength);
                } else {
                    shake_state = SHAKE_IDLE;
                    shake_active = false;
                    motor.move(0);
                }
            }
        } else {
            haptic_update();
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* ---------------- 公共 API ---------------- */

esp_err_t motor_init(void)
{
#if CONFIG_MOTOR_MT6701_INTERFACE_I2C
    gpio_set_direction((gpio_num_t)CONFIG_MOTOR_MT6701_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)CONFIG_MOTOR_MT6701_SDA, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)CONFIG_MOTOR_MT6701_SCL, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)CONFIG_MOTOR_MT6701_SCL, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(20));
    int sda_lvl = gpio_get_level((gpio_num_t)CONFIG_MOTOR_MT6701_SDA);
    int scl_lvl = gpio_get_level((gpio_num_t)CONFIG_MOTOR_MT6701_SCL);
    ESP_LOGI(TAG, "GPIO level check: SDA(GPIO%d)=%d  SCL(GPIO%d)=%d  (1=high/ok, 0=stuck low)",
             CONFIG_MOTOR_MT6701_SDA, sda_lvl, CONFIG_MOTOR_MT6701_SCL, scl_lvl);
    if (sda_lvl == 0 || scl_lvl == 0) {
        ESP_LOGE(TAG, "I2C line stuck low: check wiring (SDA/SCL shorted to GND, swapped, or module power off)");
        return ESP_ERR_INVALID_STATE;
    }
#endif

    sensor.init();

#if CONFIG_MOTOR_MT6701_INTERFACE_ABZ
    sensor.enableInterrupts(doA, doB);
    ESP_LOGI(TAG, "ABZ encoder: A=GPIO%d B=GPIO%d PPR=%d (quadrature CPR=%d)",
             CONFIG_MOTOR_MT6701_ABZ_A_PIN, CONFIG_MOTOR_MT6701_ABZ_B_PIN,
             CONFIG_MOTOR_MT6701_ABZ_PPR, (int)sensor.cpr);
#elif CONFIG_MOTOR_MT6701_INTERFACE_I2C
    i2c_bus_handle_t bus = sensor.get_i2c_bus();
    if (bus) {
        uint8_t found[16];
        uint8_t n = i2c_bus_scan(bus, found, sizeof(found));
        ESP_LOGI(TAG, "I2C1 scan: %d device(s) found", n);
        for (uint8_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  addr=0x%02X", found[i]);
        }
    } else {
        ESP_LOGE(TAG, "I2C bus handle unavailable");
    }
#endif

    motor.linkSensor(&sensor);

    for (int i = 0; i < 5; i++) {
        float a = sensor.getSensorAngle();
        ESP_LOGI(TAG, "sensor sample %d: angle=%.4f", i, a);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    driver.voltage_power_supply = CONFIG_MOTOR_VOLTAGE_SUPPLY;
    driver.init();
    motor.linkDriver(&driver);

    motor.foc_modulation = FOCModulationType::SpaceVectorPWM;
    motor.controller = MotionControlType::torque;

    motor.PID_velocity.P = 1;
    motor.PID_velocity.I = 0;
    motor.PID_velocity.D = 0.01;
    motor.voltage_limit = CONFIG_MOTOR_VOLTAGE_LIMIT;
    motor.LPF_velocity.Tf = 0.01;
    motor.velocity_limit = 10;

    if (!motor.init()) {
        ESP_LOGE(TAG, "motor init failed");
        return ESP_ERR_INVALID_STATE;
    }

    /* 校准持久化(见文件头说明): ABZ 为增量编码器, 零电角每次开机重新锚定,
     * 只复用 NVS 中的接线方向跳过方向探测。 */
    int8_t cal_dir = 0;
    if (!motor_cal_load(&cal_dir)) {
        int8_t legacy = motor_cal_legacy_dir();
        if (legacy != 0) {
            motor_cal_save(legacy);
            cal_dir = legacy;
            ESP_LOGI(TAG, "migrated legacy FOC calibration (dir=%d)", legacy);
        }
    }
    if (cal_dir != 0) {
        motor.sensor_direction = (Direction)cal_dir;
        if (!motor.initFOC()) {
            ESP_LOGE(TAG, "initFOC failed (saved dir=%d)", cal_dir);
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "initFOC: fast boot (dir=%d from NVS, zero re-anchored=%.3f rad)",
                 cal_dir, motor.zero_electric_angle);
    } else {
        if (!motor.initFOC()) {
            ESP_LOGE(TAG, "initFOC failed: sensor not responding, check MT6701 wiring");
            return ESP_ERR_INVALID_STATE;
        }
        /* 方向探测质量门控: 极对数校验失败说明探测过程有毛刺, 不可信, 不保存 */
        if ((motor.sensor_direction == Direction::CW ||
             motor.sensor_direction == Direction::CCW) &&
            motor.pp_check_result) {
            motor_cal_save((int8_t)motor.sensor_direction);
        } else {
            ESP_LOGW(TAG, "direction detection unreliable (pp_check=%d), not saved - will recalibrate next boot",
                     (int)motor.pp_check_result);
        }
    }

    motor_config = knob_configs[MOTOR_MODE_COARSE_STRONG_DETENTS];
    current_detent_center = motor.shaft_angle;
    snap_position = 0;
    snap_mode = MOTOR_MODE_COARSE_STRONG_DETENTS;
    snap_angle_offset_deg = 0.0f;
    snap_ready = true;
    ESP_LOGI(TAG, "motor ready, angle=%.2f", motor.shaft_angle);

    /* 命令队列必须在任务启动前创建 */
    s_cmd_queue = xQueueCreate(MOTOR_CMD_QUEUE_LEN, sizeof(motor_cmd_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "failed to create cmd queue");
        snap_ready = false;
        return ESP_ERR_NO_MEM;
    }

    xTaskCreatePinnedToCore(motor_task, "motor", 4096, NULL, 2, NULL, CONFIG_MOTOR_TASK_CORE);
    return ESP_OK;
}

void motor_set_mode(motor_mode_t mode, int32_t num_positions, int32_t init_position)
{
    if (mode < 0 || mode >= MOTOR_MODE_COUNT) {
        return;
    }
    motor_cmd_t cmd = { .type = MOTOR_CMD_SET_MODE, .a1 = mode, .a2 = num_positions,
                        .a3 = init_position, .a4 = 0 };
    post_cmd(cmd);
}

void motor_set_mode_range(motor_mode_t mode, int32_t min_position, int32_t max_position,
                          int32_t init_position)
{
    if (mode < 0 || mode >= MOTOR_MODE_COUNT) {
        return;
    }
    motor_cmd_t cmd = { .type = MOTOR_CMD_SET_MODE_RANGE, .a1 = mode, .a2 = min_position,
                        .a3 = max_position, .a4 = init_position };
    post_cmd(cmd);
}

void motor_set_position(int32_t position)
{
    motor_cmd_t cmd = { .type = MOTOR_CMD_SET_POSITION, .a1 = position, .a2 = 0,
                        .a3 = 0, .a4 = 0 };
    post_cmd(cmd);
}

void motor_set_position_cb(void (*cb)(int32_t position, void *ctx), void *ctx)
{
    /* 回调由 motor_task 调用; 注册通常在初始化期单次完成, 用临界区保护写 */
    portENTER_CRITICAL(&s_cb_mux);
    position_cb = cb;
    position_cb_ctx = ctx;
    portEXIT_CRITICAL(&s_cb_mux);
}

void motor_shake(int strength, int delay_ms)
{
    motor_cmd_t cmd = { .type = MOTOR_CMD_SHAKE, .a1 = strength, .a2 = delay_ms,
                        .a3 = 0, .a4 = 0 };
    post_cmd(cmd);
}

void motor_disable(void)
{
    motor_cmd_t cmd = { .type = MOTOR_CMD_DISABLE, .a1 = 0, .a2 = 0, .a3 = 0, .a4 = 0 };
    /* 紧急停是安全命令: 插队发送(队首), 不得排在积压的力反馈命令之后;
     * 队满时插队会挤掉最旧的一条普通命令, 可接受 */
    if (!s_cmd_queue ||
        xQueueSendToFront(s_cmd_queue, &cmd, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGW(TAG, "motor disable enqueue failed (queue full)");
    }
}