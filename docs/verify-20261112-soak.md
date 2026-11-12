# MaaHub 浸泡终检报告（完全替代 MAA5 跑一天）

- 检查时间：2026-11-12 11:44（+08），证据采集脚本 `soak_final.ps1` / `soak_final2.ps1` / `soak_final3.ps1`，原始输出存 `D:\WorkSpace\_tmp\maahub-day1\evidence\soak-final*-20261112.txt`
- 浸泡窗口：11-11 03:46 部署新版本起算；常驻 Hub 进程实际从 **11-12 00:29:03** 连续存活（00:29 有一次重启，见 §5）
- 版本：`build\MaaHub.exe`（11-11 03:46 构建，含调度器/串行链修复）

## 1. 结论速览

| 维度 | 结果 |
|---|---|
| 进程稳定性 | ✅ 主进程 + 6 worker 连续 11h15m 无崩溃，watchdog 每 5 分钟 OK（00:29→11:49 无间断） |
| 调度器准确性 | ✅ 02:00 / 06:00 / 08:00 三个触发点全部准点 fire，trace `[sched] exec -> status` 与 schedules.json `last_run_date` 一致 |
| 链完整性 | ✅ Maa5 两次链 6 任务全部 TaskChainCompleted；Maa1 06:00 链 Award 失败（游戏侧弹窗，非 Hub 回归） |
| 资源占用 | ✅ 主进程工作集 35.2MB；worker 各 3-6MB；CPU 累计 5539s/11.3h ≈ 13.6%（OCR 识别正常水位） |
| **配置忠实性** | ❌ **不达标**：maa1/maa5 执行器 task-params 为空、未绑定账号 → 链跑的是 fill_defaults 默认参数，与原 MAA 客户端配置差异大（见 §4）→ 待 TaskQueue 导入版部署修复 |

**判定：Hub 作为"调度+执行框架"跑满一天合格；作为 MAA5 的"行为替代"还差忠实配置导入，代码已就绪（本轮 TaskQueue 导入 + 6 项修复，编译通过 + 本机导入实测通过），待部署窗口上线。**

## 2. 今日触发时间线（trace `[sched]` + asst.log 链事件对齐）

| 时刻 | 执行器 | 触发 | 结果 | 链明细（asst.log 逐链 TaskChainCompleted） |
|---|---|---|---|---|
| 00:37 | exec-02913292d1eb（MaaEnd 探针，周四） | fire | **failed** | AutoEcoFarm 即挂——Endfield 客户端未运行（已知 RCA，`verify-20261112-maaend-scheduled.md` 已闭环，与 maa 链无关） |
| 02:00:10 | exec-5fa8dd2145ee（Maa5） | fire | **done** | StartUp 2.2s → Award 13.4s → Fight 21m23s → Infrast 19.2s → Mall 65.4s → Recruit 11.1s，6/6 Completed |
| 06:00:05 | exec-84691d0fba02（Maa1） | fire | **failed** | StartUp 2.2s ✅ → Award 06:00:08→06:00:40 TaskChainError（20 次 ProcessTask 耗尽）；截图 `2026.11.12-06.00.40.716_raw.png` = 游戏停在「今日配给」登录奖励弹窗 → 游戏侧瞬态，RCA 已闭环 |
| 08:00:05 | exec-5fa8dd2145ee（Maa5） | fire | **done** | StartUp 20.1s（冷启动）→ Award 9.9s → Fight 17m53s → Infrast 24.7s → Mall 85.0s → Recruit 10.4s，6/6 Completed；Fight 结算 1-7 代理 72 次（state.json stage_drops） |

- 昨日参照：11-11 14:10 手动链 done、18:25-19:12 账号切换测试链 done；11-11 02:00 failed（旧版，已修）。
- 调度器行为：lrd 先标记后 fire（防重入）✅；maaend 忙跳过且不标记 ✅；00:37 探针与 02:00 Maa5 间隔 1h23m 无并发冲突（同模拟器串行）。

## 3. 调度状态核对（schedules.json，17 条）

| exec | source | 时刻 | enabled | lrd（终检时） | 判定 |
|---|---|---|---|---|---|
| exec-5fa8dd2145ee | _5:Timer.Timer1 | 02:00 | T | 2026-11-12 | 今日已触发 ✅ |
| exec-5fa8dd2145ee | _5:Timer.Timer2 | 08:00 | T | 2026-11-12 | **今日已触发 ✅（08:00 待办闭环）** |
| exec-84691d0fba02 | (无):Timer.Timer2 | 06:00 | T | 2026-11-12 | 今日已触发（failed 见 §2） |
| exec-02913292d1eb | probe:scheduled | 周四 00:37 | T | 2026-11-12 | 探针，11-19 前删除 |
| 其余 13 条 | — | — | F | "" | 未启用，正确不触发 ✅ |

trace 中今日 `[sched]` 行共 4 条 fire + 1 条 scheduler_loop start，与上表一一对应；无漏触发、无误触发。

## 4. 与 MAA5 时代的行为差异（忠实性缺口清单）

远端 `task-params.json` 显示 **exec-84691d0fba02 / exec-5fa8dd2145ee 无任何参数**，执行时全部走 adapter 默认值；`executors.json` 显示两执行器 `account_id=""`。对照原 MAA 客户端配置（TaskQueue 权威快照）：

| 项 | 原 MAA（maa1/maa5） | Hub 当前实际 | 影响 |
|---|---|---|---|
| 账号切换 | AccountSwitchEnabled=T，切 180\*\*\*\*3658 / 152\*\*\*\*7662 | 不切换，用客户端当前账号 | **最大缺口**：链在错误账号上跑 |
| Award | mail/免费单抽/合成玉/月卡 全 T | 默认 award=T, **mail=F, recruit=F** | 邮件/单抽漏领 |
| Recruit | maa1 MaxTimes=300 / maa3/5=40，confirm L3-L5 | 默认 times=0 | **只计算不执行**（08:00 链 Recruit 仅 10.4s 佐证） |
| Infrast | drones=PureGold(M)/Money(5)，8 设施全换，宿舍阈值 20 | 默认 drones=_NotUse、**facility=[]** | 无人机不用、设施不换（19-25s 完成佐证） |
| Mall | 优先购「招聘许可」/黑名单「加急许可」 | 默认空数组 | 无过滤乱买 |
| Fight | maa3/5 药 5 个；maa1 不吃药 | 默认 medicine=0 | maa3/5 少刷（maa1 恰好一致） |
| 链顺序 | StartUp→Recruit→Infrast→Fight→Mall→Award | StartUp→Award→Fight→…（字典序仅提 StartUp） | Award 提前，语义偏差 |
| 定时器 | maa1 本地 0:00/6:00/12:00/18:00 四个启用 | 仅 06:00 一条 | 12:00/18:00/0:00 未跑（maa1 浸泡期只触发过一次） |

以上全部由本轮已实现并本机验证的 **TaskQueue 导入 + canonical_task_order 全序 + 账号绑定** 修复；导入实测：maa1/3/5 三份 gui.new.json → 6 任务 36 参数/份，drones_idx=5(PureGold)/1(Money)、confirm 数组、Mall 中文串、Fight stage/medicine、Award 7 键逐项与权威快照吻合；normalize 探针输出 `drones:"PureGold"`、`client_type:"Official"`、`blacklist:["加急许可"]`、链序 `StartUp Recruit Infrast Fight Mall Award` 全部正确。

## 5. 运行环境与异常记录

- 进程：MaaHub ×7（主 241840 + worker 6）自 00:29:03 存活；MuMuVMMHeadless=1、MuMuNxDevice=1（无头模式）；宿主无 Arknights 进程（游戏在模拟器内，正常）。
- **原始 MAA 客户端（MAA.exe ×3 套安装）当前均未运行**，Run 键/计划任务/启动文件夹无自启项——"关停原客户端"目标实际已达成，部署后保持即可。
- 00:29 重启原因：11-11 深夜 probe 系列操作所致（非崩溃）；watchdog.log 无 restart 记录佐证。
- 文件：trace.log 869KB/10690 行；asst.log maa5 4.5MB / maa1 1.9MB（按执行器分目录，ENG01 共享 cache 问题不复现）；今日失败截图 1 张（06:00）。
- 遗留清理项不变：3 个 MaaHubProbe* 计划任务、探针执行器 exec-02913292d1eb（11-19 前）、`.bak-probe7-20261112.json`。

## 6. 下一步（部署窗口）

**已于当日 12:55 提前完成**（08:20–18:00 空窗安全，且让 18:00 maa1 成为忠实配置首个实弹验证点）：
停 Hub → 换新版 exe → CLI 导入 maa1/maa3/maa5 三份配置 → 建 ak152/ak151 并三执行器绑定账号 →
统一 MuMu adb connect 模板 → 重启（新主 pid=253064）→ 看门狗恢复。全程与核验详见
[verify-20261112-deploy.md](verify-20261112-deploy.md)。
