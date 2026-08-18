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

---

## 硬件实测待办清单（供明天检查）

| 项 | 方法 | 预期 |
|---|---|---|
| 电机闭环/力反馈 | 开机进手感页 | 11 模式正常，无抖动/啸叫/发热 |
| 确认振动 | 触摸/操作时 | 单次清晰"咔哒" |
| 触摸 | 点击菜单项 | 正确响应、进入对应页面 |
| 触摸坐标校准 | 工厂测试-触摸测试 | 点击位置与显示坐标对应 |
| 旋转浏览 | 慢转 | 焦点逐格移动、无跳跃/误判 |
| 快旋 | 快速旋转 | 仅快速浏览，不会误触发确认/返回 |
| 页面切换残留 | 快速进出页面 | 无残留旋转导致跳页 |
| WiFi/MQTT | 状态栏 + 网页 | 绿/青点亮 |
| LED | 观察 | 启动蓝→WiFi绿→MQTT青→断网红 |
| SCD40 | 环境页 | 数值 5s 刷新 |
| 工厂测试 | 系统→工厂测试 | 触摸/LED/电机/编码器/SCD40/网络 各测试项正常 |
| 稳定性 | 长时间运行 | 无 WDT 复位/Guru/卡死 |

## 回归记录（本次大重构）

- 编译：✅ 全量通过，零 warning
- 引用检查：✅ 已删除 API（smartknob_ui_set_env/webcfg_set_env/on_confirm）无残留
- 竞态检查：✅ BLDCMotor 仅 motor_task 访问；跨任务均走 volatile 快照/队列
- 循环依赖：✅ 无（app_state 无依赖，ui→input/motor/mqtt/wifi/led/app_state）
- 任务布局：✅ motor 独占 core1；scd40/input/wifi 钉 core0
- 内存：✅ LVGL 64KB 无大层缓冲；motor 4KB / input 2KB / scd40 4KB 栈