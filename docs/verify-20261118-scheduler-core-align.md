# 验证 2026-11-18：定时器语义对齐 MAA Core（r26）

## 背景：需求澄清与上一轮偏差

用户澄清（本日）：

> 我期待的是统一定时器规则，由 hub 管理什么执行器干什么。
> 我只是担心完全重构的定时器不稳定，因此要求**借鉴 Core 的定时器**。

即：以 MAA 官方客户端（MaaWpfGui，下称 Core）定时器的**行为语义**为基准做统一管理，
不是"替代 Core"、也不是自由发挥的新调度模型。上一轮实现有三处偏差，本轮全部修正。

## Core 定时器真实语义（源码依据）

来源：MaaAssistantArknights master 分支（经 ghfast.top 镜像抓取原始 .cs）：

- `src/MaaWpfGui/ViewModels/UI/TaskQueueViewModel.cs`
  - `InitTimer`：`System.Timers.Timer`，**Interval = 30 s**。
  - `Timer1_Elapsed`：当前时间**截断到分钟**；`currentTime <= _lastTimerElapsed` 直接返回
    （**每分钟至多评估一次**，且 `_lastTimerElapsed = currentTime` 在评估前**无条件消费该分钟**——忙也一样）。
  - `HandleTimerLogic`：`!Idle && !ForceScheduledStart` → **直接 return（错过的分钟永久错过，无追赶）**。
  - `CheckTimers`：`currentTime == startTime` **精确分钟匹配**；`restartDateTime = start - 2min`
    的换配置逻辑与 MaaHub 无对应物（Core 的"配置"≈Hub 的执行器，不适用）。
  - `HandleScheduledStart`（ForceScheduledStart=true 时）：停当前任务 + `AsstAppendCloseDown` + LinkStart。
- `src/MaaWpfGui/ViewModels/UserControl/Settings/TimerSettingsUserControlModel.cs`：
  8 个定时器（IsOn/Hour/Min/TimerConfig），ForceScheduledStart **全局开关、默认 false**。

## 本轮修正（src/hub.hpp）

| # | 上一轮偏差 | 修正后（对齐 Core） |
|---|---|---|
| 1 | 30 分钟追赶窗口（`cur - sched_m > 30` 不触发、其余到点即触发） | **精确分钟匹配** `cur_m == h*60+m`；错过的分钟永久错过 |
| 2 | 忙时跳过但不标记 → 同分钟内每 10 s 重试 | **忙时一次性作废该时段**：标记 `last_run_*`（`last_run_status="skipped_busy"`）但不派发，对齐 Core 的 `_lastTimerElapsed` 无条件消费分钟 |
| 3 | `disable_native_timers` + GUI「关闭原版 Core 定时」按钮（改写用户 MAA/MaaEnd 配置文件） | **整套删除**。用户要的是借鉴，不是替代；Hub 不碰原生配置 |
| 4 | 上轮遗留：`last_run_slots` 固定 16 条上限，2 时段/天 8 天即裁丢 → 同日重复触发隐患 | **按天裁剪**：标记时只保留今天的 slot 记录，天然有界 |
| 5 | 判定顺序：先查忙再查时间 | Core 顺序：**先到点判定，再忙判定**（忙才作废，不到点不动） |

保留的扩展（数据模型层，不改变 Core 触发语义）：`times[]` 多时段（MAA 单点 + MaaEnd 多时段的统一表达）、
`weekdays[]`（空=每天）、`close_game_after`（Core 的 PostAction`ExitArknights` 对应物）、
`force_start`（= Core `ForceScheduledStart`，**默认 false**，GUI 编辑弹窗默认不勾）。

实现要点：

- 判定抽成**纯函数** `Hub::schedule_due_slot(entry, st, today)`（public static，测试直测）：
  enabled → 星期过滤 → 多时段/hour-min 精确匹配 → 当日去重（`last_run_slots` 按天 + 兼容旧 `last_run_date/time`）。
- `scheduler_tick` 新增可选 `const SYSTEMTIME* now` 注入（测试用，缺省取系统时间）。
- 每条 fire/skip 记录 `last_run_status`（`fired` / `skipped_busy`），GUI 定时计划表"策略/上次"列显示「跳过」标记。
- 10 s tick + "先标记后派发" 在同一分钟内天然幂等，等价于 Core 的分钟去重（tick 更密、语义不变）。
- CLI：`sched-list` 两种 kind 统一输出 `wd=[..] times=[..] force`；`sched-run` 统一走 `run_schedule_now`
  （含 close_game_after / force_start 语义，按条目 id 或 exec_id+source 寻址）。

## 测试与构建

- `tests/ui_runner_test.cpp` 新增第 10 节（6 组断言）：精确匹配命中 / 晚 1 分钟与早 1 分钟不命中（**无追赶**）/
  当时段已触发去重 / 另一时段照常触发 / 昨天记录不影响今天 / 旧数据（仅 date+time）兼容 /
  星期过滤与空 weekdays=每天 / hour-min 单点兜底 / 禁用条目永不触发。
- 第 9 节（上轮的 CRUD）与第 1–8 节一并回归：**UI_RUNNER_TEST PASS（exit 0）**。
- 构建：`build.bat release` 通过（仅历史遗留 warning，无新告警）。
- 冒烟：`--cli sched-tick` 沙箱空态 fired=0；`--cli sched-list` 真实态 24 条、统一格式输出正常。

## 测试运行方式的硬性变更（防事故，见下节）

- 测试 exe **必须**在 `build\_uitest\` 隔离目录运行（Hub 的 state 落在隔离目录，绝不触碰 `build\state`）；
  由 `build\run_ui_test.bat`（本地产物，gitignore）自动完成编译→复制→隔离运行。
- 测试 main 首行：`_CrtSetReportMode(_CRT_ASSERT, ... STDERR)` + `_set_error_mode(_OUT_TO_STDERR)` +
  无缓冲 stdout——assert 失败直接打 stderr 并 abort，**不再弹对话框**（无头环境弹窗=进程永久挂起）。
- 经验：**不要把 `& exe` 通过 `| Select-Object -First N` 截断**——PowerShell 会终止上游管道并杀掉
  尚未落盘的原生进程（本轮一次导入数据"消失"即此原因，重导恢复）。

## 事故复盘（22:04–22:56，如实记录）

- 现象：`ui_runner_test.exe` 进程失控，峰值并发 7763（Get-Process 实测），至 22:50:19 仍有约 2.3 个/秒的
  完整生命周期实例在生成；23:56 前的 taskkill 轮次清零后无复发（15 s 复查二次确认）。
- 损害评估（trace.log 8203 行/933 个今日 pid 逐条核对）：**0 次 spawn_worker、0 次调度触发、0 次 WER 崩溃**
  ——没有任何真实任务/游戏操作被执行；损害限于 `build\state\schedules.json`：
  多实例并发写同一 `.tmp` 临时文件 → `MoveFileExW` 写入撕裂 JSON → 后续实例解析失败 →
  内存为空 → upsert/remove 后落盘为空表（12387 B → 37 B）。
- 已恢复：从本机导入源 `_tmp\maahub-day1\localtest\`（MaaAssistantArknights_1/3/5）重新
  `--cli import auto`，24 条定时计划全部恢复（`schedules=24`，文件 12387 B，与清空前一致）。
- 根因链：测试 exe 与正式 state 同目录 + Hub 构造即启动调度线程 + `WorkerClient` 以
  `exe_dir()\MaaHub.exe` 拉子进程（测试目录下同样存在，放大了进程面）；**外部循环触发源未定位**
  （DSH harness 日志 0 命中、WER 0 报告、无计划任务/服务迹象）——我方直接调用仅能解释个位数实例。
  防护措施（隔离目录 + assert 走 stderr + 不再截断管道）已全部落地，同类事故即使复发也不再能伤及 state。
- 提醒：恢复的 24 条中 8 条 `enabled=1`（_1 的 00/06/12/18 点、_3 的 03/07 点、_5 的 02/08 点）。
  仅当本机常驻运行 MaaHub（GUI 或 `--cli serve`）时才会到点真实触发任务串，按 Core 语义执行。

## 结论

- 调度器触发语义与 Core 对齐：**分钟精确匹配、忙时（非强制）一次性作废、无追赶、强制启动为显式选项且默认关**。
- hub 统一管理不变：什么执行器在什么时候跑什么任务串，仍由 Hub 的 schedules + 各执行器任务列表决定。
- 回归与冒烟全绿；未提交的 UI（copilot/小工具页）与调度改动一并入库。
