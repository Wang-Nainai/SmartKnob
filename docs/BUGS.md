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

## BUG-009（Baseline，触摸失灵的代码级根因——已修正结论）XPT2046 轴向映射

- 现象：屏幕"完全无法触控"。
- 调查结论（依据店家 `XPT2406.C` 实测代码 + 原理图）：
  - 店家对该屏用 `0xD0` 读横轴(X)、`0x90` 读纵轴(Y)，与数据手册字面定义
    （0x90=A[001]"X-Position"）相反——**轴向取决于面板玻璃接线，必须以
    模块实测为准**。此前判断 atanisoft"标反"过于武断，它实际与店家一致。
  - 已改为自研裸驱动并采用**店家轴向约定**（0xD0=X / 0x90=Y），
    另有 `TOUCH_SWAP_XY/MIRROR_X/MIRROR_Y` 可在上机后一键纠正。
  - 采纳店家滤波策略（采 5 次排序去极值取均值）+ 双读一致性校验（防噪声）。
- 归属模块：display。
- 状态：✅ 已修复（自研驱动替换第三方组件，固件 -4.5KB）。
- 修复阶段：触摸专项。

## BUG-010（Baseline，待硬件确认）触摸接线 / 模块版本存疑

- 关键发现（GMT240_18P 原理图）：该模块原理图中 **T_CLK/T_CS/T_DIN/T_DO/T_IRQ
  （引脚 10-14）全部标注 NC**——对应"**不带触摸**"版本；电阻触摸排线
  （XL/XR/YU/YD）直接由屏 FPC 引出，需板载 XPT2046 才有触摸功能。
- 待确认：模块背面丝印区是否有 16 脚触摸 IC（XPT2046/TSC2046/GTxxx）。
- 状态：⏳ 待硬件确认——已内置两处诊断：
  1. 开机日志 `XPT2046 selftest: z1=.. z2=.. x=.. y=..`（未按下应 z1≈0, z2≈4095；
     若恒 0/4095 且按下不变 → 接线问题或模块无触摸 IC）
  2. 工厂测试→触摸测试：实时显示 z1/z2/raw_x/raw_y（按下时 z1 应明显增大）

## BUG-011（Regression，自检发现并修复）pg_hass 表盘句柄未保存

- 现象（潜在）：HASS 控制视图指针更新用了错误的容器对象。
- 原因：重构时 `lv_scale_create` 返回的局部变量未存入页面结构体。
- 归属模块：ui。
- 状态：✅ 已修复（d->scale 字段）。
- 修复阶段：UI 重构（X-Knob 风格）。

## BUG-012（Baseline）分区表缺少 otadata，OTA 从未真正可用

- 现象：Web OTA 烧写"成功"，但重启后仍运行旧固件。
- 原因：partitions.csv 缺少 `otadata` 分区，`esp_ota_set_boot_partition()`
  无法持久化启动选择 → OTA 写入成功但永远不切换启动分区。
- 归属模块：分区表。
- 状态：✅ 已修复（新增 otadata 0x2000；应用分区同步扩至 3MB×3 以容纳 BLE）。
  **注意：分区表变更，首次烧录建议 `idf.py erase-flash` 一次。**
- 修复阶段：S-Dial/BLE 专项。

## BUG-013（Baseline，整体校验修正）触摸轴向结论修正（并入 BUG-009）

- 修正：参考店家 `XPT2406.C` 实测代码后确认该屏 `0xD0=横轴(X)`、`0x90=纵轴(Y)`，
  自研驱动已改为店家约定；SWAP/MIRROR Kconfig 可在上机后微调。
  另：GMT240 18P 原理图显示 T_* 引脚 NC（"不带触摸"版本），
  用户模块是否有触摸 IC 待上机确认（开机 selftest + 工厂测试可判）。
- 状态：✅ 已按店家约定实现。

## BUG-014（Regression，整体校验发现）触摸总线异常时幽灵连点

- 现象（潜在）：若触摸芯片缺失/MISO 悬空，读数恒 0 或 0xFFF，
  `pressure = z1+4095-z2` 恒为 4095 → UI 收到持续幽灵触摸疯狂误点。
- 归属模块：display。
- 状态：✅ 已修复（z1/z2 双高/双低判总线无效 + 原始坐标 50~4045 有效性窗口）。
- 修复阶段：整体校验。

## BUG-015（Regression，整体校验发现）返回菜单后电机手感模式不恢复

- 现象（潜在）：从手感页返回主菜单，电机仍停留在子页设置的模式
  （如 1° 精细档），菜单浏览手感错乱。
- 原因：页面只在 create 时设置电机模式，返回时不恢复。
- 归属模块：ui 框架。
- 状态：✅ 已修复（page_ops 新增 `on_resume` 生命周期，pop 动画结束后调用
  新栈顶页的 on_resume 重新声明电机模式，并同步旋钮输入基准）。
- 修复阶段：整体校验。

## BUG-016（Baseline，整体校验发现）BLE 广播外观字段缺失标志

- 现象（潜在）：广播中外观字段未携带（该 NimBLE 版本需要
  `appearance_is_present` 标志）。
- 归属模块：blehid。
- 状态：✅ 已修复。
- 修复阶段：整体校验。

## BUG-017（Baseline，二次校验发现）UI 改版后中文字库缺字 38 个

- 现象（必然）：X-Knob 风格 UI 改版新增大量文案（点击/切换/工厂测试/蓝牙/
  滚轮/播放/音量/搜索/保存/屏幕/上一首…），自研字库 lv_font_msyh_16 仅含
  旧文案 121 字形（且完全不含 ASCII），新页面文字大面积空白。
- 另发现：pg_env 温度 ° 符号在此前转义修复时丢失 UTF-8 前导字节 0xC2。
- 归属模块：ui / fonts。
- 状态：✅ 已修复——
  1. 新增 `tools/gen_msyh_font.py`：自动扫描全部页面文案生成字库
     （C:/Windows/Fonts/msyh.ttc → PIL 渲染 → LVGL FMT_TXT 4bpp），
     字库与 UI 永远同步，文案改动后重跑脚本即可；
  2. 生成 215 字形（117 CJK + 全部 ASCII + ° ·），覆盖校验脚本零缺失；
  3. 度量与旧字库一致（line_height=22/base_line=5，经 LVGL
     `ofs_y = ascent - bbox_top - box_h` 公式核对）；
  4. ° 符号补回 \xC2\xB0 完整 UTF-8。
- 校验脚本：临时 `check_glyphs.py` 比对 cmap unicode_list 与源码用字。
- 修复阶段：二次整体校验。

## BUG-018（Regression，二次校验发现）lv_scale 默认可点击吞掉页面级点击

- 现象（必然）：LVGL 对象默认带 CLICKABLE 标志（label/image 例外），
  `lv_scale` 未显式移除 → 手感页点击表盘无法切换模式、家居控制页点击表盘
  不发 ON/OFF、设置编辑页点击表盘不保存。
- 归属模块：ui。
- 状态：✅ 已修复（三处 scale 显式 `lv_obj_remove_flag(CLICKABLE)`）。
- 修复阶段：二次整体校验。

## BUG-019（Regression，二次校验发现）浮层位于 flex 滚动容器内导致错位

- 现象（必然）：家居控制视图/设置编辑视图是 flex+pad 滚动根的子对象，
  显示时被放到内容流末尾（y=125/50 处）且随滚动偏移。
- 归属模块：ui。
- 状态：✅ 已修复（重构为 根→列表滚动容器 + 根→全屏浮层 结构，
  浮层不透明直接覆盖列表，无需逐行隐藏）。
- 修复阶段：二次整体校验。

## BUG-020（Regression，二次校验发现）触摸测试浮层超出页面产生滚动

- 现象（潜在）：触摸测试视图 y=22 高 320（底 342>320），页面可被拖滚 22px。
- 归属模块：ui。
- 状态：✅ 已修复（高 298；非列表页根容器统一禁用滚动）。
- 修复阶段：二次整体校验。

---

## 硬件实测待办清单（供检查）

| 项 | 方法 | 预期 |
|---|---|---|
| **首次烧录** | `idf.py erase-flash` 后再 flash | 分区表已变更（+otadata、3MB 分区） |
| **触摸接线/IC** | 开机 selftest 日志 + 工厂测试→触摸测试 | 按下时 z1 明显增大；恒 0/4095 → 接线或模块无触摸 IC |
| **触摸方向** | 触摸测试点四角 | 与手指一致；不一致改 `TOUCH_SWAP_XY/MIRROR_X/MIRROR_Y` |
| **BLE 配对** | 电脑蓝牙搜索 "SmartKnob" | 可配对、断线自动重连、重启免配对 |
| **S-Dial 音量** | 进 S-Dial 旋转 | 音量增减；状态栏蓝牙图标变绿 |
| **S-Dial 滚轮** | 切模式后旋转 | 浏览器/文件列表滚动 |
| **S-Dial 媒体** | 点中心/上下曲 | 播放暂停、切歌 |
| 菜单聚焦动画 | 旋转 | 图标列收窄+红边+回弹动画，滚动跟随 |
| 电机闭环/力反馈 | 手感页 | 11 模式正常 |
| WiFi/MQTT/LED | 状态栏 + LED | 绿/青点亮 |
| **OTA 真实验证** | 网页上传当前固件 bin | 重启后运行新固件（BUG-012 修复验证） |
| 稳定性 | 长时间运行 | 无 WDT/Panic/卡死 |

## 回归记录（触摸专项 + UI 重构）

- 编译：✅ 全量通过，零 warning
- 依赖：✅ atanisoft/esp_lcd_touch 已移除并自动清理（固件 0x1a7400，17% 空闲）
- 引用检查：✅ 无 esp_lcd_touch 残留
- 竞态检查：✅ 触摸轮询仅发生在 LVGL 任务内（indev 回调），无跨任务 SPI 访问
- 实时性：✅ LVGL 钉 core0（5ms 最小唤醒），motor 独占 core1