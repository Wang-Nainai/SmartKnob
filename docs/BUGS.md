# SmartKnob 问题记录

区分两类问题：
- **Baseline Bug**：重构前已存在的缺陷（重构只是暴露或尚未触及）。
- **Regression Bug**：本次重构新引入的缺陷。

每条记录：现象 / 原因 / 归属模块 / 类型 / 状态 / 修复阶段。

---

## BUG-001（Baseline）LVGL 层缓冲分配死循环

- 现象：进入含 `lv_scale` 圆环仪表的页面后，LVGL 任务 >5s 不喂狗，任务 WDT 复位
  （backtrace 全在 `lv_tlsf_malloc` / `lv_draw_layer_alloc_buf`）。
- 原因：`lv_obj_set_style_clip_corner(scale, true)` 需要 240×240 层缓冲
  （RGB565 = 112KB），但 LVGL 堆仅 64KB（`CONFIG_LV_MEM_SIZE_KILOBYTES=64`）。
  分配失败 → `draw_buf_flush` 的 `while(layer->draw_task_head)` 无限重试。
- 归属模块：ui / display（LVGL 配置）。
- 状态：✅ 已修复（删除 4 个页面的 `clip_corner`，层缓冲不再需要）。
- 修复阶段：Phase 0 前（会话内修复）。

## BUG-002（Baseline）`lv_scale_set_major_tick_every(0)` 除零崩溃

- 现象：进入环境页时 `Guru Meditation: IntegerDivideByZero`，
  PC 在 `scale_find_section_tick_idx`（`tick_idx % major_tick_every`）。
- 原因：设置 `major_tick_every = 0`，LVGL 9.2 无入参校验。
- 归属模块：ui。
- 状态：✅ 已修复（全部改为 1，label 隐藏无视觉差异）。
- 修复阶段：Phase 0 前。

## BUG-003（Baseline）旋钮快旋手势误判

- 现象：快速浏览被误判为"确认"；轻微回退被误判为"返回"；页面切换后残留旧输入。
- 原因：旧 `knob_timer_cb` 用"3 步/250ms burst"判定，无速度信息、无稳健去重，
  且页面切换仅部分重置状态（`burst_start_tick` 未清零）。
- 归属模块：ui（输入处理）。
- 状态：✅ 架构级修复（Phase 2/3 重构为 input 组件 + 触摸优先交互，见 ARCHITECTURE.md §3）。
- 修复阶段：Phase 2 / Phase 3。

## BUG-004（Baseline）motor_shake 跨线程操作 BLDCMotor

- 现象：确认/返回时偶发抖动异常（UI 线程 `motor_shake` 与 motor_task 同时调 `loopFOC`）。
- 原因：shake 在 LVGL 线程内 `loopFOC` 死循环 50ms，与 motor_task 并发访问 BLDCMotor。
- 归属模块：motor。
- 状态：✅ 已修复（Phase 1：命令队列 + 非阻塞 shake 状态机；正脉冲缺失也已补）。
- 修复阶段：Phase 1。

## BUG-005（Baseline）motor_shake 缺正脉冲

- 现象：抖动只有负脉冲，手感不对称。
- 原因：Phase 1 首次实现时 `start_shake` 未先执行 `motor.move(+strength)`。
- 归属模块：motor。
- 状态：✅ 已修复。
- 修复阶段：Phase 1。

## BUG-006（Baseline）motor_disable 形同虚设

- 现象：`motor_disable()` 后下一周期 haptic 重新接管。
- 原因：无 `motor_control_enabled` 标志，DISABLE 只清了一次 move(0)。
- 归属模块：motor。
- 状态：✅ 已修复。
- 修复阶段：Phase 1。

## BUG-007（Baseline）UI 阻塞 50ms
- 现象：每次确认/返回操作 UI 冻结约 50ms。
- 原因：`motor_shake` 在 LVGL 线程内阻塞循环。
- 归属模块：ui / motor。
- 状态：✅ 已修复（非阻塞 shake）。
- 修复阶段：Phase 1。

## BUG-008（Baseline）`motor_set_mode_range` 参数丢失

- 现象：命令结构体把 max_position 与 init_position 复用同一字段，init 值丢失。
- 原因：Phase 1 首次实现 `motor_cmd_t` 只有 3 个参数。
- 归属模块：motor。
- 状态：✅ 已修复（增加 a4 字段）。
- 修复阶段：Phase 1。

## BUG-009（Baseline，触摸失灵的代码级根因）XPT2046 XY 轴互换

- 现象：屏幕"完全无法触控"（触摸即使被检测到，坐标也是错的，点哪里都不命中控件）。
- 原因：第三方驱动 `atanisoft/esp_lcd_touch_xpt2046` 把寄存器命令字标反——
  它用 `0x90` 读 Y、`0xD0` 读 X；而 XPT2046/TSC2046 数据手册及所有通用触摸库
  （TFT_eSPI / XPT2046_Touchscreen 等）均为 **0x90=X、0xD0=Y**。
- 归属模块：display（第三方 managed component）。
- 状态：✅ 已修复——抛弃该组件，在 `display.c` 内自研约 100 行 XPT2046 裸驱动
  （数据手册标准命令 + 压力判定 `z = z1+4095-z2` + 可配置标定 + 原始值诊断 API），
  并从 `idf_component.yml` 移除依赖（固件 -4.5KB）。
- 修复阶段：触摸专项。

## BUG-010（Baseline，待硬件确认）触摸接线存疑

- 现象：此类红板触摸 SPI（T_CLK/T_DIN/T_DO/T_CS/T_IRQ）与 LCD 是**独立引脚**，
  若只接了 T_CS 而 T_CLK/T_DIN/T_DO 未接到 SCK(GPIO12)/MOSI(GPIO11)/MISO(GPIO13)，
  读数将恒为 0 或 4095，触摸完全无效。
- 归属模块：硬件接线。
- 状态：⏳ 待硬件确认——已内置两处诊断：
  1. 开机日志 `XPT2046 selftest: z1=.. z2=.. x=.. y=..`（未按下应 z1≈0, z2≈4095；
     若恒 0/4095 且按下不变 → 接线问题）
  2. 工厂测试→触摸测试：实时显示 z1/z2/raw_x/raw_y（按下时 z1 应明显增大）

## BUG-011（Regression，自检发现并修复）pg_hass 表盘句柄未保存

- 现象（潜在）：HASS 控制视图指针更新用了错误的容器对象。
- 原因：重构时 `lv_scale_create` 返回的局部变量未存入页面结构体。
- 归属模块：ui。
- 状态：✅ 已修复（d->scale 字段）。
- 修复阶段：UI 重构（X-Knob 风格）。

---

## 硬件实测待办清单（供明天检查）

| 项 | 方法 | 预期 |
|---|---|---|
| **触摸接线** | 开机日志 selftest + 工厂测试→触摸测试 | 按下时 z1 明显增大；raw x/y 随触摸变化 |
| **触摸方向** | 触摸测试：点屏幕四角 | 显示坐标与手指位置一致；不一致调 `TOUCH_SWAP_XY/MIRROR_X/MIRROR_Y` |
| **触摸标定** | 点屏幕边缘 | 边缘可点中；偏差调 `TOUCH_X/Y_MIN/MAX`（sdkconfig） |
| 菜单聚焦动画 | 旋转 | 聚焦项图标列收窄+右侧红边+回弹动画，滚动跟随 |
| 菜单点击 | 触摸列表 | 进入对应页面 |
| HASS 控制 | 点击设备→旋转/点击 | 全屏表盘; 旋转发 LEFT/RIGHT, 点击发 ON/OFF |
| 设置编辑 | 点击进入→旋转→点击保存 | 实时预览亮度; 保存到 NVS |
| 电机闭环/力反馈 | 开机进手感页 | 11 模式正常，无抖动/啸叫/发热 |
| 确认振动 | 触摸操作时 | 单次清晰"咔哒" |
| WiFi/MQTT/LED | 状态栏 + LED | 绿/青点亮 |
| SCD40 | 环境页 | 数值 5s 刷新 |
| 稳定性 | 长时间运行 | 无 WDT 复位/Guru/卡死 |

## 回归记录（触摸专项 + UI 重构）

- 编译：✅ 全量通过，零 warning
- 依赖：✅ atanisoft/esp_lcd_touch 已移除并自动清理（固件 0x1a7400，17% 空闲）
- 引用检查：✅ 无 esp_lcd_touch 残留
- 竞态检查：✅ 触摸轮询仅发生在 LVGL 任务内（indev 回调），无跨任务 SPI 访问
- 实时性：✅ LVGL 钉 core0（5ms 最小唤醒），motor 独占 core1