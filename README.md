# SmartKnob —— 基于 ESP32-S3 的联网力反馈智能旋钮

基于 BLDC + FOC 的力反馈旋钮，参考 X-Knob 与 scottbez1/SmartKnob，结合 SimpleFOC 硬件方案实现。

- 本地：LVGL 9.2 终端 UI（旋钮 + 触摸交互）+ 13 种力反馈手感 + 环境监测
- PC：BLE HID 仿 Surface Dial（Windows 10 1903+ 免驱），切歌/音量/滚动/系统拨号盘
- 智能家居：MQTT 接入 Home Assistant，设备槽位可在 Web 上自由配置
- 管理：Web 页面配置 WiFi/MQTT/HA 设备 + 环境趋势 + OTA 固件升级

> 硬件架构、设计决策、历史 Bug 与防回退规则见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) 与 [`docs/BUGS.md`](docs/BUGS.md)。

## 硬件

| 部件 | 型号/规格 | 接口 |
|---|---|---|
| 主控 | ESP32-S3 N16R8 | 16MB Flash，8MB PSRAM，CPU 160MHz |
| 显示屏 | ST7789 240x320 RGB565 | SPI2 @80MHz，双缓冲 DMA |
| 触摸 | XPT2046 | 与 LCD 共用 SPI2（CS=14），四点校准 |
| 电机 | BLDC 2804（7 极对） | SimpleFOC 3PWM / MCPWM，12V 供电、限幅 5V |
| 编码器 | MT6701 | ABZ 增量模式，A=GPIO7，B=GPIO4，PPR=1024 (CPR=4096) |
| 环境传感器 | SCD40（CO2/温/湿） | I2C0 @100kHz（SDA=5，SCL=6） |
| 指示灯 | WS2812 单颗 | RMT，GPIO48 |
| 网络 | WiFi STA + MQTT(HA) + Web 配置/OTA | — |

完整 GPIO 定义见各组件 Kconfig 与源码。

## 功能

- **13 种力反馈手感模式**（无边界/边界/多圈/开关/自动回中/精细/粗略/磁性档位/回中带制动/调节器），SimpleFOC 力反馈算法（snap point、endstop、死区、怠速漂移校正）
- **S-Dial 电脑控制**（BLE HID，Windows 10 1903+ 原生 Surface Dial 协议 + 媒体键兼容模式）：
  - 切歌（默认）/音量/滚轮/表盘四种模式，单击模式键循环切换
  - 表盘模式：旋转由 Windows 系统拨号盘处理，长按中央弹系统圆盘菜单
  - 中央短按 = 播放/暂停，上一首/下一首恒可用，触摸手势横滑音量、竖滑滚轮
- **LVGL 9.2 页面系统**（X-Knob 风格三副本无限循环列表/Apple 风格全屏表盘）
- **Home Assistant**：MQTT Discovery（传感器 + 设备自动化触发器 + level 通道），
  设备槽位（灯/空调等，最多 6 个）通过 Web 表单自由配置
- **环境监测**：SCD40 CO2/温湿度，设备端 2 小时趋势图 + Web 24 小时趋势 + HA 状态
- **Web 管理台**：WiFi/MQTT/HA 设备配置（NVS 持久化）、实时状态、OTA、恢复出厂
- **BLE HID Surface Dial 协议**：Windows 免驱识别（Rudimentary Dial），支持系统圆盘菜单
- 交互：旋转=浏览/调节（力反馈棘轮），触摸=确认/返回；快速逆时针甩动=返回（页面白名单）
- 屏幕亮度/自动熄屏（NVS 持久化）+ 工厂测试页

## 注意

- 分区表含 otadata、应用分区 3MB×3；**首次烧录建议先 `idf.py erase-flash`**。
- BLE HID 报文协议是"纯数据"形态（不带 Report ID 前缀），改动前必读
  [`docs/ARCHITECTURE.md` §14.2](docs/ARCHITECTURE.md)。
- Motor FOC 校准只持久化接线方向，零电角每次开机重新锚定（ABZ 增量编码器无绝对
  零位，跨重启复用零电角会导致开机失控，见 `docs/BUGS.md` BUG-021）。

## 软件结构

ESP-IDF 组件化（`components/`）：

```
main/          应用入口、任务编排
components/
  motor/       电机控制任务 + 力反馈引擎（SimpleFOC + MT6701，独占 core1）
  display/     ST7789 + XPT2046 + LVGL 移植层
  ui/          页面系统（smartknob_ui + pages/，page_mgr 生命周期管理）
  input/       旋钮输入（Motor 快照 → 事件队列）
  blehid/      BLE HID（Surface Dial + Consumer + Mouse）
  mqtt_ha/     Home Assistant MQTT（发现/动作/level 通道）
  hass_cfg/    HA 设备槽位配置解析（Web 表单 → NVS）
  webcfg/      Web 管理台 + OTA + NVS 配置
  wifi/        WiFi STA + SoftAP 配网回退
  scd40/       SCD40 驱动（含 60 秒自愈重启）
  env_hist/    环境历史环形缓冲（PSRAM）
  app_state/   跨任务环境/网络状态快照
  led/         WS2812 状态灯（RMT，多任务互斥）
  sysmon/      系统监控采样
```

## 构建与烧录

依赖：ESP-IDF v5.5.5（LVGL 9.2.0、esp_simplefoc 1.4.1 等第三方组件由
`main/idf_component.yml` 声明，编译时自动拉取到 `managed_components/`）。

Windows 项目脚本（推荐）：

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1 build
powershell -ExecutionPolicy Bypass -File tools\build.ps1 flash
```

标准 idf.py 流程：

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

**首次烧录建议先 `idf.py erase-flash`**（分区表含 otadata，应用分区 3MB×3）。

## 配置

- WiFi / MQTT / HA 设备：设备开机后通过 Web 管理页（`http://<设备IP>`，
  配网模式 `192.168.4.1`）写入 NVS，**凭据不提交到仓库**（`sdkconfig` 已被
  `.gitignore` 排除，`sdkconfig.defaults` 无敏感默认值）。
- 中文 UI 文案修改后运行 `python tools/gen_msyh_font.py` 重新生成字库，
  再用 `python tools/check_glyphs.py` 校验覆盖。
- 触摸首次使用会进入四点校准向导（XPT2046 通用数据手册的轴向与本面板
  玻璃接线不一致，必须以实测校准为准）。

## 文档

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) —— 软件架构、任务/数据流、
  校准与 NVS 设计、防回退规则（改代码前先读）
- [`docs/BUGS.md`](docs/BUGS.md) —— 历史 Bug 记录、根因、回归防护、硬件实测清单

## 致谢

- [scottbez1/SmartKnob](https://github.com/scottbez1/SmartKnob) —— 力反馈旋钮思路与算法参考
- [SmallPond/X-Knob](https://github.com/SmallPond/X-Knob) —— UI 风格与 Surface Dial 方案参考
- [esp32-surface-dial](https://github.com/) / [super-dial](https://github.com/) —— Surface Dial HID 协议实测参考
- Espressif esp-iot-solution `usb_surface_dial` —— 官方 Surface Dial 描述符参考
- SimpleFOC、LVGL 及 ESP-IDF 社区

## 许可证

本项目以 [GPL-3.0](LICENSE) 发布。
