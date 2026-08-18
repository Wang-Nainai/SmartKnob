# SmartKnob —— 基于 ESP32-S3 的智能力反馈旋钮

本科毕业设计：基于 BLDC + FOC 的力反馈旋钮，参考 X-Knob 与 scottbez1/SmartKnob，结合店家（M创动工坊）SimpleFOC 硬件方案实现。

## 硬件

| 部件 | 型号/规格 | 接口 |
|---|---|---|
| 主控 | ESP32-S3 N16R8 | 16MB Flash，当前未启用 PSRAM，CPU 160MHz |
| 显示屏 | ST7789 240x320 RGB565 | SPI2 @20MHz，双缓冲 DMA |
| 触摸 | XPT2046 | 与 LCD 共用 SPI2（CS=14） |
| 电机 | BLDC 2804（7 极对） | SimpleFOC 3PWM / MCPWM，12V 供电、限幅 5V |
| 编码器 | MT6701 | ABZ 增量模式，A=GPIO7，B=GPIO4，PPR=1024 (CPR=4096) |
| 环境传感器 | SCD40（CO2/温/湿） | I2C0 @100kHz（SDA=5，SCL=6） |
| 指示灯 | WS2812 单颗 | RMT，GPIO48 |
| 网络 | WiFi STA + MQTT(HA) + Web 配置/OTA | — |

完整 GPIO 定义见各组件 Kconfig 与源码。

## 功能

- 11 种力反馈手感模式（无边界/边界/多圈/开关/自动回中/精细/粗略/磁性档位/回中带制动）
- scottbez1 SmartKnob 力反馈算法移植（snap point、endstop、死区、怠速漂移校正）
- LVGL 9.2 页面系统（启动/菜单/手感/智能家居/环境/设置/系统）
- 手势输入：慢转导航、快旋确认、反转返回（无按键硬件）
- SCD40 环境监测，上报 Web 状态页与 Home Assistant（MQTT discovery）
- 网页配置（WiFi/MQTT，NVS 持久化）+ OTA 固件升级
- 屏幕亮度/自动熄屏（NVS 持久化）

## 软件结构

ESP-IDF 组件化（`components/`）：

```
main/          应用入口、任务编排
components/
  motor/       电机控制任务 + 力反馈引擎（SimpleFOC + MT6701）
  display/     ST7789 + XPT2046 + LVGL 移植层
  ui/          页面系统（smartknob_ui + pages/）
  scd40/       SCD40 驱动
  wifi/        WiFi STA + 重连
  mqtt_ha/     Home Assistant MQTT
  webcfg/      网页配置 + OTA + NVS
  led/         WS2812 状态灯
```

## 构建与烧录

依赖：ESP-IDF v5.5.5。

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

`components/` 与 `main/` 的第三方依赖由 `main/idf_component.yml` 声明（LVGL 9.2.0、esp_simplefoc 1.4.1、esp_lcd_touch_xpt2046 1.0.5）。

WiFi/MQTT 凭据通过网页配置写入 NVS，不提交到仓库（见 `sdkconfig.defaults`）。