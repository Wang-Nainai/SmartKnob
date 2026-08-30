# SmartKnob 软件架构（目标）与阶段推进记录

本文档记录项目重构的**目标架构**、**已完成的阶段**与**验证状态**。
验证状态统一使用：
- `编译验证：通过/未通过`
- `静态分析：通过/未通过`
- `代码验证：通过`（指代码层面可确认的数据流/接口/竞态）
- `硬件实测：尚未验证`（无实体硬件时一律写此状态）

---

## 1. 目标架构

```
                     SmartKnob
                         │
        ┌────────────────┼────────────────┐
        │                │                │
      Motor            Input              UI
        │                │                │
      Haptic         Gesture/Event       LVGL
        │                │                │
        └──────────── Event / State ──────┘
                         │
            ┌────────────┼────────────┐
            │            │            │
          SCD40         MQTT         Web
                         │
                        HA
```

## 2. 核心原则

1. **Motor 单任务独占**（Phase 1 已完成）：
   只有 `motor_task` 可以调用 `BLDCMotor / BLDCDriver / Encoder / loopFOC / move`。
   其它模块只能：投递命令（`motor_cmd_queue`）+ 读状态快照（volatile）。
2. **输入与 UI 解耦**：
   旋钮原始位置 → `input` 组件 → 标准化事件（`knob_event_t`）→ 队列 → UI 消费。
   UI 不直接轮询 motor 位置做手势。
3. **UI 不直接操作电机**：
   UI 只能调 `motor_set_mode* / motor_shake / motor_disable`（均为命令投递）。
4. **实时性隔离**：
   WiFi/MQTT/SCD40/Web 断网或阻塞不得影响 Motor 与 UI。

## 3. 输入交互模型（设计决策）

硬件输入 = **旋钮 + 触摸屏**。

| 动作 | 事件 | 用途 |
|---|---|---|
| 旋转 | `KNOB_EVENT_ROTATE`（方向 + 步数） | 浏览焦点 / 调节数值 |
| 触摸点击 | LVGL 原生 tap 事件 | 确认 / 选择 / 进入 |
| 触摸滑动 | LVGL 原生 slide 事件 | 返回上一页 |

**设计决策（BUG-003）**：旧系统用"快旋 3 步/250ms = 确认、反快旋 = 返回"的手势。
该手势在无按键硬件上是"最后手段"，但存在明显误判风险（快速浏览被当成确认、回退误判为返回）。
由于本项目具备触摸屏，确认/返回应优先走触摸，旋转只负责浏览/调节，以最不易误判的方式操作。
快旋手势不作为导航依赖（可后续按需恢复，见 Phase 8）。

## 4. 阶段推进记录

| 阶段 | 内容 | 状态 | 验证 |
|---|---|---|---|
| Phase 0 | 基线（git 独立仓库 / sdkconfig.defaults / README / 现状审计） | ✅ 完成 | 编译通过；硬件实测未验证 |
| Phase 1 | Motor 命令队列 + 单任务独占 + 非阻塞 shake + disable | ✅ 完成 | 编译通过、静态通过；硬件实测未验证 |
| Phase 2 | Input 组件（旋钮 ROTATE 事件，去抖/去重/复位） | ✅ 完成 | 编译通过；硬件实测未验证 |
| Phase 3 | Application/Navigation（统一事件消费 + 触摸优先导航） | ✅ 完成 | 编译通过；硬件实测未验证 |
| Phase 4 | UI/LVGL 重构（状态栏/工厂测试页/视觉统一） | ✅ 完成 | 编译通过；硬件实测未验证 |
| Phase 5 | Sensor Manager（app_state 单一数据源） | ✅ 完成 | 编译通过；硬件实测未验证 |
| Phase 6 | 实时性隔离（scd40/input 钉 core0，motor 独占 core1） | ✅ 完成 | 编译通过；硬件实测未验证 |
| Phase 7 | 配置统一（亮度/熄屏 NVS 统一到 webcfg） | ✅ 完成 | 编译通过；硬件实测未验证 |
| Phase 8 | 扩展（LED 连接状态指示：WiFi 绿/MQTT 青/断红） | ✅ 完成 | 编译通过；硬件实测未验证 |

## 5. 跨模块数据流（现状）

```
motor_task(core1): Encoder → loopFOC → haptic_update → snap_position(volatile)
input_task(core0): 轮询 snap_position → knob_event_t → 队列
LVGL_task:         消费 knob_event_t + LVGL 触摸事件 → 页面导航
                   → motor_set_mode* / motor_shake（命令队列）
scd40_task(core0): I2C → app_state 快照 → UI 环境页 / MQTT publish / Web status
wifi:       STA + 重连 → 状态 → UI 状态栏 / webcfg / led(绿)
mqtt:       esp-mqtt 异步 → HA discovery + state + 控制命令 → led(青)
webcfg:     HTTP / OTA / NVS 配置
```

## 6. 任务与实时性隔离（现状）

| 任务 | 核 | 优先级 | 说明 |
|---|---|---|---|
| motor | core1 | 2 | 唯一操作 BLDCMotor；1ms 周期 |
| LVGL | 任意 | 2 | 渲染 + 页面 + 输入消费 |
| input | core0 | 1 | 轮询位置快照 → 事件队列 |
| scd40 | core0 | 3 | I2C 轮询 → app_state |
| wifi | core0 | 高 | 系统任务 |
| mqtt/httpd | 任意 | — | esp-mqtt / esp_http_server 系统任务 |

核心保障：**motor 独占 core1**，scd40/input/wifi 全部钉 core0，
任何网络/传感器活动都不会抢占电机控制循环。

## 7. 模块清单（现状）

```
components/
  motor/      电机控制 + 力反馈引擎(单任务独占)
  input/      旋钮输入 → ROTATE 事件
  app_state/  传感器/共享状态单一数据源
  blehid/     BLE HID 设备(NimBLE): 电脑音量/媒体/滚轮 (S-Dial)
  ui/         页面系统 + 状态栏 + 工厂测试
  display/    ST7789 + XPT2046 裸驱动(自研,含标定/诊断) + LVGL 移植
  scd40/      SCD40 驱动
  wifi/       WiFi STA
  mqtt_ha/    Home Assistant MQTT
  webcfg/     网页配置 + OTA + NVS
  led/        WS2812 状态灯
```

## 9. BLE HID 电脑控制（S-Dial，仿 X-Knob Surface Dial）

- 协议：标准 BLE HID over GATT（HID Service 0x1812 + Report Map +
  Consumer Control Report(ID1) + Mouse Report(ID2) + Battery），设备名 `SmartKnob`。
- 免驱：Windows/macOS 蓝牙设置直接配对，绑定持久化（NVS），断线自动重连广播。
- 能力：音量加减/静音/播放暂停/上一首/下一首（消费控制）、滚轮与相对移动（鼠标）。
- 页面交互（`pg_pcdial`）：旋转=音量/滚轮（页面内切换模式），
  点中心=播放暂停，底部按钮=上下曲；状态栏蓝牙图标显示连接状态。
- 启用成本：固件 +340KB（应用分区已扩至 3MB×3，余 37%）。

## 10. 工具链（tools/）

| 脚本 | 用途 |
|---|---|
| `build.ps1` | 一键构建（含完整 ESP-IDF 环境变量） |
| `gen_msyh_font.py` | 中文字库生成：自动扫描 UI 文案 → msyh.ttc 渲染 → LVGL 4bpp 字库。**新增文案后必须重跑** |
| `check_glyphs.py` | 校验 UI 用字是否全部被字库覆盖（缺失时报出码点与文件） |

## 11. 字库管线（lv_font_msyh_16）

- 格式：LVGL FMT_TXT / PLAIN 4bpp / SPARSE_TINY（unicode_list 升序，二分查找）
- 度量约定：`ofs_y = ascent - bbox_top - box_h`（与 LVGL
  `lv_draw_label.c` 定位公式核对一致），`adv_w` 单位 1/16 像素
- 字符集：自动 = UI 全部用字 + ASCII + ° · ；当前 215 字形

## 8. 触摸子系统（自研 XPT2046 驱动）

第三方 `atanisoft/esp_lcd_touch_xpt2046` 存在 XY 命令字标反的缺陷（BUG-009），
已替换为 `display.c` 内自研裸驱动：

- 数据手册标准命令：`0x90=X, 0xD0=Y, 0xB0=Z1, 0xC0=Z2`（12bit, DFR, PD=00）
- 压力判定：`z = z1 + 4095 - z2`，阈值 `CONFIG_TOUCH_Z_THRESHOLD`（默认 300）
- 标定：`TOUCH_X/Y_MIN/MAX` + `SWAP_XY/MIRROR_X/MIRROR_Y`（Kconfig）
- 诊断：开机 selftest 日志 + `display_touch_get_raw()` 供工厂测试实时显示
- 读数仅发生在 LVGL 任务的 indev 回调内，与刷屏同任务串行，无跨任务 SPI 竞争