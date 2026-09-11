# SmartKnob 问题记录

本文档记录 SmartKnob 的历史 Bug、根因、修复结论、当前状态和回归防护。

状态分类：

- `Baseline`：重构前已存在的问题。
- `Regression`：重构或后续实现引入的问题。
- `当前风险`：代码静态分析已发现，但没有在本任务中修复。
- `硬件待验证`：代码存在，但仓库没有当前硬件的可复核测试结论。

验证等级：

- `代码验证`：可从当前代码确认。
- `静态分析`：通过源码、配置和构建产物确认。
- `编译验证`：实际构建通过。
- `硬件实测`：必须有实体硬件测试结果；仓库当前没有完整报告。

---

## BUG-001（Baseline）LVGL layer buffer 分配失败和 64 KB pool

- 现象：进入含 `lv_scale` 圆环仪表的页面后，LVGL 任务长时间不喂狗，任务 WDT 复位；backtrace 位于 `lv_tlsf_malloc` / `lv_draw_layer_alloc_buf`。
- 原因：`lv_obj_set_style_clip_corner(scale, true)` 可能申请约 240 x 240 RGB565 层缓冲，约 112 KB；旧 LVGL heap 只有 64 KB，分配失败后在 draw layer 队列中反复重试。
- 归属：UI / Display / LVGL 配置。
- 历史修复：删除相关页面的 `clip_corner`，取消层缓冲需求。
- 当前状态：部分修复。`clip_corner` 已移除，但 SmartKnob 当前最终构建仍把 LVGL 解析成 builtin 64 KB allocator，设计中希望的 CLIB/PSRAM 迁移尚未实际生效。
- 含义：不能再把当前版本写成“LVGL 已使用 libc/PSRAM 并彻底解决 64 KB 池问题”。

## BUG-002（Baseline）`lv_scale_set_major_tick_every(0)` 除零

- 现象：进入环境页时出现 `Guru Meditation: IntegerDivideByZero`。
- 原因：`major_tick_every=0`，LVGL 9.2 在 `scale_find_section_tick_idx` 中执行取模，没有参数保护。
- 归属：UI。
- 状态：已修复；所有 scale 显式使用非零 tick 间隔。

## BUG-003（Baseline）旧快旋手势误判

- 现象：快速浏览被误判为确认，轻微回退被误判为返回；页面切换后可能残留旧输入。
- 原因：旧 `knob_timer_cb` 使用 3 步 / 250 ms burst 判定，没有稳定状态同步和输入基准重置。
- 归属：UI / Input。
- 修复方式：引入 Input 组件、事件队列、Motor mode sequence 和页面切换后的 `knob_input_reset()`；确认主要交给触摸。
- 当前状态：架构级修复。不得重新引入“快旋确认/反快旋返回”的旧交互。

## BUG-004（Baseline）`motor_shake` 跨线程直接操作 BLDCMotor

- 现象：确认或返回时偶发抖动异常。
- 原因：LVGL 线程中的 `motor_shake` 与 `motor_task` 同时访问 BLDCMotor。
- 归属：Motor / UI。
- 状态：已修复；Motor 单任务独占，Shake 改为命令队列和非阻塞状态机。

## BUG-005（Baseline）Shake 缺少正脉冲

- 现象：抖动只有负脉冲，手感不对称。
- 原因：Shake 实现没有先执行正方向脉冲。
- 归属：Motor。
- 状态：已修复；状态机先施加正脉冲，再施加负脉冲。

## BUG-006（Baseline）`motor_disable` 被 haptic 重新接管

- 现象：调用 `motor_disable()` 后下一周期 Motor 又继续施加力矩。
- 原因：没有持久的 `motor_control_enabled` 状态。
- 归属：Motor。
- 状态：已修复；Disable 后暂停 haptic 和 Shake，模式切换才重新使能。

## BUG-007（Baseline）UI 因 Shake 阻塞约 50 ms

- 现象：确认或返回时 UI 冻结。
- 原因：`motor_shake` 在 LVGL 线程内执行阻塞循环。
- 归属：UI / Motor。
- 状态：原 Shake 阻塞已修复为命令队列状态机。
- 当前风险：设置页 Motor 重新校准确认路径仍会在 LVGL 回调中执行 `vTaskDelay(600 ms)`。

## BUG-008（Baseline）`motor_set_mode_range` init 参数丢失

- 现象：设置范围时初始化位置被覆盖。
- 原因：旧命令结构只有 3 个参数，`max_position` 和 `init_position` 复用字段。
- 归属：Motor。
- 状态：已修复；`motor_cmd_t` 使用 `a1/a2/a3/a4` 保存 mode、min、max、init。

## BUG-009（Baseline，结论修正）XPT2046 轴向取决于面板接线

- 现象：触摸方向错误，表现为“完全无法触控”或坐标错位。
- 原因：数据手册命令名与模块实际玻璃接线约定不一致；不能只按手册字面判断。
- 归属：Display / 触摸驱动。
- 修复方式：自研 XPT2046 裸驱动使用 `0xD0=横轴 X`、`0x90=纵轴 Y`，并提供 SWAP/MIRROR Kconfig。
- 当前状态：代码已实现；最终方向仍需实体面板四点校准确认。

## BUG-010（Baseline，硬件待确认）触摸 IC 和模块版本

- 现象：某些 2.4 英寸模块原理图的 T_CLK/T_CS/T_DIN/T_DO/T_IRQ 标注 NC，可能没有触摸 IC。
- 影响：若模块没有 XPT2046，总线读数会异常或恒为 0/4095。
- 归属：硬件 / Display。
- 当前诊断：开机 selftest 打印 z1/z2/x/y；工厂测试页实时显示原始值。
- 状态：硬件待验证。恒 0/4095 或不随按压变化时必须检查接线和模块版本。

## BUG-011（Regression）`pg_hass` scale 句柄未保存

- 现象：HASS 控制视图更新了错误的容器对象，表盘指针不更新。
- 原因：重构时 `lv_scale_create()` 的返回值没有保存到页面数据结构。
- 归属：UI。
- 状态：已修复；页面使用 `d->scale` 保存句柄。

## BUG-012（Baseline）分区表缺少 `otadata`

- 现象：Web OTA 显示成功，重启后仍运行旧固件。
- 原因：`partitions.csv` 没有 `otadata` 分区，`esp_ota_set_boot_partition()` 无法持久化启动分区选择。
- 归属：分区 / OTA。
- 状态：已修复；新增 `otadata` 0x2000，并配置 3 个 3 MB 应用分区。
- 注意：分区表布局已变化，首次烧录建议 `erase-flash` 一次。

## BUG-013（Baseline，并入 BUG-009）触摸轴向修正

- 现象：对 XPT2046 轴向的早期结论不一致。
- 原因：把手册定义和模块接线约定混为一谈。
- 修复结论：以店家模块实测代码和实际面板接线为准；自研驱动使用 `0xD0=X`、`0x90=Y`。
- 状态：代码已采用该约定，最终硬件方向仍需四点校准。

## BUG-014（Regression）触摸总线异常导致幽灵连点

- 现象：触摸芯片缺失或 MISO 悬空时读数恒为 0/4095，压力值持续为有效值，UI 收到持续触摸。
- 原因：没有总线无效判定和坐标窗口。
- 归属：Display。
- 状态：已修复；加入 z1/z2 双高/双低判定、自适应压力基线、原始坐标窗口和双读一致性检查。

## BUG-015（Regression）返回菜单后 Motor 模式不恢复

- 现象：从手感设置子页面返回主菜单后仍保留子页面 Motor 模式，菜单浏览手感错误。
- 原因：页面只在 `create` 时设置 Motor 模式，pop 后没有恢复栈顶页面状态。
- 归属：UI / page_mgr。
- 状态：已修复；pop 动画结束后调用栈顶页面 `on_resume`，页面重新声明 Motor mode/range，并调用 `knob_input_reset()`。

## BUG-016（Baseline）BLE 广播缺少 appearance present 标志

- 现象：某些主机的 BLE HID 设备类型识别异常。
- 原因：该 NimBLE 版本的广播 appearance 需要 `appearance_is_present`。
- 归属：BLE HID。
- 状态：已修复；广播设置 `appearance_is_present=1`。

## BUG-017（Baseline）UI 改版后中文缺字

- 现象：X-Knob 风格页面出现方框、空白和特殊符号丢失。
- 原因：旧字库只包含少量旧文案，且早期没有完整覆盖 ASCII、度和间隔号。
- 归属：UI / Font。
- 历史结果：曾经生成 215 字形并通过覆盖检查。
- 当前状态：字库已重新生成，当前是 280 字形，`line_height=22`、`base_line=5`。
- 当前工具缺口：`gen_msyh_font.py` 扫描 Motor 文案，但 `check_glyphs.py` 只检查 UI 页面和 `smartknob_ui.c`，不检查 `motor.cpp`。

## BUG-018（Regression）`lv_scale` 吞掉页面级点击

- 现象：手感页、HASS 控制页和设置编辑页点击表盘不触发页面动作。
- 原因：LVGL 对象默认可能带 `CLICKABLE` 标志，scale 没有显式移除。
- 归属：UI。
- 状态：已修复；相关 scale 使用 `lv_obj_remove_flag(..., LV_OBJ_FLAG_CLICKABLE)`。

## BUG-019（Regression）浮层位于 flex 滚动容器内部导致错位

- 现象：HASS 控制层和设置编辑层被 flex 布局放进内容流，位置和滚动偏移错误。
- 原因：浮层是 flex + pad 滚动根对象的子对象。
- 归属：UI。
- 状态：已修复；列表放在独立滚动容器，浮层作为页面根的直接子对象覆盖列表。

## BUG-020（Regression）触摸测试浮层超出页面产生滚动

- 现象：触摸测试全屏视图高度超过页面，可被拖滚。
- 原因：浮层高度和根页面滚动策略错误。
- 归属：UI。
- 状态：已修复；触摸测试高度调整为 298，相关非列表页禁用滚动。

## BUG-021（Regression）校准持久化导致部分开机电机失控疯转

- 现象：引入 NVS 校准持久化后，有时开机 FOC 立即介入且换向错误，轻碰电机即失控加速旋转。
- 原因：MT6701 ABZ 是不带 Z 索引的增量编码器，编码器计数以上电瞬间转轴位置为零点，每次开机零参考都不同。`zero_electric_angle` 是相对于本次开机计数零参考的偏移（`electricalAngle() = normalize(dir * pole_pairs * getMechanicalAngle() - zero_electric_angle)`），跨开机保存必然错位；转轴停位与上次开机相近时错位小（看似正常），停位远时电角度错位超过 90 度，力矩变正反馈，一碰即失控。
- 归属：Motor / FOC 校准持久化。
- 修复方式：NVS 只持久化接线方向 `sensor_direction`（单 blob `mcal/cal`，含 magic/版本，原子写入）；零电角每次开机由 `alignSensor()` 重新锚定；全流程校准的保存增加极对数校验（`pp_check_result`）门控；旧三键存档开机自动迁移方向。
- 当前状态：已修复。禁止恢复“预置 NVS 中的 zero_electric_angle 跳过零电角测量”；除非改用带 Z 索引或绝对式接口，否则零电角必须每次开机重新锚定（开机时间约 1.5 秒是增量编码器的物理下限）。

---

# 当前静态分析发现

以下问题不是凭空推测，均来自当前源码、构建配置或 Kconfig 映射。本次任务只同步文档，不修改业务代码。

## CUR-001 当前 LVGL 仍实际使用 builtin 64 KB allocator

- 证据：构建产物同时存在 `CONFIG_LV_USE_STDLIB_MALLOC=1` 和 `CONFIG_LV_USE_BUILTIN_MALLOC=1`。
- 原因：LVGL 9.2 的 `lv_conf_kconfig.h` 先根据 `CONFIG_LV_USE_BUILTIN_MALLOC` 把 `CONFIG_LV_USE_STDLIB_MALLOC` 定义为 `LV_STDLIB_BUILTIN`。
- 影响：设计中的 CLIB + PSRAM 迁移没有生效；LVGL 对象分配仍受 64 KB builtin pool 限制。
- 状态：当前风险。需要单独修复配置并重新验证，不能直接宣称已解决。

## CUR-002 Web 环境历史时间轴间隔错误

- 证据：`env_hist.c` / `env_hist.h` 的日级缓冲按 60 秒写入；`webcfg.c` 的 `/api/envhist` 返回 `"iv_s":300`。
- 影响：浏览器按 5 分钟间隔绘制，实际数据点是 1 分钟间隔，时间轴和鼠标悬浮值会错位。
- 状态：当前风险。未修改代码。

## CUR-003 page pop 在动画 ready 回调中删除页面

- 证据：`pm_pop_anim_done()` 删除页面并释放页面数据。
- 影响：与“动画回调中不要删除 LVGL 对象”的硬约束冲突。当前实现可能与已有对象宿主有关，但不能把这种模式推广到其他动画对象。
- 状态：当前风险，待单独分析 LVGL 动画链表和页面生命周期后处理。

## CUR-004 设置页重新校准在 LVGL 回调中阻塞 600 ms

- 证据：`pg_setting.c` 的 Motor 重新校准路径执行 `vTaskDelay(pdMS_TO_TICKS(600))` 后重启。
- 影响：短暂阻塞 UI，违反 UI 回调不做长阻塞的约束。
- 状态：当前风险。由于随后重启，影响窗口有限，但仍应移除或改为非阻塞重启流程。

## CUR-005 主菜单文案仍写 11 种模式

- 证据：`pg_menu.c` 的描述文案写“11 种手感模式”，实际 `motor_get_mode_count()` 为 12。
- 影响：UI 信息与实际 Motor 模式数量不一致。
- 状态：当前风险，属于 UI 文案修复。

## CUR-006 `README.md` 与当前代码不一致

- 证据：README 仍写 PSRAM 未启用、SPI 20 MHz、11 种模式和旧依赖。
- 影响：新开发者可能按 README 得到错误硬件和功能结论。
- 状态：本文档同步未修改 README；README 需要单独更新。

## CUR-007 Motor 命令入队存在最多 20 ms 有界等待

- 证据：`post_cmd()` 使用 `xQueueSend(..., pdMS_TO_TICKS(20))`。
- 影响：调用方通常不是长期阻塞，但 UI 或网络任务在队列满时仍可能等待最多 20 ms，因此不能把该 API 描述成严格零等待。
- 状态：当前风险。队列长度 8 和命令频率较低时影响有限，但实时性审计必须考虑该边界。

---

# Regression Prevention

以下规则来自历史故障和当前代码事实，后续修改必须遵守：

1. UI、MQTT、BLE、Web、Input 不得直接操作 BLDCMotor。
2. Shake 必须保持非阻塞状态机；不得在 UI 线程调用 `motor.loopFOC()` 或长时间 `delay()`。
3. `motor_disable()` 必须持续抑制 haptic，直到下一次模式切换明确重新使能。
4. 页面切换必须同步 encoder snapshot、Motor mode sequence 和 Input event queue。
5. `page pop` 后必须让新栈顶页面执行 `on_resume`，重新声明 Motor mode/range。
6. `lv_scale` 不应吞掉页面级点击；按页面交互语义显式管理 `CLICKABLE`。
7. 全屏浮层不要放在 flex scroll content 内部。
8. LVGL 动画回调不要删除仍有关联动画的对象；`ui_label_roll` 必须复用幽灵标签。
9. 不恢复 LVGL 64 KB builtin allocator 作为正式内存方案。
10. 字库生成和校验必须与源码文案同步。
11. 触摸总线异常不能造成幽灵点击。
12. 不关闭 comprehensive heap poisoning 来掩盖越界或 heap corruption。
13. 不把 `CONFIG_LV_USE_STDLIB_MALLOC=1` 单独当成 CLIB 已生效的证据。
14. 不把代码存在或编译通过写成硬件已验证。
15. 不把 Web 恢复出厂描述成全 NVS 擦除；它当前只清 `webcfg`。
16. 环境历史当前使用固定间隔推时间，没有每个样本的绝对时间戳；新增 API 前不得假装已有真实时间戳。

---

# 硬件实测待办清单

| 项目 | 检查方法 | 当前结论 |
|---|---|---|
| 首次烧录 | `erase-flash` 后烧录 | 分区表已变更，待实机确认 |
| 触摸 IC | 开机 selftest + 工厂测试原始值 | 待实机确认是否存在并接线正确 |
| 触摸方向 | 四点校准和四角点击 | 待实机确认 |
| BLE 配对 | Windows/macOS 搜索 SmartKnob | 待实机确认 |
| S-Dial 音量 | `pg_pcdial` 旋转 | 待实机确认 |
| S-Dial 滚轮 | 切换滚轮模式后旋转 | 待实机确认 |
| 媒体控制 | 播放暂停、上一首、下一首 | 待实机确认 |
| 菜单聚焦动画 | 主菜单旋转和触摸滑动 | 待实机确认 |
| Motor 闭环/力反馈 | `pg_playground` 12 模式 | 待实机确认 |
| WiFi/AP 配网 | STA 连接、90 秒回退、192.168.4.1 | 待实机确认 |
| MQTT/HA Discovery | 3 个 sensor + 16 个 device_automation | 待实机确认 |
| Web/OTA | `/status`、`/api/envhist`、`/ota` | 待实机确认 |
| LED | WiFi/MQTT 状态色和工厂测试 | 待实机确认 |
| 长时间稳定性 | 连续运行、heap poisoning 日志、WDT | 待实机确认 |

---

# 历史结论与当前事实的区别

- “代码实现”不等于“硬件已经验证”。
- “编译通过”不等于“运行稳定”。
- “移除了某个触发条件”不等于“根因架构已经完成”。
- “配置项存在于 `sdkconfig`”不等于“该配置最终生效”。
- “任务或页面存在”不等于“实际硬件已经完成回归测试”。
