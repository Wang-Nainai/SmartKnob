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
- 当前状态：已彻底修复。`clip_corner` 已移除（历史修复），且 CUR-001 已解决：
  LVGL malloc choice 切 CLIB，`lv_malloc` → libc `malloc` → SPIRAM
  （见 CUR-001 与 ARCHITECTURE.md §9），clip_corner 类大层缓冲现在可以直接分配。
- 含义：64 KB builtin 池问题从机制上消除；防回退见 ARCHITECTURE.md §9.3 / §22 第 26 条。

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
- 当前状态：字库已重新生成，当前是 294 字形，`line_height=22`、`base_line=5`。
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

## BUG-022（Regression）手感页红弧到处出现/画满一圈

- 现象：手感页切到无界模式整个环被红色填满；开关模式位置在量程内也冒出红色溢出弧。
- 原因：两个错误叠加。1) 误把 `motor_get_angle_offset_deg()` 当越界距离——它实际是档内偏角 `angle_to_detent_center`（motor.cpp:512），恒可能非 0（粗档 ±4°，60° 档距模式可达 ±30°），"off != 0 就画红"在所有模式随机触发。2) `lv_arc_set_angles(start, end)` 在 start > end 时 LVGL 顺时针绕远路绘制（25→0 = 335°），直接画满一圈。
- 归属：UI / pg_playground。
- 修复方式：红弧双条件判定——位置停在边界档 AND 档内偏移方向朝界外，幅值封顶 30°；无界模式永不画红弧/填充并清理旧填充残留；切模式瞬间强制清零红弧（电机命令异步，瞬间 offset 是旧值）。
- 当前状态：已修复。防回退：`motor_get_angle_offset_deg()` 不是越界距离，禁止用它单独做越界判定。

## BUG-023（Regression）HASS 空调温度填充弧不可见

- 现象：空调温度调节时圆环上无任何反馈（只有中央数字变化）。
- 原因：控制视图圆环的 indicator 宽度为 0（v4"圆点负责指示"时代的残留样式），温度改用填充弧后从未打开宽度。
- 归属：UI / pg_hass。
- 修复方式：indicator 宽度 12 + 圆头，颜色随开关状态联动。
- 当前状态：已修复。防回退：表盘弧层创建时必须核对指示层宽度；全局规则见 ARCHITECTURE.md §22 第 17 条（填充弧与圆点互斥、一律 `lv_arc_set_angles` 直接设角、禁止 `lv_arc_set_value`——其内建值动画与 50ms 定时器连发冲突曾造成填充滞后回弹）。

## BUG-024（硬件实测）BLE HID 报文携带 Report ID 前缀导致三类输入全部异常

- 现象（实机 Windows 实测）：表盘模式旋转无反应或单方向；上一首/下一首都触发下一首；点击中央被 Windows 当成一次滚动。
- 根因：Windows BLE HID 栈按 input characteristic 的 Report Reference 匹配报告，通知内容全部是数据，**不带 Report ID 前缀**。报文首字节写 Report ID 会把 ID 污染进数据位：
  - dial `{10,...}`：0x10 使旋转字段错位，解码值超出 logical 范围被丢弃
  - consumer `{01,mask,...}`：0x01 的 bit0 恰好 = Scan Next 用法，无论 mask 都触发下一首
  - dial button `{10,03,00,00}`：Dial 字段被污染成 +768（+76.8°），Windows 执行一次滚动
- 归属：BLE HID 协议层。
- 修复方式：全部报文改为纯数据（consumer 2B / mouse 4B / dial 3B）；dial TLC 加 Touch 位（Button1+Touch 双位，Touch 恒 1）；每档旋转 ±100（10 度/档，对齐 Espressif 官方与 X-Knob）；PnP PID 0x4004→0x4005 强制 Windows 重新枚举读新 map。
- 当前状态：已修复并有阶段性实测反馈（旋转方向、上一首/下一首、切歌模式可用）。防回退：改 HID 报文前必须重读 ARCHITECTURE.md §14.2；**报文不得携带 Report ID 前缀**。

## BUG-025（Regression）按住按钮退出页面时崩溃（LoadProhibited）

- 现象：触摸按着页面按钮未松手时退出页面，偶发 Core 0 panic（EXCVADDR=0x0）。
- 根因：`pm_delete_page` 原顺序先 `destroy()`（free 页面数据）再 `lv_obj_delete`；LVGL 删除对象树广播 `LV_EVENT_DELETE`，S-Dial 页面回调以 `LV_EVENT_ALL` 注册，收到 DELETE 后继续解引用已置 NULL 的 `p->data`。
- 归属：UI / page_mgr / pg_pcdial。
- 修复方式：1) `pm_delete_page` 顺序改为先删对象树后释放数据（CUR-003 缓解，已核对 12 个页面 destroy 均不访问 root）；2) filter=ALL 的页面回调入口拦截 `LV_EVENT_DELETE`。
- 当前状态：已修复。防回退：以 ALL 注册的页面回调必须拦截 DELETE 事件；新增页面回调时同检。

## BUG-026（Regression）"电机档位即值"页面快转瞬时越界写非法值

- 现象：HASS 空调温度快转出现 31/15、风速 `ac_fan` 越界索引 `ac_fan_names[4]`；设置页亮度保存 9/101、熄屏 -1 分钟。
- 根因：端点制动拉回前 `motor_get_position()` 会短暂越过量程；页面直接采用未钳制。
- 归属：UI / pg_hass / pg_setting。
- 修复方式：读电机位置后按模式量程钳制（温度 16..30、风速 0..3、亮度 10..100、分钟 0..30）。
- 当前状态：已修复。防回退：任何"电机档位即值"页面必须钳制，规则见 ARCHITECTURE.md §22 第 22 条。

## BUG-027（Baseline，安全）急停命令可能被命令队列静默丢弃

- 现象：命令队列满（8 条）时 `motor_disable()` 走 FIFO 尾部入队 20ms 后被丢弃，力反馈继续生效。
- 归属：Motor。
- 修复方式：急停改 `xQueueSendToFront` 插队，队满挤掉最旧普通命令。
- 当前状态：已修复。防回退：安全命令不得排 FIFO 尾部。

## BUG-028（Baseline）SCD40 持续总线错误不触发自愈

- 现象：I2C 持续故障（非"未就绪"）时 `not_ready_count` 不增长，60 秒自愈流程永不运行，传感器永久静默。
- 修复方式：自愈计数前移，总线错误与 CRC 错误路径同样累计。
- 当前状态：已修复。

## BUG-029（Baseline）MQTT discovery 定时器与 reinit 竞争（use-after-free）

- 现象：discovery 序列发送期间（30+ 条 × 400ms）Web 保存配置触发 `mqtt_ha_reinit`，esp_timer 回调在锁外使用刚被 destroy 的 client。
- 归属：MQTT。
- 修复方式：`disc_timer_cb` try-take `s_client_mux`（esp_timer 不可阻塞），拿不到跳过本轮。
- 当前状态：已修复。

## BUG-030（Baseline）hass_cfg 名称占满截断宽度时无 NUL 终止

- 现象：设备名恰为 18 字节时 `name` 数组无终止符，后续 `snprintf`/UI 显示越界读。
- 归属：hass_cfg 解析器。
- 修复方式：memcpy 前强制 `buf[nl]=0`。
- 当前状态：已修复。

## BUG-031（Baseline）sysmon 在关中断临界区内调用堆函数（潜在死锁）

- 现象：`heap_caps_*` 内部取堆互斥锁；若持锁任务恰在 tick 切换时被换下，关中断的 sysmon 无法再调度 → 死锁。
- 归属：sysmon。
- 修复方式：堆采样移出临界区，临界区只保护快照字段写入。
- 当前状态：已修复。

## BUG-032（Baseline）wifi_ap_fallback_stop 在事件任务中 abort

- 现象：`ESP_ERROR_CHECK(esp_wifi_set_mode(STA))` 在 GOT_IP 事件任务失败时直接 panic 重启整机。
- 修复方式：改为记录日志（模式切换时序敏感，下次断线流程自愈）。
- 当前状态：已修复。

## BUG-033（Baseline）wifi APSTA 切换与断线重连竞态（eb alloc fail panic 风险）

- 现象：`wifi_ap_fallback_start` 先 `esp_wifi_disconnect()` 再延迟 300ms 切
  `set_mode(APSTA)`；期间事件任务的 DISCONNECTED 回调可能再次 `esp_wifi_connect()`，
  与 set_mode 在闭源 WiFi 库内并发（实测 ieee80211_hostap_attach panic）。
- 修复方式：新增 `s_ap_switching` 门控 —— 切换窗口内 DISCONNECTED 事件跳过
  `esp_wifi_connect()`（推迟到 APSTA 布局完成后的 STA_START 事件或显式 connect）；
  三条回滚/成功路径统一清标志并恢复重连。
- 当前状态：已修复。防回退：DISCONNECTED 里的 connect 门控不可删除。

## BUG-034（Baseline）motor shake 脉冲执行期间无速度门控

- 现象：高速抑制只在 `start_shake` 启动时判一次；正/负脉冲期间用户急转，
  电机持续施加脉冲力矩，产生对抗手感。
- 修复方式：shake 状态机每周期检查 `|shaft_velocity| > 15 rad/s`，超限立即
  清状态并 `move(0)`，与启动时抑制语义一致。
- 当前状态：已修复。

## BUG-035（Baseline）MQTT 下行命令分片被解析成半截数据 / led 载荷无 hex 校验

- 现象：超长 payload 被 esp-mqtt 分片交付时只取首片，`atoi` 解析半截数据；
  `led` 命令长度 6 但含非 hex 字符时 `strtol` 静默置 0（黑灯）。
- 修复方式：DATA 事件 `data_len != total_data_len` 时丢弃并告警（短命令不应
  分片）；led 载荷逐字符 `isxdigit` 校验，非法时拒绝并告警。
- 当前状态：已修复。

## BUG-036（Baseline）app_state 环境快照四字段组合撕裂

- 现象：`app_state_get_env` 四字段无锁读取，可能取到"新 CO2 + 旧温湿度"的
  撕裂组合（单字段原子、组合非原子）。
- 修复方式：`set_env`/`get_env` 加轻量 portMUX 临界区（纯赋值，无阻塞调用）。
- 当前状态：已修复。

## BUG-037（信息）env_hist 单写多读无锁、display 熄屏双任务 check-then-act

- 两处均为低危（历史曲线瞬时错位 / 下次触摸自愈），记录不修：
  env_hist 读者为 UI/HTTP，撕裂只影响个别数据点；display 熄屏竞态的残留
  窗口会被下一次触摸/旋转的 notify_activity 自愈。

## BUG-038（Regression）进入系统监控偶发 IWDT panic 重启（二次修复：快照锁改 mutex）

- 现象：第一轮修复（IWDT 900ms + 重扫描降频 5s）后仍重启，但形态变化为
  `assert failed: spinlock_acquire (lock->count > 0 && lock->count < 0xFF)`
  —— `sample_once` 的 `portENTER_CRITICAL(&s_snap_mux)` 断言失败。
- 根因：FreeRTOS SMP spinlock 的递归计数在 `uxTaskGetSystemState`（挂起调度
  + 内部 kernel spinlock）与跨任务快照读取（LVGL 任务 `sysmon_get_snapshot`）
  交叉时损坏 —— spinlock 的 owner/递归语义不适合这种"重扫描挂起调度 + 多
  任务短读"场景。
- 修复方式：sysmon 快照保护从 spinlock（portENTER_CRITICAL）整体改为
  FreeRTOS mutex（`xSemaphoreCreateMutex`）—— 读者/写者均为任务上下文，
  持锁时间为结构体拷贝，无 ISR 使用；IWDT 900ms 保留作为
  `uxTaskGetSystemState` 自身关中断窗口的兜底。
- 当前状态：已修复（编译验证）。防回退：sysmon 快照保护禁止改回
  portENTER_CRITICAL spinlock。

## BUG-039（Baseline）SCD40 持续 not ready，自愈循环反复无效

- 现象：上电等待 1000ms 后 start_periodic 成功，但 `get_data_ready` 持续
  0x0000（一拍 0x8000 瞬态），60s 自愈循环反复 stop→reinit→start 拉不回。
- 分析：CRC 校验通过说明是传感器真实响应；固件侧时序已按 datasheet
  （上电 1000ms / stop 后 800ms / reinit 后 30ms）。持续性 0x0000 指向
  传感器硬件状态（焊接/供电/芯片劣化），需替换或换线验证。
- 修复方式：1) 自愈退避 —— 连续自愈失败 2 次后间隔 60s→180s（有数据即复位）；
  2) ready 判定回归 datasheet 精确位 bit11 (0x0800)（放宽 status!=0 属
  错误归因，BUG-040 修复后真因是上电时序）。
- 当前状态：已修复（v1.0.0-17 实测 CO2 正常上报）。

## BUG-043（Regression）ready 判定后立即读测量，I2C 命令间隔不足导致超时刷屏

- 现象：`read-meas tx/rx failed, err=0x103`（ESP_ERR_TIMEOUT）每 5~6 秒刷屏，
  但数据周期性正常上报（约每 3 拍成功一次）。
- 根因：`scd40_data_ready(0xE1B8)` 返回 ready 后**立即**发
  `read_measurement(0x0344)`——两次 I2C 命令间隔 <1ms，违反 Sensirion
  命令间隔要求，sensor 对第二个命令 NACK/超时。v1.0.0-16 偶发成功恰因
  前次失败的超时（200ms）充当了间隔。
- 修复方式：`scd40_task` 在判定 ready 与 read 之间加 `vTaskDelay(2ms)`。
- 当前状态：已修复（编译验证）。防回退：**任何 Sensirion 命令序列之间
  都要保证 >=1ms 间隔**，连续 I2C 命令连发是这类传感器的通用陷阱。

## BUG-040（Regression）越界红弧错位/显示不全 —— red_arc 的 bg_angles 钳制

- 现象：调节器(ADJUSTER)等模式下红弧出现在错误角度位置；其它有界模式
  红弧"很短一截"。
- 根因：`red_arc` 创建时 `lv_arc_set_bg_angles(0, 0)`（BUG-022 清零残留）。
  LVGL 把 indicator 弧钳制在 bg 弧范围内 —— bg=(0,0) 时红弧被裁剪或绕
  远路错位，与红弧应在的量程窗口位置无关。
- 归属：UI / pg_playground。
- 修复方式：`red_arc` 的 `bg_angles` 改为全周 (0, 360)（main 弧宽度 0 保持
  背景不可见，indicator 由此可画在任意位置）。
- 当前状态：已修复（编译验证）。防回退：**越界红弧的 bg_angles 必须全周**；
  任何"只在特定角度画弧"的 arc，其 bg_angles 必须覆盖全部可能的绘制区间
  （与 BUG-022 同源的第二因子）。

## BUG-042（Regression）sysmon 快照锁配对错误导致页面全零卡死

- 现象：进系统监控后所有数据 0%、页面无更新（LVGL 的 get_snapshot 永久阻塞）。
- 根因：重扫描降频改造时把两处 `portENTER_CRITICAL` 都替换成
  `xSemaphoreTake`，但 `give` 只保留一处 —— 每轮重扫描净泄漏一次锁，
  sysmon_task 永久持有 `s_snap_mux`，LVGL 任务的 `sysmon_get_snapshot`
  阻塞在 `portMAX_DELAY` 上，页面永远停留在创建时的初始值。
- 归属：sysmon（BUG-038 重构的引入错误）。
- 修复方式：重扫描路径的任务表增量计算段不加锁（`s_prev`/`s_states` 为
  sysmon_task 私有），锁只保护"发布 s_snap"的瞬间 —— take/gift 严格配对。
- 当前状态：**功能已整体移除**（2026-09，见 ARCHITECTURE.md §19）——
  BUG-031/038/042 三个 sysmon 专属问题随之归零；IWDT 回调 300ms 默认值
  （触发源已删）；FreeRTOS 运行时统计
  （GENERATE_RUN_TIME_STATS / RUN_TIME_STATS_USING_ESP_TIMER）一并关闭
  （CPU 百分比的唯一消费者就是 sysmon）。

## BUG-041（信息）表盘页眉尾文字与数字的跨页统一

- 现象：各表盘页的顶部标题、底部提示、中央数字位置不统一
  （pg_playground 标题 y=34 离表盘近、底部 -10 偏高、设置编辑数字未做
  视觉居中偏移）。
- 统一约定（本次落地）：
  - 顶部标题/模式名：`LV_ALIGN_TOP_MID, y=28`
  - 底部提示文字：`LV_ALIGN_BOTTOM_MID, y=-4`（统一低位）
  - 中央大数字：`LV_ALIGN_CENTER, y=+4`（48px 数字无下伸部的视觉居中补偿），
    附属单位标签随数字同步下移
- 涉及页面：pg_playground / pg_hass / pg_setting / pg_pcdial。
- 当前状态：已落地。新增表盘类页面必须套用同一套偏移。

---

# 当前静态分析发现

## CUR-001 当前 LVGL 仍实际使用 builtin 64 KB allocator（已修复）

- 历史：`sdkconfig` 里项目补丁 int 符号 `LV_USE_STDLIB_MALLOC=1` 与 LVGL 组件
  Kconfig choice（默认 `LV_USE_BUILTIN_MALLOC=y`）冲突，`lv_conf_kconfig.h`
  按 builtin 分支覆盖符号，实际生效 builtin 64KB 固定池。
- 修复方式：sdkconfig 中直接把 Memory Settings choice 切到
  `CONFIG_LV_USE_CLIB_MALLOC=y`（放弃 int 补丁符号，删除
  `main/Kconfig.projbuild` 的历史补丁段），配合既有
  `SPIRAM_MALLOC_ALWAYSINTERNAL=0` 大块分配落 PSRAM。
- 验证（静态全链）：sdkconfig → sdkconfig.h 仅存 `CONFIG_LV_USE_CLIB_MALLOC 1`
  → `lv_conf_kconfig.h` 走 CLIB 分支 → `LV_STDLIB_CLIB`；redefined 警告消失；
  `LV_MEM_SIZE_KILOBYTES` 已被 regen 清理。运行时行为待烧录后以页面叠加
  场景确认（64KB 池 OOM 死循环不再出现）。

## CUR-002 Web 环境历史时间轴间隔错误（已修复）

- 历史：`/api/envhist` 返回 `"iv_s":300`，日级缓冲实际 60 秒/点，浏览器时间轴错 5 倍。
- 当前状态：已修复；`webcfg.c` 返回 `iv_s:60`，前端 `ED.iv_s||60` 兜底与真实间隔一致。

## CUR-003 page pop 在动画 ready 回调中删除页面（已彻底修复）

- 证据：`pm_pop_anim_done()` 删除页面并释放页面数据。
- 2026-09 第一阶段缓解：`pm_delete_page` 顺序改为"先删对象树后释放数据"——`LV_EVENT_DELETE` 广播期间页面数据仍有效，页面回调（含 filter=ALL）可安全读取；回调业务操作由事件码拦截（BUG-025）。
- 2026-09 彻底修复：`pm_pop_anim_done` 不再在动画回调中删页，改为
  `lv_async_call(pm_pop_finish_cb, p)` 下一拍执行（回调返回时动画链表已完全
  处理完）；on_resume 与 knob_input_reset 一并移入 async 完成回调，删除完成
  后再恢复栈顶页面。`pm_replace` 的同步删除保留（startup 场景无用户交互）。
- 状态：已修复。防回退：pop 删除路径的 async 化不可回退；`ui_label_roll`
  幽灵标签同源教训（动画回调删对象破坏 LVGL 动画链表）见 ARCHITECTURE.md §10。

## CUR-004 设置页重新校准在 LVGL 回调中阻塞 600 ms（已修复）

- 当前状态：已修复；改为一次性 `lv_timer`（600ms）回调中 `esp_restart()`，提示先渲染，不再阻塞 UI。

## CUR-005 主菜单文案与模式数不一致（已解决）

- 历史：`pg_menu.c` 曾写"11 种手感模式"，而实际为 12；新增 `MOTOR_MODE_ADJUSTER` 后为 13。
- 当前状态：已解决；`pg_menu` 副标题已更新为 13，与 `motor_get_mode_count()` 一致。

## CUR-006 `README.md` 与当前代码不一致（已解决）

- 历史：README 曾写 PSRAM 未启用、SPI 20 MHz、11 种模式和旧依赖。
- 当前状态：已解决；README 已重写为开源版本（含 GPL-3.0、致谢合规标注、
  构建说明与 -IdfPath 用法），并随仓库发布。

## CUR-007 Motor 命令入队存在最多 20 ms 有界等待

- 证据：`post_cmd()` 使用 `xQueueSend(..., pdMS_TO_TICKS(20))`。
- 状态：仍存在。另注意：安全命令（急停）已改 `xQueueSendToFront` 插队（BUG-027），普通命令仍为 FIFO 尾部入队。

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
13. 不把 `CONFIG_LV_USE_STDLIB_MALLOC=1` 单独当成 CLIB 已生效的证据（该 int 符号已被删除，choice 才是生效开关——见 CUR-001）。
14. 不把代码存在或编译通过写成硬件已验证。
15. 不把 Web 恢复出厂描述成全 NVS 擦除；它当前只清 `webcfg`。
16. 环境历史当前使用固定间隔推时间，没有每个样本的绝对时间戳；新增 API 前不得假装已有真实时间戳。
17. BLE HID 报文一律纯数据，不带 Report ID 前缀（BUG-024）；dial TLC 为 Button1+Touch 双位。
18. HID 报文或 Report Map 变更后必须改 PnP PID 强制 Windows 重新枚举，并要求用户删设备重配对。
19. filter=ALL 的页面回调必须拦截 `LV_EVENT_DELETE`（BUG-025）。
20. "电机档位即值"页面读 `motor_get_position()` 后必须按量程钳制（BUG-026）。
21. 安全命令（急停）用 `xQueueSendToFront` 插队（BUG-027）。
22. esp_timer 回调访问共享资源用 try-take 锁，不阻塞（BUG-029）。
23. `pm_delete_page` 顺序保持"先删对象树后释放数据"，页面 destroy 不得访问 `p->root`。
24. 传感器自愈计数覆盖总线错误路径（BUG-028）。
25. pop 删除必须走 `lv_async_call` 异步化（CUR-003/BUG-025），禁止在动画 ready 回调直接删页。
26. LVGL 分配器只用组件 Kconfig 的 malloc choice（当前 CLIB=y）；禁止恢复 int 型
    `LV_USE_STDLIB_MALLOC` 补丁符号或 builtin 池（CUR-001）。
27. APSTA 切换窗口内的 DISCONNECTED 不执行 `esp_wifi_connect()`（BUG-033 门控）。
28. motor shake 状态机每周期执行速度门控，与启动抑制语义一致（BUG-034）。
29. MQTT 下行命令先校验分片与载荷合法性再解析（BUG-035）。
30. 跨任务快照的组合读（多字段）必须用临界区保护写入与读取（BUG-036）。
31. 引入 `uxTaskGetSystemState`/`vTaskGetRunTimeStats` 级别的重量级
    FreeRTOS API 前必须评估 IWDT 影响（BUG-038/042/移除决定的教训——
    sysmon 因它们整体移除）；替代方案：主循环日志 + 工厂测试页。
32. FreeRTOS 运行时统计（GENERATE_RUN_TIME_STATS 等）已关闭；重新开启前
    确认有真实消费者（此前唯一消费者 sysmon 已移除）。

---

# 硬件实测待办清单

| 项目 | 检查方法 | 当前结论 |
|---|---|---|
| 首次烧录 | `erase-flash` 后烧录 | 分区表已变更，待实机确认 |
| 触摸 IC | 开机 selftest + 工厂测试原始值 | 待实机确认是否存在并接线正确 |
| 触摸方向 | 四点校准和四角点击 | 待实机确认 |
| BLE 配对 | Windows/macOS 搜索 SmartKnob | 部分实测：删除重配对流程已验证 |
| S-Dial 切歌/音量 | 切歌（默认）/音量模式旋转 | 实测已可用（2026-09 实机） |
| S-Dial 滚轮 | 滚轮模式旋转（顺时针=向下） | 已修复方向，待复测 |
| S-Dial 表盘 | 表盘模式旋转/系统菜单/重配对生效 | 旋转已可用；长按菜单待复测 |
| 播放暂停 | 中央短按（表盘/切歌/音量/滚轮模式） | 待复测（表盘短按补发媒体键为新行为） |
| 菜单聚焦动画 | 主菜单旋转和触摸滑动 | 待实机确认 |
| Motor 闭环/力反馈 | `pg_playground` 13 模式 | 待实机确认 |
| WiFi/AP 配网 | STA 连接、90 秒回退、192.168.4.1 | 待实机确认 |
| MQTT/HA Discovery | sensor + device_automation + level 通道 | 待实机确认 |
| Web/OTA | `/status`、`/api/envhist`（60s 间隔）、`/ota` | 待实机确认 |
| LED | WiFi/MQTT 状态色和工厂测试 | 待实机确认 |
| 长时间稳定性 | 连续运行、heap poisoning 日志、WDT | 待实机确认 |

---

# 历史结论与当前事实的区别

- “代码实现”不等于“硬件已经验证”。
- “编译通过”不等于“运行稳定”。
- “移除了某个触发条件”不等于“根因架构已经完成”。
- “配置项存在于 `sdkconfig`”不等于“该配置最终生效”。
- “任务或页面存在”不等于“实际硬件已经完成回归测试”。
