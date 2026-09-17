# SmartKnob 软件架构

本文档以当前仓库中的实际代码、`sdkconfig`、Kconfig 和组件依赖为准，描述 SmartKnob 的真实软件架构、重要设计决策、当前实现状态和防回退约束。

文档状态标记：

- `代码验证`：可从当前代码的数据流、接口和生命周期直接确认。
- `静态分析`：通过源码、配置和构建产物确认，但未在实体设备上执行该场景。
- `编译验证`：工程能够完成构建。
- `硬件实测`：必须有实体硬件上的可复核结果；仓库当前没有完整硬件测试报告，因此不把代码注释中的历史“实测”自动扩大成当前版本已全量验证。

---

## 1. 项目定位

SmartKnob 是一台可联网的力反馈智能旋钮，当前软件提供四类能力：

1. 本地终端：LVGL UI、BLDC 力反馈、触摸交互、环境监测、设置和系统监控。
2. PC 控制器：通过 BLE HID 提供音量、媒体控制、鼠标滚轮和相对鼠标移动接口。
3. Home Assistant 控制器：使用 MQTT、HA MQTT Discovery、动作触发器和下行命令。
4. 浏览器管理台：提供 WiFi/MQTT 配置、状态、环境历史、OTA、重启和恢复出厂入口。

设备没有物理按键，交互基础是旋钮旋转、触摸、水平和垂直触摸手势，以及受页面白名单约束的快速逆时针甩动返回。

---

## 2. 硬件平台

### 2.1 当前代码与配置基线

| 部件 | 当前实际配置 |
|---|---|
| MCU | ESP32-S3 N16R8，16 MB Flash，8 MB PSRAM |
| CPU | 当前 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=160`，不是 240 MHz |
| 软件栈 | ESP-IDF 5.5.5，LVGL 9.2.0，esp_simplefoc 1.4.1 |
| LCD | ST7789，240 x 320，竖屏，RGB565 |
| LCD SPI | SPI2，80 MHz，双 DMA 缓冲；每缓冲 240 x 30 像素 |
| 触摸 | XPT2046，与 LCD 共用 SPI2，片选 GPIO14，SPI 1 MHz |
| 电机 | 2804 BLDC，7 极对，SimpleFOC 3PWM，供电 12 V，电压限幅 5 V |
| 编码器 | MT6701 ABZ，A=GPIO7，B=GPIO4，PPR=1024，硬件四倍频 CPR=4096 |
| 环境传感器 | SCD40，I2C0，100 kHz，SDA=GPIO5，SCL=GPIO6 |
| 状态灯 | WS2812，RMT，GPIO48 |

LCD 引脚：SCLK=GPIO12，MOSI=GPIO11，MISO=GPIO13，DC=GPIO9，RST=GPIO8，CS=GPIO10，背光=GPIO21。

### 2.2 触摸坐标与校准

- 自研 XPT2046 裸驱动采用该面板实际接线约定的命令字：`0xD0` 读横轴 X，`0x90` 读纵轴 Y。
- 当前 `sdkconfig` 为 `TOUCH_SWAP_XY=n`、`TOUCH_MIRROR_X=y`、`TOUCH_MIRROR_Y=y`，用于未经过四点校准时的基础映射。
- 触摸校准由 `pg_tcal` 完成，以 `x' = a*rx + b*ry + c`、`y' = d*rx + e*ry + f` 做最小二乘仿射拟合，4 点残差超过 30 px 则重来。
- 校准矩阵以 blob `m6` 存储在 NVS namespace `touchcal`。
- 开机若存在有效触摸校准，直接加载；否则启动页进入四点校准向导。
- 触摸轮询发生在 LVGL indev 回调中，与 LVGL 刷屏处于同一任务，避免跨任务并发访问同一 SPI 总线。
- 触摸异常时通过 z1/z2 总线无效判定、原始坐标 50..4045 窗口、双读一致性和自适应静息压力基线抑制幽灵触摸。

---

## 3. 软件分层

```
main
  ├─ app_state       环境/WiFi/AP 共享只读快照
  ├─ display         ST7789 + XPT2046 + LVGL port
  ├─ ui              page_mgr + pages + status bar + font
  ├─ input           motor snapshot -> knob_event_t
  ├─ motor           BLDCMotor 唯一所有者 + haptic command queue
  ├─ scd40           CO2/温度/湿度采集
  ├─ env_hist        PSRAM 双环形历史缓冲
  ├─ wifi            STA 无限重连 + SoftAP 配网回退
  ├─ blehid          NimBLE BLE HID
  ├─ mqtt_ha         MQTT / HA Discovery / 下行命令
  ├─ webcfg          HTTP + NVS 配置 + OTA
  └─ led             WS2812 状态灯
```

组件依赖尽量单向。例如 `wifi` 直接读取 NVS `webcfg`，避免 `wifi -> webcfg` 循环依赖；HTTP 服务由 `main` 启动。

---

## 4. 总体数据流

```
MT6701 ABZ
   ↓
motor_task(core1)
   ↓ volatile position/mode/seq snapshot
input_task(core0)
   ↓ knob_event_t + event queue
LVGL task(core0)
   ↓ page_mgr -> 当前 page
页面 UI / motor command queue / MQTT / BLE / display APIs
```

环境链路：

```
SCD40(I2C0)
   ↓ main.c scd40_task(core0)
app_state 快照  +  MQTT 遥测  +  env_hist PSRAM 环形缓冲
   ↓             ↓                  ↓
UI 环境页       Home Assistant      设备趋势图 / Web 趋势图
```

所有跨任务 Motor 控制都通过 Motor 命令队列 `s_cmd_queue`，所有跨任务 Motor 状态读取都通过 volatile 快照和序列号。

### 4.1 当前启动顺序

```
nvs_flash_init
  ↓
app_state_init
  ↓
led_init
  ↓
display_init
  ↓
motor_init
  ↓
smartknob_ui_init
  ↓
knob_input_init
  ↓
scd40_init + periodic measurement + scd40_task
  ↓
wifi_init
  ↓
blehid_init
  ↓
coex preference + SNTP
  ↓
webcfg_start
  ↓
wifi_wait_connected / SoftAP fallback
  ↓
mqtt_ha_init
  ↓
主循环 heap 水位监控
```

WiFi 驱动必须先于 BLE 初始化，避免 BLE 先占用内部 DMA 内存导致 `esp_wifi_init()` 的 `NO_MEM` 崩溃。HTTP server 由 `app_main` 启动，不放在 WiFi 事件回调中。

---

## 5. 核心设计原则

1. Motor 所有权唯一：只有 `motor_task` 可以直接访问 `BLDCMotor`、`BLDCDriver`、Encoder、`loopFOC()` 和 `move()`。
2. 外部模块只能投递 Motor 命令和读取状态快照，禁止直接修改 `motor.mode`、`motor.target` 或调用 `motor.loopFOC()` / `motor.move()`。
3. Input 不直接读硬件编码器，而是消费 Motor 发布的位置快照。
4. UI 不直接访问 Motor 底层对象；交互页面通过 `motor_set_mode*`、`motor_set_position`、`motor_shake`、`motor_disable` 投递命令。
5. page_mgr 是唯一页面栈和生命周期管理者。
6. 页面切换、模式重置和位置强制设置必须同步 Input 基准，不能靠简单清零掩盖幽灵旋转。
7. 高频路径避免阻塞式网络、长 delay、大块分配和大日志。
8. 网络任务不得阻塞 Motor FOC；UI 任务不得执行耗时网络同步操作。

---

## 6. Motor 架构

### 6.1 所有权

`components/motor/motor.cpp` 同时拥有硬件对象和当前 haptic 配置。

唯一允许触碰 SimpleFOC 对象的任务：

```text
motor_task
```

唯一任务内部操作：

```text
motor.loopFOC()
motor.move()
motor.init()
motor.initFOC()
motor.PID_velocity.*
```

其他组件只能通过 `include/motor.h` 中的 API 投递命令或读取快照。

### 6.2 Command Queue

命令队列：

```text
static QueueHandle_t s_cmd_queue
长度：8
```

命令类型：

```text
MOTOR_CMD_SET_MODE
MOTOR_CMD_SET_MODE_RANGE
MOTOR_CMD_SET_POSITION
MOTOR_CMD_SHAKE
MOTOR_CMD_DISABLE
```

公共 API：

```text
motor_set_mode
motor_set_mode_range
motor_set_position
motor_shake
motor_disable
```

命令入队由任意任务调用。`post_cmd()` 最终执行 `xQueueSend(..., pdMS_TO_TICKS(20))`，队列满时最多等待 20 ms 后记录 warning 并丢命令。因此它是有界等待，不是严格意义上的零等待非阻塞 API；当前 20 ms 是需要在实时性审计中关注的边界。

Motor 对外状态：

```text
motor_get_position
motor_get_mode
motor_get_angle_offset_deg
motor_is_ready
motor_get_mode_seq
```

`motor_get_mode_seq()` 在模式切换或位置强制设置时递增，是 Input 防幽灵旋转的核心同步信号。

### 6.3 任务与实时性

当前 `motor_task`：

| 项目 | 当前值 |
|---|---|
| Core | `CONFIG_MOTOR_TASK_CORE=1` |
| 优先级 | 2 |
| 栈 | 4096 字节 |
| 调度周期 | 约 1 ms，`vTaskDelay(pdMS_TO_TICKS(1))` |
| 每周期顺序 | 排空命令队列 -> `loopFOC()` -> disable/shake/haptic |

`app_main`、LVGL、Input、SCD40 均固定在 core0；NimBLE host 和 WiFi task 也配置在 core0。Motor 独占 core1 是当前实时隔离策略。

### 6.4 当前 13 种手感模式

`motor_mode_t` 当前有 13 个模式：

| # | 枚举 | 当前显示/用途 |
|---|---|---|
| 1 | `MOTOR_MODE_UNBOUND_NO_DETENTS` | 无边界无制动 |
| 2 | `MOTOR_MODE_BOUND_NO_DETENTS` | 有边界无制动，范围 0..10 |
| 3 | `MOTOR_MODE_MULTI_TURN_NO_DETENTS` | 多圈无制动 0..72 |
| 4 | `MOTOR_MODE_ON_OFF` | 开关模式 0..1（HASS 灯开关两档） |
| 5 | `MOTOR_MODE_AUTO_RETURN_CENTER` | 自动回中 |
| 6 | `MOTOR_MODE_FINE_NO_DETENTS` | 精细无制动，约 1 度/档，范围 0..255 |
| 7 | `MOTOR_MODE_FINE_DETENTS` | 精细有制动，约 1 度/档，范围 0..255 |
| 8 | `MOTOR_MODE_COARSE_STRONG_DETENTS` | 粗略强制动，约 8.2258 度/档，范围 0..31 |
| 9 | `MOTOR_MODE_COARSE_WEAK_DETENTS` | 粗略弱制动，约 8.2258 度/档，范围 0..31 |
| 10 | `MOTOR_MODE_MAGNETIC_DETENTS` | 磁性制动，范围 0..31 |
| 11 | `MOTOR_MODE_RETURN_CENTER_WITH_DETENTS` | 回中带制动，范围 -6..6 |
| 12 | `MOTOR_MODE_UNBOUNDED_DETENTS` | 无边界棘轮，列表浏览，约 8.2258 度/档 |
| 13 | `MOTOR_MODE_ADJUSTER` | 调节模式，约 25.714 度/档（360/14，一整圈 14 档），HASS 空调温度/风速用 |

磁性制动模式的 4 个特殊吸附点当前为 `{2, 10, 21, 22}`。

`pg_menu` 的菜单副标题已与 `motor_get_mode_count()` 一致（13）。

### 6.5 保护与手感算法

| 保护/算法 | 当前实际值 |
|---|---|
| 失控速度保护 | `abs(motor.shaft_velocity) > 60 rad/s` 时 `motor.move(0)` |
| 死区 | `DEAD_ZONE_DETENT_PERCENT=0.2`，同时受 1 度上限约束 |
| 有界端点 | 出界时 `PID_velocity.P = endstop_strength_unit * 4` |
| 怠速回中 | EWMA alpha=0.001，速度阈值 0.05 rad/s，500 ms 后开始，最大 5 度，中心修正 alpha=0.0005 |
| Shake | 非阻塞状态机 `SHAKE_POS -> SHAKE_NEG -> SHAKE_IDLE` |
| Shake 去重 | 正在抖动时忽略新请求 |
| Shake 高速抑制 | 速度大于 15 rad/s 时忽略 Shake，避免对抗快转 |
| Disable | 置 `motor_control_enabled=false`，清 Shake 并 `move(0)` |
| Disable 退出 | 下一次模式切换会重新 `motor_control_enabled=true` |

Shake 不是 `delay()`，也不会在 UI 线程直接操作 BLDCMotor。

### 6.6 FOC 校准与 NVS

MT6701 工作在 ABZ 增量模式（无 Z 索引引脚），编码器计数以上电瞬间转轴位置为零点，每次开机零参考都不同。`zero_electric_angle` 是相对于本次开机计数零参考的偏移（`electricalAngle() = normalize(dir * pole_pairs * getMechanicalAngle() - zero_electric_angle)`），**跨开机保存必然错位**，曾导致部分开机换向错误、电机失控疯转（BUG-021）。

Motor 校准 namespace：

```text
mcal
```

键：

```text
cal  blob {magic u16=0x4B4D, ver u8=2, dir i8}  单条目原子写入
```

只持久化接线方向 `sensor_direction`（硬件属性，开机间恒定）。旧三键方案（`zangle`/`dir`/`valid`）已废弃，开机时自动把旧 `dir` 迁移进 `cal` blob。

启动行为：

1. `mcal/cal` 存在且有效：预置 `sensor_direction` 后执行 `initFOC()`，跳过方向探测（约 2 秒），零电角由 `alignSensor()` 重新锚定。开机时间约 1.5 秒。
2. 无有效存档：执行 `initFOC()` 全流程校准。方向探测的极对数校验（`pp_check_result`）失败时不保存，下次开机重试全流程。
3. 设置页“重新校准”双击确认后调用 `motor_clear_calibration()`，擦除整个 `mcal` namespace，随后重启执行全流程校准。

禁止恢复“预置 NVS 中的 zero_electric_angle 跳过零电角测量”的做法；除非改用带 Z 索引或绝对式接口（I2C），否则零电角必须每次开机重新锚定。

不得用全局 `nvs_flash_erase()` 代替 Motor 校准清理。

---

## 7. Input 架构

### 7.1 数据流

```
MT6701 ABZ
   ↓
motor_task 内部 Encoder
   ↓ motor snapshot
input_task
   ↓
knob_event_t
   ↓ FreeRTOS queue
LVGL task -> page_mgr -> 当前页面 on_rotate(steps)
```

Input 使用 `motor_get_position()` 和 `motor_get_mode_seq()`，不直接访问编码器。

### 7.2 当前参数

| 项目 | 当前值 |
|---|---|
| 轮询周期 | `CONFIG_KNOB_INPUT_POLL_MS=5` |
| 事件队列长度 | `CONFIG_KNOB_INPUT_QUEUE_LEN=16` |
| 事件类型 | `KNOB_EVENT_ROTATE` |
| `steps` | 相对上次采样位置差；正值为顺时针，负值为逆时针 |
| 时间戳 | `esp_timer_get_time()` 的毫秒值 |

一次采样中跨越多个档位会作为一个含多步的 `steps` 事件发送。

### 7.3 状态同步与防幽灵旋转

Input 使用 seqlock 式三次读取：

```text
seq1 = motor_get_mode_seq()
pos  = motor_get_position()
seq2 = motor_get_mode_seq()
```

若 `seq1 != seq2` 或 `seq1 != last_seq`：

1. 重新读取位置和序列号。
2. 清空输入事件队列。
3. 不向上层发送 delta。

页面切换时 page_mgr 调用 `knob_input_reset()`；Motor 在模式切换或位置强制设置时递增 `snap_seq`。两套机制共同防止：

- 页面切换后旧位置差泄漏到新页面。
- 进入页面后突然跳格。
- 动画期间残留输入被新页面消费。

禁止用未经分析的 `steps = 0` / `delta = 0` 补丁掩盖状态同步问题。

---

## 8. UI 架构

### 8.1 page_mgr

页面栈最大深度 8，页面由 `page_ops_t` 描述：

```text
create
destroy
on_rotate
on_back
on_tick
on_resume
flick_block
```

导航 API：

```text
pm_push
pm_replace
pm_pop
pm_top
pm_busy
pm_depth
```

进入页面时 page_mgr 创建根对象并调用 `create`；退出时调用 `destroy`，然后删除根 LVGL 对象。状态栏返回按钮只在页面栈深度大于 1 时显示。

### 8.2 页面生命周期

```
页面 create
  ↓
操作 / 子页面 push
  ↓
子页面 pop 动画结束
  ↓
恢复栈顶页面
  ↓
调用 on_resume
  ↓
重新声明页面所需 Motor mode/range
  ↓
knob_input_reset()
```

当前实现 `on_resume` 的页面包括：

```text
pg_menu
pg_playground
pg_hass
pg_env
pg_setting
pg_factory
```

页面不能只在 `create` 时声明 Motor 状态；从子页面返回后必须通过 `on_resume` 恢复，否则会残留子页面的手感模式。

### 8.3 甩动返回与 flick_block

全局快速逆时针甩动返回参数：

```text
窗口：350 ms
累计：逆时针 >= 10 steps
```

当前只允许以下页面对 `on_back` 触发甩动返回：

```text
PAGE_ENV
PAGE_HASS
PAGE_SETTING
PAGE_SYSINFO
PAGE_FACTORY
PAGE_APCFG
```

`flick_block` 当前用于：

- `pg_hass` 控制视图：旋转代表设备输入（灯=开/关两档，空调=温度/风速调节）。
- `pg_setting` 编辑视图：旋转代表亮度或熄屏时长输入。

其他页面即使有 `on_back`，也不在甩动返回白名单内。触摸确认、点击和状态栏返回按钮仍是主要导航手段。

### 8.4 触摸手势

- 普通页面左右横滑会执行 `on_back`。
- `pg_pcdial` 横滑映射音量，上下滑映射鼠标滚轮。
- `pg_tcal` 禁用普通手势，触摸只用于采样校准点。
- 手势通过 LVGL `LV_EVENT_GESTURE` 产生，并调用 `lv_indev_reset()` 取消当前按压，避免手势再触发 `CLICKED`。

### 8.5 状态栏

状态栏高度 22 px，位于 top layer，包含：

```text
返回按钮
页面标题
WiFi
Bluetooth
MQTT
时间
```

当前颜色语义：

| 元素 | 当前实现 |
|---|---|
| 时间 | 有效时间显示 `HH:MM`，未同步时显示 `--:--` |
| WiFi | 已连接白色；未连接灰色 |
| MQTT | 已连接白色；客户端已配置但未连接红色；未配置灰色 |
| Bluetooth | 已连接蓝色；未连接灰色 |

当前 WiFi 图标没有区分“未配置”和“已配置但连接失败”，两者都显示灰色。只有 MQTT 使用了红/灰区分。

### 8.6 页面列表

| 页面 | 当前功能 |
|---|---|
| `pg_startup` | SmartKnob 启动动画；2 秒后根据触摸校准状态进入 `pg_tcal` 或 `pg_menu` |
| `pg_menu` | 6 项三副本无限循环菜单；触摸滑动和旋钮切换焦点，点击进入 |
| `pg_pcdial` | BLE HID 电脑控制；单击模式键循环 切歌(默认)/音量/滚轮/表盘 四种模式，中央短按=播放暂停、表盘模式长按=弹 Windows 系统圆盘菜单，上一首/下一首恒可用；触摸手势横滑=音量、竖滑=滚轮；配对状态显示 |
| `pg_playground` | 13 种 Motor 手感试玩；点击切换模式，Apple 风格圆盘（有界=量程窗内填充弧+端点红弧，无界=圆点绕全周） |
| `pg_hass` | 真实设备循环列表：卧室灯/客厅灯/过道灯（开/关两档，旋转点击同效）+ 卧室空调（点图标圆底切温度/风速模式，旋转=每度一档/风速四象限）；控制视图为 Apple 风格表盘（温度=比例填充弧，开/关与风速=圆点定位） |
| `pg_env` | CO2、温湿度、等级卡和 2 小时趋势图；旋转或点击切换 CO2/温度/湿度 |
| `pg_setting` | 亮度、熄屏时长、系统监控、蓝牙清除配对、Motor 重新校准 |
| `pg_sysinfo` | 版本、IP、MQTT、运行时长、熄屏配置、构建时间、Web 地址和工厂测试入口 |
| `pg_apcfg` | SoftAP 配网页；显示热点名、访问 URL 和二维码 |
| `pg_tcal` | 四点触摸校准；旋钮累计 12 格可跳过 |
| `pg_factory` | 触摸、LED、Motor、编码器、SCD40、WiFi/MQTT 的现场测试入口 |

`pg_factory` 不是恢复出厂页面；Web 恢复出厂走 HTTP `/factory_reset`。

---

## 9. LVGL Memory Strategy

### 9.1 当前生效状态（CUR-001 已修复，2026-09）

LVGL 使用 libc malloc，对象分配经 `malloc()` 落入 ESP32-S3 的 8 MB PSRAM
（`CONFIG_SPIRAM_USE_MALLOC=y` + `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`
使大于阈值(默认 16KB→0)的分配优先 PSRAM），不再受 64 KB builtin pool 限制。

当前生效配置链（代码验证）：

```text
sdkconfig:                CONFIG_LV_USE_CLIB_MALLOC=y
  ↓ choice（三选一，无 builtin）
build/config/sdkconfig.h: 只有 CONFIG_LV_USE_CLIB_MALLOC 1
                          （CONFIG_LV_USE_BUILTIN_MALLOC 与
                           LV_MEM_SIZE_KILOBYTES 已被 regen 清理）
  ↓
src/lv_conf_kconfig.h:    CLIB 分支 → CONFIG_LV_USE_STDLIB_MALLOC = LV_STDLIB_CLIB
```

历史冲突说明：旧版本曾用 `main/Kconfig.projbuild` 补 int 符号
`LV_USE_STDLIB_MALLOC=1`，但 `lv_conf_kconfig.h` 按 builtin 分支覆盖，
实际始终 builtin 64 KB（CUR-001）。现已删除 int 补丁符号，直接使用
组件 Kconfig 的 malloc choice；该文件仅保留历史注释。

### 9.2 运行时验证状态

- 编译验证：redefined 警告消失，bin 尺寸变化符合 builtin 池移除预期。
- 运行验证（硬件待确认）：页面叠加场景不再出现 64 KB 池 OOM 死循环；
  `LV_USE_MEM_MONITOR`（builtin 专用）当前无意义，勿依据它判断。

### 9.3 禁止回退

- 不恢复 LVGL builtin 64 KB pool 作为正式内存策略（勿在 menuconfig 里
  把 malloc choice 切回 "LVGL's built in implementation"）。
- 不删除 heap poisoning 来掩盖 heap corruption。
- 不把任何 int 型 `LV_USE_STDLIB_MALLOC` 补丁符号加回 Kconfig —— choice
  才是唯一生效开关（CUR-001 教训）。

当前 `CONFIG_HEAP_POISONING_COMPREHENSIVE=y` 是长期启用的内存踩踏检测措施，不是临时调试开关。出现 heap corruption 时必须分析生命周期、重复释放、越界写、callback、PSRAM 和 LVGL 异步对象，不得直接关闭 poisoning。

---

## 10. LVGL 对象生命周期

`display` 组件自行创建 LVGL task，当前参数：

| 项目 | 当前值 |
|---|---|
| Core | 0 |
| 优先级 | 8 |
| 栈 | 6144 字节 |
| 最小调度延迟 | 5 ms |
| 最大调度延迟 | 500 ms |
| Tick | 2 ms |
| 渲染模式 | partial render，双 DMA buffer |

### 10.1 ui_label_roll

`ui_label_roll()` 采用幽灵标签复用：

1. 第一次变化时创建宿主标签的子标签，并记录在宿主 user data。
2. 文本变化时，幽灵标签复用旧文本并淡出。
3. 幽灵标签在动画结束后继续隐藏复用，不删除。
4. 主标签设置新文本，新旧文本在固定宽度容器内重合。

禁止把 `ui_label_roll` 改回 create -> animate -> delete 的循环。历史实测表明，在动画 ready 回调中删除仍有动画关联的 LVGL 对象可能导致动画链表破坏和堆损坏。

### 10.2 页面删除（CUR-003 已彻底修复）

`page_mgr` 的 pop 删除路径已完全异步化：

```text
pop 动画 ready 回调（pm_pop_anim_done）
  ↓ 仅摘出栈顶引用、结束 pm_animating
lv_async_call(pm_pop_finish_cb, p)
  ↓ 下一拍执行（动画链表已完全处理完）
pm_delete_page（先删对象树 → 后释放页面数据，已核对 12 个页面 destroy
  均不访问 p->root）
  ↓
恢复新栈顶页面 on_resume + knob_input_reset
```

防回退要点：
- pop 删除路径的 async 化不可回退 —— 在动画 ready 回调中直接删除对象与
  `ui_label_roll` 幽灵标签的历史堆损坏教训同源（LVGL 动画链表破坏）。
- 页面回调以 `LV_EVENT_ALL` 注册时必须拦截 `LV_EVENT_DELETE`（删除树广播）。
- `pm_replace`（startup→menu/tcal）保留同步删除，该场景无用户交互。

---

## 11. 字体系统

当前字库：

```text
lv_font_msyh_16
字体源：Microsoft YaHei 16 px
格式：LVGL FMT_TXT / PLAIN 4bpp / SPARSE_TINY
当前字形数：294
line_height：22
base_line：5
```

生成流程：

```
UI / Motor 源码文案
  ↓
tools/gen_msyh_font.py 自动收集字符
  ↓
Pillow + msyh.ttc 生成 4bpp 位图
  ↓
components/ui/fonts/lv_font_msyh_16.c
  ↓
tools/check_glyphs.py 校验
```

`gen_msyh_font.py` 当前扫描：

```text
components/ui/pages/*.c
components/ui/smartknob_ui.c
components/motor/motor.cpp
```

`check_glyphs.py` 当前只扫描：

```text
components/ui/pages/*.c
components/ui/smartknob_ui.c
```

因此生成器包含 Motor 模式文案，但覆盖率检查没有覆盖 `motor.cpp`。这是当前工具链缺口，不应误认为校验脚本覆盖了全部动态文案。

修改中文或特殊字符后必须：

1. 修改源码文案。
2. 运行 `python tools/gen_msyh_font.py`。
3. 运行 `python tools/check_glyphs.py`。
4. 重新编译并检查缺字。

当前校验结果：源码已使用 CJK 字符 175 个，当前字库覆盖 294 字形，检查通过。

---

## 12. 环境传感器与历史数据

### 12.1 SCD40

SCD40 初始化参数：

```text
I2C0
SDA=GPIO5
SCL=GPIO6
100 kHz
设备地址 0x62
```

`main.c` 创建 `scd40_task`：

```text
Core 0
优先级 3
栈 4096
轮询周期 CONFIG_SCD40_POLL_INTERVAL_MS=2000
```

任务流程：

```
data_ready
  ↓
read measurement
  ↓
app_state_set_env
  ↓
mqtt_ha_publish
  ↓
env_hist_push_if_due
```

SCD40 测量周期通常为约 5 秒；软件轮询为 2 秒，只有 `data_ready` 为真时读取。

### 12.2 app_state

`app_state` 是环境数据和 WiFi/AP 状态的跨任务只读快照：

- SCD40 任务写入 `co2_ppm`、`temperature_c`、`humidity_pct`、`has_data`。
- UI、MQTT、Web 和工厂测试读取环境快照。
- WiFi 状态和 SoftAP SSID/IP 也由该组件保存，AP 字符串字段由临界区保护。

### 12.3 env_hist

历史缓冲首次使用时优先从 PSRAM 分配；PSRAM 不可用时回退普通 calloc。

| 缓冲 | 当前容量 | 当前采样间隔 | 用途 |
|---|---|---|---|
| 高分辨率 | 120 点 | 60 秒 | 设备 `pg_env` 2 小时趋势图 |
| 日级缓冲 | 1440 点 | 60 秒 | Web `/api/envhist` 24 小时趋势图 |

实现特性：

- 两个环形缓冲区同时记录 CO2、温度 x10、湿度 x10。
- 不持久化到 Flash，重启清零。
- `seq` 累加值用于 UI 判断是否有新样本。
- 日级缓冲第一次满 60 秒时，会先用现有 2 小时缓冲回填，避免 Web 曲线长期为空。

当前已知冲突：`env_hist.h` 和 `env_hist.c` 实际按 60 秒写入日级缓冲，但 Web `/api/envhist` 返回 `"iv_s":300`，前台图画布也按 5 分钟计算时间轴。因此浏览器时间轴和实际采样间隔不一致。本次只记录问题，不修改代码。

当前历史数据没有保存每个样本的绝对时间戳，只保存采样序号和时间间隔；Web 图表用当前时间和固定 interval 反推横轴。

---

## 13. WiFi

### 13.1 STA

- 配置优先级：NVS `webcfg/wifi_ssid`、`webcfg/wifi_pass`，缺失时使用 Kconfig 默认值。
- `WIFI_EVENT_STA_DISCONNECTED` 后无限重连。
- 当次开机已经连接过 WiFi 后再断线，只维持后台重连，不开启 SoftAP。
- 连接成功强制 `WIFI_PS_NONE`，避免入站 TCP 被省电策略丢包。
- WiFi task 配置在 core0。

### 13.2 SoftAP 配网回退

当前配置：

```text
WIFI_AP_FALLBACK_ENABLE=y
WIFI_AP_FALLBACK_TIMEOUT_SEC=90
WIFI_AP_PASSWORD 默认空
```

流程：

```
STA 启动
  ↓
等待 90 秒
  ├─ 已连接 -> 正常运行
  └─ 未连接且本次开机从未连过
       ↓
     APSTA + SmartKnob-XXXX
       ↓
     Web 管理页
       ↓
     保存新 WiFi 配置
       ↓
     STA 连上后关闭 SoftAP
```

SoftAP 默认访问地址是 `192.168.4.1`。`pg_apcfg` 可以手动关闭提示页，但热点会保留到 STA 配网成功。

AP 启动前会检查 internal largest free block；小于 24 KB 时拒绝启动热点，避免闭源 WiFi 库内存分配失败崩溃。

APSTA 切换窗口（BUG-033）：`s_ap_switching` 置位期间，事件任务的
DISCONNECTED 回调**不执行** `esp_wifi_connect()`，避免重连与
`set_mode(APSTA)` 在闭源库内并发 panic；切换完成（或任一回滚路径）清标志
并显式恢复重连。该门控不可删除。

---

## 14. BLE HID

### 14.1 实现

使用 NimBLE，设备名：

```text
SmartKnob
```

当前服务：

```text
HID Service 0x1812
Battery Service 0x180F
Device Information Service 0x180A
PnP ID 0x2A50（PID=0x4005）
```

HID Report Map 包含：

```text
Report ID 1：Consumer Control（7 个用法逐位独立字段，16 位位图）
Report ID 2：Mouse，3 按键 + dx/dy + wheel
Report ID 10：Surface Dial（Rudimentary Dial 0x0E TLC，
              Button1 + Touch 双位 + Dial 15-bit 相对角度）
```

当前功能 API：

```text
blehid_consumer_send
blehid_dial_rotate
blehid_dial_button
blehid_mouse_scroll
blehid_mouse_move
blehid_disconnect
blehid_unpair_all
```

### 14.2 报文协议（关键结论，防回退）

由 X-Knob / esp32-surface-dial / superdial 三个 Windows 实测可用项目反向验证：
**Windows BLE HID 栈按各 input characteristic 的 Report Reference 匹配报告，
通知内容全部是报告数据 —— 不带 Report ID 前缀**。

曾把报文首字节写成 Report ID（如 `{10, ...}`），造成三类实测故障：

- dial 报文：前缀 0x10 使旋转字段错位/越界 → 旋转无反应或单方向
- consumer 报文：前缀 0x01 的 bit0 恰好 = Scan Next 用法 → 上一首/下一首都变下一首
- dial button 报文：前缀使 Dial 字段被污染成大角度 → 点击被 Windows 当成一次滚动

任何 HID 报文改动前必须先重读本节。报文格式：

```text
consumer(2B)  {mask_lo, mask_hi}
mouse(4B)     {buttons, dx, dy, wheel}
dial(3B)      {btn_byte, rot_lo, rot_hi7}
btn_byte      bit0=Button1(按下), bit1=Touch(恒 1, 仿真 Surface Dial 触摸态)
rot           15-bit 有符号补码, 单位 0.1 度 (unit exponent -1)
```

- 每格电机档位映射 ±100（=10 度/档），与 Espressif 官方 usb_surface_dial、
  X-Knob 的每档值一致；Windows 圆盘 UI 每档响应一次。
- 旋转/按压语义由 Windows 系统处理（短按/长按/工具切换），不经 Consumer 页。
- PnP ID PID 用 0x4005：Report Map 变更（Button+Touch 布局）后必须让 Windows
  把设备当新 HID 实例重新读 map —— Windows 按旧 map 缓存解析新报文会产生
  位错乱（旋转单方向等），改 PID 强制重新枚举。

### 14.3 配对与协议

- 使用 Just Works、bonding、Secure Connections，不要求 MITM。
- 绑定数据由 NimBLE 的 NVS 持久化能力保存。
- 断线后重新广播；REPEAT_PAIRING 删旧绑定后重试，防配对风暴。
- Protocol Mode 支持 Boot/Report；Boot 模式下 Consumer/Dial 静默，Mouse Boot 报告 3 字节。
- Windows 所需的 PnP ID 已实现，广播 appearance 使用 Generic HID 0x03C2。
- 连接后请求 30..45 ms connection interval 和 latency 2，降低与 WiFi 共存时的空口争用。
- `blehid_consumer_send` 内部 press→release 间隔 8ms；旋转发送采用 30ms 合批窗口
  + pending 累计不丢失（页面侧实现），表盘模式一次报告携带全部积压并分段限幅。

清除配对使用 `blehid_unpair_all()`，只清除 BLE 绑定，不影响 Motor 校准、触摸校准、WiFi 或 MQTT 配置。

S-Dial 页面（pg_pcdial）的 Windows 实测回归（旋转方向、上一首/下一首、
播放暂停、长按系统菜单、重配对生效）仍需实体电脑验证；仓库无完整实机报告。

---

## 15. MQTT / Home Assistant

### 15.1 当前启用状态

当前 `sdkconfig` 中：

```text
CONFIG_MQTT_HA_ENABLE=y
```

代码支持关闭 MQTT；关闭时相关 API 为空实现。

### 15.2 传感器发现

设备在 MQTT 连接后发布 3 个 HA sensor discovery：

```text
<client_id>_co2
<client_id>_temp
<client_id>_humidity
```

状态 topic：

```text
homeassistant/sensor/<client_id>/state
```

状态载荷：

```json
{"co2": 800, "temp": 24.5, "rh": 48.0}
```

### 15.3 设备动作触发器

设备发布 8 个 `device_automation` discovery（真实设备槽位），组合为：

```text
3 盏灯：bedroom_light / living_light / hall_light，动作 on / off
卧室空调：bedroom_ac，动作 on / off
```

动作 topic：

```text
smartknob/action
```

载荷示例：

```text
bedroom_light_on
bedroom_ac_off
```

触发器发现配置每 400 ms 发布一条，发送完成后停止定时器。重新连接时会重新发布；连接时还会对旧版占位触发器（light/ac/fan/washer × on/off/left/right）发空保留载荷让 HA 清除。

### 15.4 下行命令

设备订阅：

```text
smartknob/cmnd/#
```

当前支持：

| Topic | 载荷 | 行为 |
|---|---|---|
| `smartknob/cmnd/shake` | 可忽略 | `motor_shake(3, 40)` |
| `smartknob/cmnd/mode` | 整数模式编号 | 投递 `motor_set_mode`；非法编号由 Motor API 拦截 |
| `smartknob/cmnd/led` | 6 位 RRGGBB | 设置 WS2812 |

### 15.5 页面控制通道

`pg_hass` 控制视图发布两类消息：

```text
smartknob/action                          payload=<dev>_<act>，边沿事件（on/off）
smartknob/level/<key>                     整数绝对值（旋转类调节结算后的终值）
<mqtt_topic>/HOME/<设备中文名>            payload=ON/OFF（X-Knob 风格兼容通道）
```

level 通道当前键：

```text
bedroom_ac_temp  16..30（空调温度）
bedroom_ac_fan   0..3（自动/低/中/高）
```

结算发布规则：空调旋转调节时 UI 实时跟随，MQTT 在静止 250 ms 后只发布一次终值——惯性滑过若干档不会触发多次 HA 动作；切换温度/风速模式或退出控制视图前会强制 flush 未结算值。灯的开/关走 action 通道（边沿触发，电机两档位置即状态）。

`mqtt_topic` 默认是 `knob`，可通过 NVS `webcfg/mqtt_topic` 覆盖；当前 Web 保存表单没有提供该字段。

### 15.6 幂等和线程安全

- `mqtt_ha_init()` 以 `s_client` 判定是否已初始化，重复调用不会重复创建 client。
- `mqtt_ha_reinit()` 在 mutex 下停止并销毁旧 client，再用 NVS 配置创建新 client。
- publish 路径也用同一 mutex 避免 client 生命周期和发送并发。
- MQTT task stack 设置为 4096，reconnect timeout 30 秒，network timeout 30 秒。

---

## 16. WebCfg

HTTP server 绑定 `0.0.0.0`，监听端口 80，STA 和 SoftAP 模式都可访问。HTTP task 栈 8192，发送和接收等待时间均放宽到 20 秒。

当前实际 URL：

| URL | 方法 | 功能 |
|---|---|---|
| `/` | GET | 管理台 HTML |
| `/status` | GET | 状态 JSON |
| `/api/envhist` | GET | 24 小时历史，chunked JSON |
| `/save` | POST | 保存 WiFi/MQTT 并重连 |
| `/api/set` | POST | 电机、显示、LED、BLE HID 控制 |
| `/ota` | POST | 上传固件到下一个 OTA 分区 |
| `/restart` | POST | 重启 |
| `/factory_reset` | POST | 清除 `webcfg` namespace 并重启 |

旧基线中提到的 `/api/config` 和 `/api/factory` 当前不存在。

### 16.1 `/api/set`

当前 action：

```text
motor_mode
shake
estop
brightness
timeout
led
ble_disc
hid
```

`hid` 的 value：

```text
vol_up
vol_dn
mute
play
next
prev
scr_up
scr_dn
```

### 16.2 环境历史

`/api/envhist` 使用 `httpd_resp_send_chunk()` 分段发送：

```text
iv_s
n
co2[]
temp[]   x10
rh[]     x10
```

当前 `iv_s` 返回 300，但实际日级缓冲采样间隔是 60 秒，见第 12.3 节。

### 16.3 OTA

- 目标为 `esp_ota_get_next_update_partition(NULL)`。
- 最大接收大小 0x300000，即 3 MB。
- 写入完成后调用 `esp_ota_set_boot_partition()`。
- 成功后约 1 秒重启。
- 分区表包含 `otadata` 和 3 个 3 MB 应用分区。

### 16.4 恢复出厂

当前 Web 恢复出厂行为是：

```text
webcfg_erase_all()
esp_restart()
```

它删除整个 NVS namespace `webcfg`，包括 WiFi、MQTT、亮度、熄屏和可能存在的 `mqtt_topic` 配置。

它不会删除：

```text
mcal        Motor 校准
touchcal    触摸校准
BLE bonds   NimBLE 配对
```

不得把当前 Web 恢复出厂描述成 `nvs_flash_erase()`。

---

## 17. NVS 设计

| Namespace | Key | 类型 | 用途 | Web 恢复出厂删除 |
|---|---|---|---|---|
| `mcal` | `cal` | blob {magic, ver, dir} | FOC 接线方向存档 | 否 |
| `touchcal` | `m6` | blob/6 floats | 四点触摸仿射矩阵 | 否 |
| `webcfg` | `wifi_ssid` | str | STA SSID | 是 |
| `webcfg` | `wifi_pass` | str | STA 密码 | 是 |
| `webcfg` | `mqtt_uri` | str | Broker URI | 是 |
| `webcfg` | `mqtt_user` | str | MQTT 用户名 | 是 |
| `webcfg` | `mqtt_pass` | str | MQTT 密码 | 是 |
| `webcfg` | `mqtt_topic` | str | HA 控制 topic 前缀；代码读取，Web 保存表单当前不写 | 是 |
| `webcfg` | `brightness` | i32 | 屏幕亮度 10..100 | 是 |
| `webcfg` | `timeout` | i32 | 熄屏分钟 0..30，0=常亮 | 是 |
| NimBLE 管理 | 由协议栈管理 | - | BLE pairing/bond | 否，使用 `blehid_unpair_all()` |

NVS 全盘擦除只有启动时 `ESP_ERR_NVS_NO_FREE_PAGES` / `ESP_ERR_NVS_NEW_VERSION_FOUND` 恢复路径，以及首次烧录建议的 `erase-flash`。业务功能不得随意调用 `nvs_flash_erase()`。

---

## 18. 任务、Core 与实时性

| 任务/执行体 | Core | 优先级 | 栈 | 说明 |
|---|---:|---:|---:|---|
| `app_main` | 0 | 系统默认 | 3584 | 初始化和启动编排 |
| `motor` | 1 | 2 | 4096 | 唯一访问 BLDCMotor/FOC |
| `LVGL` | 0 | 8 | 6144 | LVGL timer、渲染、页面、Input 消费 |
| `knob_input` | 0 | 1 | 2048 | 5 ms 轮询 Motor 快照 |
| `scd40` | 0 | 3 | 4096 | SCD40 数据读取 |
| NimBLE host | 0 | 协议栈默认 | 4096 | `CONFIG_BT_NIMBLE_PINNED_TO_CORE=0` |
| WiFi task | 0 | 协议栈默认 | 协议栈配置 | `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y` |
| MQTT task | 未显式绑定 | 协议栈默认 | 4096 | 项目代码未设置 core affinity |
| HTTPD task | 未显式绑定 | 协议栈默认 | 8192 | 项目代码未设置 core affinity |

实时性规则：

- Motor loop 不被 UI、网络、传感器或日志长阻塞。
- LVGL 任务不执行同步网络请求、大块文件 IO 或长 `delay`。
- MQTT callback、BLE callback 和实时任务不进行大 JSON 拼接或长时间等待。
- LED RMT 发送使用 mutex 串行化，避免 WiFi、MQTT、UI、HTTP 并发调用。

当前已知阻塞点：`pg_setting` 的 Motor 重新校准双击确认路径在 LVGL 回调中执行 `vTaskDelay(600 ms)` 后重启。该等待违反“UI 回调不做长阻塞”的约束，但本次文档同步不修改代码。

---

## 19. 系统监控（已移除）

`pg_sysmon` 页面与 `sysmon` 组件已于 2026-09 移除（BUG-042/BUG-038 两次
实机崩溃后评估）：该功能对最终用户几乎无价值，而其核心 API
`uxTaskGetSystemState` 挂起双核调度并逐任务扫描栈水印，是全系统最重的
调用，先后引发 IWDT panic 与 spinlock count assert 两个实机重启问题。

替代的诊断手段：
- 主循环每 10 秒打印 heap 水位（free/min/largest/internal）。
- 工厂测试页的编码器/SCD40/网络现场诊断。
- 需要任务级分析时，用 `idf.py monitor` 临时排查（或从 git 历史恢复本组件）。

---

## 20. 分区与工具链

当前分区：

```text
nvs      0x6000
otadata  0x2000
phy_init 0x1000
factory  0x300000
ota_0    0x300000
ota_1    0x300000
```

工具：

| 工具 | 用途 |
|---|---|
| `tools/build.ps1` | 固定 ESP-IDF v5.5.5 环境并执行 `idf.py` |
| `tools/gen_msyh_font.py` | 扫描 UI/Motor 文案并生成 4bpp 中文字库 |
| `tools/check_glyphs.py` | 校验 UI 页面和主 UI 文件的中文字符覆盖 |

构建命令：

```powershell
powershell -File tools\build.ps1 build
```

不得擅自替换项目默认构建流程。

`sdkconfig` 和 `sdkconfig.defaults` 必须保持 UTF-8。不要在 PowerShell 中用 `Set-Content` 批量改写 `sdkconfig`，以免破坏编码和配置语义。

---

## 21. 已知限制与硬件待验证

当前仓库可从代码和静态分析确认，但不能自动等同于硬件验证：

| 项目 | 当前状态 |
|---|---|
| 首次烧录和分区 | 分区表已含 otadata，首次变更建议完整擦除；待实机确认 |
| 触摸 IC/接线 | 代码有 selftest、原始值诊断和无触摸模块提示；待实机确认 |
| 触摸方向 | 有四点校准和 Kconfig mirror/swap；待实机确认 |
| BLE 配对 | 代码实现 NimBLE HID、绑定持久化、重连；待实机确认 |
| S-Dial 音量/滚轮/媒体 | 页面和 HID 报文已实现；待实机确认 |
| 菜单动画 | 代码实现三副本循环和宽度动画；待实机确认 |
| Motor 闭环/力反馈 | FOC、12 模式和校准持久化已实现；待实机确认 |
| WiFi/AP 回退 | 代码实现 90 秒回退和管理页；待实机确认 |
| MQTT/HA Discovery | 3 个 sensor + 8 个 device_automation + level 通道已实现；待实机确认 |
| Web/OTA | HTTP API、chunked history 和 OTA 分区已实现；待实机确认 |
| LED | 代码实现状态灯和互斥保护；待实机确认 |
| 长时间稳定性 | 无当前版本连续运行报告；待实机确认 |

---

## 22. 防回退设计决策

1. UI、MQTT、BLE、Web、Input 不得直接操作 BLDCMotor。
2. Shake 必须保持非阻塞状态机。
3. `motor_disable` 必须持续抑制 haptic，直到下一次模式切换重新使能。
4. 页面切换必须同时重置 Input 队列基准和 Motor mode sequence 同步。
5. `page pop` 后必须调用栈顶页面的 `on_resume`。
6. `lv_scale` 不应吞掉页面级点击，需要按其实际交互显式移除 `CLICKABLE`。
7. 浮层应独立于 flex scroll content，避免布局流把浮层放到错误位置。
8. LVGL 动画回调不应删除仍有关联动画的对象；`ui_label_roll` 必须复用幽灵标签。
9. LVGL 不恢复 builtin 64 KB allocator 作为正式方案。
10. 字库与源码文案必须同步生成和校验。
11. 触摸总线异常不能产生持续幽灵点击。
12. 不关闭 comprehensive heap poisoning 来掩盖内存破坏。
13. 不把 `CONFIG_LV_USE_STDLIB_MALLOC=1` 单独当作 CLIB 已生效的证明；必须检查最终 Kconfig 映射和构建配置。
14. 不把没有绝对时间戳的 env_hist 描述成已完成真实时间戳历史。
15. 不把 Web 恢复出厂描述成全 NVS 擦除。
16. 不把代码级实现描述成当前硬件已完整验证。
17. 表盘指示全局唯一规则：连续值用填充弧，离散位置用圆点，两者互斥不得叠加；弧更新一律用 `lv_arc_set_angles` 直接设角，禁止用 `lv_arc_set_value`（内建值动画与定时器连发冲突，见 BUG-022/023）。
18. `motor_get_angle_offset_deg()` 是档内偏角（恒可能非 0），不是越界距离；越界判定必须同时看位置是否停在边界档与偏移方向是否朝界外（见 BUG-022）。
19. 旋转类调节（温度/风速）走 level 通道结算发布（静止 250 ms 发一次绝对值），禁止按档连发 ±1 事件（惯性滑档会造成 HA 连跳，见 BUG-021 之后的结算发布改造）。
20. BLE HID 报文一律为纯数据，不得带 Report ID 前缀 —— Windows BLE HID 按 characteristic 的 Report Reference 匹配，前缀会污染数据位（旋转单方向/上一首变下一首/点击变滚动，见 BUG-024）。改 HID 协议前必须重读 §14.2。
21. 以 `LV_EVENT_ALL` 注册的页面回调必须在入口拦截 `LV_EVENT_DELETE`（页面销毁广播，此时 p->data 已释放）；`pm_delete_page` 的固定顺序是先删对象树后释放数据，DELETE 期间数据有效但也不得执行业务操作（见 BUG-025）。
22. "电机档位即值"的页面（HASS 空调、设置编辑等）读 `motor_get_position()` 后必须按该模式量程钳制 —— 端点制动拉回前位置会短暂越界，直接采用会写非法值（见 BUG-026）。
23. esp_timer 回调内访问可被其它任务销毁的共享资源（如 MQTT client）必须 try-take 锁，拿不到即跳过本轮，不得阻塞等待（见 BUG-027）。
24. 安全命令（急停等）不得排在命令队列 FIFO 尾部，应插队发送（见 BUG-027）。
25. 传感器"60 秒自愈"计数必须同时覆盖正常未就绪路径与总线/CRC 错误路径（见 BUG-028）。

---

## 23. 当前文档与代码冲突摘要

1. 实际 CPU 配置是 160 MHz，旧基线写 240 MHz。
2. LVGL malloc choice 已切 CLIB，对象分配经 libc 落 PSRAM（CUR-001 已修复，
   编译层验证生效；运行时页面叠加场景待烧录确认）。
3. Web `/api/envhist` 已改为 60 秒间隔（CUR-002 已修复，iv_s=60 与日级缓冲一致）。
4. page_mgr 的 pop 删除路径已异步化（CUR-003 已彻底修复：动画 ready 回调摘出，
   `lv_async_call` 下一拍删页，见 §10.2）。
5. `pg_setting` 重新校准路径的 600 ms 阻塞已改为非阻塞定时器重启（CUR-004 已修复）。
6. `README.md` 已重写为开源版本（CUR-006 已解决）；`docs/` 两份文档与代码同步。
7. BLE HID Surface Dial 的 Windows 实测回归已有阶段性实机反馈：旋转、上一首/
   下一首、切歌模式已验证可用；播放暂停与长按系统菜单需继续实测。
8. 已记录不修的低危项见 docs/BUGS.md BUG-037（env_hist 单写多读、display 熄屏
   双任务竞态——均有自愈或影响有限）。
