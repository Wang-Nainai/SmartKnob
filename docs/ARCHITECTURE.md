# SmartKnob 软件架构

本文档记录项目**目标架构**、**阶段推进记录**与**验证状态**。
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
   WiFi/MQTT/SCD40/Web/BLE 断网或阻塞不得影响 Motor 与 UI。

## 3. 输入交互模型（设计决策）

硬件输入 = **旋钮 + 触摸屏**。

| 动作 | 事件 | 用途 |
|---|---|---|
| 旋转 | `KNOB_EVENT_ROTATE`（方向 + 步数） | 浏览焦点 / 调节数值 |
| 触摸点击 | LVGL 原生 tap 事件 | 确认 / 选择 / 进入 |
| 状态栏返回键 | `on_back` | 返回上一页 |

**设计决策（BUG-003）**：旧系统用"快旋 3 步/250ms = 确认、反快旋 = 返回"手势，
误判风险高。本项目具备触摸屏，确认/返回走触摸，旋转只负责浏览/调节。

## 4. 页面结构（当前）

```
开机 → 启动动画页(2s) → 主菜单(X-Knob 聚焦展开列表)
  ├─ S-Dial 电脑控制   BLE HID: 旋转=音量/滚轮, 点击=播放暂停, 上下曲
  ├─ 手感体验          11 种力反馈模式, 点击切换, 表盘+越界红弧
  ├─ 智能家居          设备列表 → 控制视图(旋转=LEFT/RIGHT, 点击=ON/OFF)
  ├─ 环境              CO2 仪表 + 等级 + 温湿度
  ├─ 设置              亮度/熄屏时长(旋转调节, 点击保存, NVS)
  └─ 系统              固件/网络信息 → 工厂测试页
SoftAP 配网页          WiFi 超时自动弹出(二维码+热点名+IP)
```

状态栏：返回键(子页显示) + 页面标题 + WiFi/BLE/MQTT 图标 + 时间。

## 5. 阶段推进记录

| 阶段 | 内容 | 状态 |
|---|---|---|
| Phase 0 | 基线（git 独立仓库 / sdkconfig.defaults / README / 现状审计） | ✅ |
| Phase 1 | Motor 命令队列 + 单任务独占 + 非阻塞 shake + disable | ✅ |
| Phase 2 | Input 组件（旋钮 ROTATE 事件，去抖/去重/复位） | ✅ |
| Phase 3 | 统一事件消费 + 触摸优先导航 | ✅ |
| Phase 4 | X-Knob 风格 UI 重构 + 工厂测试页 | ✅ |
| Phase 5 | app_state 传感器单一数据源 | ✅ |
| Phase 6 | 实时性隔离（motor 独占 core1，其余钉 core0） | ✅ |
| Phase 7 | 配置统一（NVS "webcfg" 单一命名空间） | ✅ |
| Phase 8 | LED 连接状态指示 | ✅ |
| 触摸专项 | 自研 XPT2046 驱动（店家轴向约定 + 标定 + 诊断） | ✅ |
| S-Dial 专项 | BLE HID 电脑控制 + 分区表 3MB + otadata 修复 | ✅ |
| 配网专项 | SoftAP 回退（热点+二维码+自动退出）+ STA 无限重连 | ✅ |
| Web v2 | 全功能管理页（电机/显示/LED/BLE/恢复出厂） | ✅ |
| HA 双向 | device_automation 触发器 + cmnd 命令通道 | ✅ |

所有阶段：编译验证通过、静态分析通过；**硬件实测：尚未验证**。

## 6. 跨模块数据流（现状）

```
motor_task(core1): Encoder → loopFOC → haptic_update → snap_position(volatile)
input_task(core0): 轮询 snap_position → knob_event_t → 队列
LVGL_task(core0):  消费 knob_event_t + LVGL 触摸事件 → 页面导航
                   → motor_set_mode* / motor_shake（命令队列）
scd40_task(core0): I2C → app_state 快照 → UI 环境页 / MQTT publish / Web status
wifi:       STA 无限重连 + SoftAP 回退 → app_state 状态 → UI 状态栏/配网页
mqtt:       传感器遥测 + HA device_automation 触发器 + smartknob/cmnd 命令
webcfg:     HTTP 管理页(状态/控制/配置/OTA/恢复出厂) + NVS
blehid:     BLE HID(音量/媒体/滚轮) → 状态栏图标
```

## 7. 任务与实时性隔离（现状）

| 任务 | 核 | 优先级 | 说明 |
|---|---|---|---|
| motor | core1 | 2 | 唯一操作 BLDCMotor；1ms 周期 |
| LVGL | core0 | 2 | 渲染 + 页面 + 输入消费 |
| input | core0 | 1 | 轮询位置快照 → 事件队列 |
| scd40 | core0 | 3 | I2C 轮询 → app_state |
| NimBLE host | core0 | 高 | BT 协议栈（`BT_NIMBLE_PINNED_TO_CORE=0`） |
| wifi / mqtt / httpd | core0 | — | 系统任务 |

核心保障：**motor 独占 core1**，其余全部钉 core0。

## 8. 模块清单（现状）

```
components/
  motor/      电机控制 + 力反馈引擎(单任务独占)
  input/      旋钮输入 → ROTATE 事件
  app_state/  传感器/WiFi/AP 共享状态单一数据源
  blehid/     BLE HID 设备(NimBLE): 电脑音量/媒体/滚轮
  ui/         页面系统 + 状态栏 + 工厂测试 + 配网页
  display/    ST7789 + XPT2046 裸驱动(自研,含标定/诊断) + LVGL 移植
  scd40/      SCD40 驱动
  wifi/       WiFi STA(无限重连) + SoftAP 配网回退
  mqtt_ha/    Home Assistant: 遥测 + 触发器发现 + cmnd 命令
  webcfg/     Web 管理页(状态/控制/配置/OTA/恢复出厂) + NVS
  led/        WS2812 状态灯(互斥保护)
```

## 9. BLE HID 电脑控制（S-Dial，仿 X-Knob Surface Dial）

- 协议：标准 BLE HID over GATT（HID Service 0x1812 + Report Map +
  Consumer Control Report(ID1) + Mouse Report(ID2) + Battery），设备名 `SmartKnob`。
- 免驱：Windows/macOS 蓝牙设置直接配对，绑定持久化（NVS），断线自动重连广播。
- 能力：音量加减/静音/播放暂停/上一首/下一首（消费控制）、滚轮与相对移动（鼠标）。
- 页面交互（`pg_pcdial`）：旋转=音量/滚轮（页面内切换模式），
  点中心=播放暂停，底部按钮=上下曲；状态栏蓝牙图标显示连接状态。

## 10. SoftAP 配网回退（WiFi 超时自动开热点）

```
开机 → STA 连接(无限重连, 最长等 WIFI_AP_FALLBACK_TIMEOUT_SEC=90s)
  ├─ 连上 → 正常运行
  └─ 超时 → wifi_ap_fallback_start():
       APSTA 模式, 热点 SmartKnob-XXXX(MAC尾缀),
       main 调 webcfg_start() → httpd 在 AP/STA 网段均可达,
       UI 500ms 轮询自动弹出 PAGE_APCFG(热点名+二维码+IP)
  用户连热点 → 浏览器扫码/输 IP → 管理页改 WiFi
  保存 → STA 新凭据重连(热点保持) → GOT_IP
       → wifi_ap_fallback_stop()(热点关闭) → 配网页自动退出 → MQTT 接上
```

- Kconfig：`WIFI_AP_FALLBACK_ENABLE/TIMEOUT_SEC/AP_PASSWORD`
- 配网页可返回键关闭（`ui_apcfg_set_dismissed`），热点保持到配网成功

## 11. Web 管理页（STA 与 AP 模式均可用）

- 状态页（5s 自动刷新）：IP/RSSI/WiFi/MQTT/热点/BLE HID/CO2/温湿度/
  电机模式与位置/亮度/熄屏/剩余内存/版本/运行时长
- 控制端点 `POST /api/set`：motor_mode / shake / estop /
  brightness / timeout / led(RRGGBB) / ble_disconnect
- 配置：WiFi/MQTT（NVS 持久化，保存后自动重连）
- OTA 固件上传；重启；恢复出厂（清 NVS + 重启）
- 并发保护：webcfg_start 临界区守卫（app_main 与 WiFi 事件任务可能并发调用）

## 12. Home Assistant 双向控制

### 12.1 通道总览

```
┌──────────┐   MQTT    ┌────────────┐   集成/Zigbee   ┌──────────┐
│ SmartKnob│ ─────────→│ Home       │ ──────────────→ │ 真实设备  │
│          │←───────── │ Assistant  │ ←────────────── │ 灯/风扇…  │
└──────────┘           └────────────┘                 └──────────┘
 上行1: 传感器遥测(co2/temp/rh)     [自动发现]
 上行2: smartknob/action 动作事件   [device_automation 触发器自动发现]
 下行:  smartknob/cmnd/# 命令      [shake / mode / led]
```

### 12.2 旋钮 → HA：设备自动化触发器（已实现）

旋钮向 HA 发布 16 个 `device_automation` 触发器发现配置
（灯光/空调/风扇/洗衣机 × on/off/left/right），动作主题 `smartknob/action`。

**HA 侧零 YAML**：设置 → 设备与服务 → SmartKnob 设备 → 16 个可绑定动作 →
自动化界面可视化绑定到任意真实实体。

例（旋钮"灯光 ON"控制真实灯泡）：
`自动化: 触发=设备(SmartKnob) light_on → 动作=灯.toggle`

### 12.3 HA → 旋钮：命令通道（已实现）

| 主题 | 载荷 | 效果 |
|---|---|---|
| smartknob/cmnd/shake | 任意 | 电机振动 |
| smartknob/cmnd/mode | 0-10 | 切换力反馈模式 |
| smartknob/cmnd/led | RRGGBB | LED 颜色 |

例：HA 自动化(门铃)调 `mqtt.publish` 主题 `smartknob/cmnd/shake`。

### 12.4 规划：设备状态回显 + 实体映射（Tier 3，未实现）

- **方案 A（推荐）**：Web 增"HA 实体映射"配置；旋钮订阅 HA statestream
  (`homeassistant/state/<entity>/state`) 回显真实设备状态；旋转映射亮度命令。
- **方案 B**：HA 建 MQTT 虚拟实体(command/state 指向 smartknob/...)
  + 模板自动化同步真实设备；旋钮直接读写该主题。

## 13. 工具链（tools/）

| 脚本 | 用途 |
|---|---|
| `build.ps1` | 一键构建（含完整 ESP-IDF 环境变量） |
| `gen_msyh_font.py` | 中文字库生成：扫描 UI 文案 → msyh.ttc → LVGL 4bpp。**新增文案后重跑** |
| `check_glyphs.py` | 校验 UI 用字是否全部被字库覆盖 |

## 14. 字库管线（lv_font_msyh_16）

- 格式：LVGL FMT_TXT / PLAIN 4bpp / SPARSE_TINY（unicode_list 升序）
- 度量：`ofs_y = ascent - bbox_top - box_h`（与 lv_draw_label 定位公式一致），
  `adv_w` 单位 1/16 像素
- 字符集：自动 = UI 全部用字 + ASCII + ° ·；当前 215 字形

## 15. 触摸子系统（自研 XPT2046 驱动）

- 店家实测约定：`0xD0=横轴(X), 0x90=纵轴(Y)`；`0xB0=Z1, 0xC0=Z2`
- 压力判定 `z = z1+4095-z2`（阈值 `TOUCH_Z_THRESHOLD`）；总线异常防护
- 标定 Kconfig：`TOUCH_X/Y_MIN/MAX` + `SWAP_XY/MIRROR_X/MIRROR_Y`
- 诊断：开机 selftest 日志 + `display_touch_get_raw()`（工厂测试实时显示）
- 轮询仅在 LVGL 任务 indev 回调内，与刷屏同任务串行
