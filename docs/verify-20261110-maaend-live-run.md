# MaaEnd「全套日常」仿人实跑验收（2026-11-10）

> 通过 MaaHub GUI 真实操作 MaaEnd 预设 `DailyFull`（全套日常），在已登录的《明日方舟：终末地》主界面上完整跑一轮。
> 结论：**跑通并自然收尾**（CloseGamePC 关闭游戏）。核心卡点不是移植缺陷，而是 **Windows UIPI 完整性级别不匹配**。

## 1. 结论摘要

| 项 | 结果 |
|---|---|
| GUI 完整链路（选预设 → 连接 → Link Start） | **PASS** |
| MaaEnd 真实操作游戏（点击/进菜单/切图） | **PASS**（游戏内产生实际收益） |
| 16 个日常任务全部下发 | **PASS**（15 个 task_id 实际执行） |
| AutoEcoFarm（生态农场）三个子任务 | **FAIL**（`AutoEcoFarmWulingCityFarmFailed` / `SisPaintingFarmFailed` / `RanchFailed`，20:25:38） |
| CloseGamePC（关游戏） | **PASS**（20:59:14，游戏进程消失，只剩启动器） |
| 总耗时 | **38 分钟**（20:20:36 → 20:59:14） |

## 2. 环境

| 项 | 值 |
|---|---|
| 机器 | ENG01（DESKTOP-P72CMPJ），Win10，1920×1080 |
| 游戏 | `Endfield.exe` pid 237620，`UnityWndClass` hwnd 6031996，client 1280×768 |
| 游戏版本 | `CN_WIN_REL_1.6.3_C10781649_ET0704668`，UID `1439188325` |
| 游戏完整性 | **高（elevated）** —— 从启动器以管理员拉起 |
| MaaHub | `build\MaaHub.exe`，**以 `/RL HIGHEST` 启动（elevated）** |
| MaaEnd 核心 | `maaend-v5.15.0-beta.1` |
| 预设 | `tasks\preset\DailyFull.json`（16 项） |

## 3. 根因：UIPI 完整性级别不匹配

这是本次实跑最重要的发现。

### 3.1 现象对比

| 轮次 | MaaHub 权限 | 游戏权限 | 结果 |
|---|---|---|---|
| 第 1 轮（19:43–20:08） | 普通（medium） | elevated | 每个任务 32s 超时跳过；**农场奖励弹窗关不掉**，后续任务连续 Failed |
| 第 2 轮（20:20–20:59） | **elevated** | elevated | 全程正常操作，**弹窗能被自动关闭**，38 分钟跑完并关闭游戏 |

### 3.2 机制

- 游戏进程是高完整性（elevated）。
- 普通完整性进程向高完整性窗口 `PostMessage`/`SendInput` 会被 **UIPI（User Interface Privilege Isolation）** 静默丢弃，返回值仍是成功。
- MaaEnd 的 Win32 controller 走的是窗口消息注入 → 被 UIPI 拦 → 表现为「识别得到画面、点击无效果、节点超时」。
- 高完整性进程操作低完整性窗口是允许的，反向才被拦。

### 3.3 验证证据（七轮输入注入实验，摘要）

| 实验 | 结果 |
|---|---|
| 普通进程 `SetCursorPos`/`SendInput`（游戏前台） | 全部返回 False |
| 普通进程 `PostMessage`（鼠标 + 键盘，游戏） | 完全无效（Unity 走 Raw Input，不读 Win32 鼠标消息） |
| 普通进程 `BlockInput(FALSE)` | **err=5 ERROR_ACCESS_DENIED**（需管理员） |
| `schtasks /RL HIGHEST` 提权后 | `elevated=True`，`SetCursorPos(960,540)=True`、`ClickAbs ok=True` → **注入成功** |

### 3.4 对产品的含义

MaaHub 若要驱动以管理员身份运行的终末地（默认从管理员启动器拉起时就是），**MaaHub 自身必须以管理员权限运行**；或在 UI 上显式提示「检测到目标窗口完整性高于自身，请以管理员重启」。

## 4. 执行时间线（第 2 轮）

| 时间 | 事件 |
|---|---|
| 20:18:01 | 以 `/RL HIGHEST` 重启 MaaHub（pid 236492） |
| 20:19:xx | GUI 选「全套日常」，状态 `未连接 · 核心 v5.15.0-beta.1` |
| 20:20:0x | 点「连接」→ 游戏被前置（controller 拿到控制权） |
| 20:20:36 | 点 `Link Start!` → **16 个任务全部 post**（trace.log 时间戳 1794313236.8） |
| 20:20:37 | task 200000001 开始（OutpostTradingSchedule） |
| 20:20:37 | `AutoDeliveryEnsureDeliveryMissionSelected`（快递） |
| 20:21:18 | `SceneEnterMenuRegionalDevelopment`（区域发展） |
| 20:22:21 | `SceneAnyEnterWorld`（进世界） |
| 20:22:23 | `__AutoEcoFarmWulingCityFarmExecute`（吴陵城农场） |
| 20:23:23 | `SceneEnterMapWulingSisPainting`（Sis 绘画农场） |
| 20:24:31 | `__AutoEcoFarmRanchExecute`（牧场） |
| **20:25:38** | **三个农场子任务 Failed**（奖励弹窗阻塞） |
| 20:32:50 | `StashBackpackSubTask`（背包整理） |
| 20:46:01 | `TrialOfSwordmancyEnterTrialMenu`（试剑） |
| **20:59:14** | **`CloseGamePC` 执行，`ret=true`** |
| 20:59:15 | 日志终止，任务队列结束 |
| 21:18 | 桌面只剩「终末地」启动器，`Endfield.exe` 不存在 |

## 5. task_id 执行清单

从 `maafw.log` 统计到 **15 个 distinct task_id**：

```
200000001  200000002  200000004  200000005  200000006  200000007  200000008
200000009  200000010  200000011  200000012  200000013  200000014  200000016
200000018
```

（003/015/017 未出现，推测为内部合并或跳过节点，非丢失。）

trace.log 中 16 条 post 的 entry 映射：

| # | task | entry |
|---|---|---|
| 1 | SellProduct | `OutpostTradingSchedule` |
| 2 | SeizeDeliveryJobs | `SeizeDeliveryJobsCheckOngoingJob` |
| 3 | DeliveryJobs | `DeliveryJobsMain` |
| 4 | AutoStockpile | `AutoStockpileMain` |
| 5 | AutoSell | `AutoSellMain` |
| 6 | EnvironmentMonitoring | `EnvironmentMonitoringMain` |
| 7 | ResourceRecycleStation | `ResourceRecycleStationStart` |
| 8 | AutoEcoFarm | `AutoEcoFarmTask` |
| 9 | ProtocolSpace | `ProtocolSpaceSchedule` |
| 10 | DijiangRewards | `DijiangRewards` |
| 11 | VisitFriends | `VisitFriendsMain` |
| 12 | GiftOperator | `GiftOperatorMain` |
| 13 | DailyRewards | `DailyRewardStart` |
| 14 | TrialOfSwordmancy | `TrialOfSwordmancyMain` |
| 15 | CreditShoppingN2 | `CreditShoppingMain` |
| 16 | CloseGamePC | `CloseGamePC` |

## 6. 证据文件

| 文件 | 内容 |
|---|---|
| `_tmp/maahub-import/hub_run1.png` | Link Start 后游戏被前置接管 |
| `_tmp/maahub-import/now6.png` | **农场采集奖励实锤**（高级认知载体×10、武器检查套组×17、血菌块×2、血菌×4、重红柱状菌×4、中红柱状菌×1） |
| `_tmp/maahub-import/now12.png` | 「获得奖励 信用×100」弹窗（第 2 轮中途，后被自动关闭） |
| `_tmp/maahub-import/final1.png` | **游戏已关闭**，只剩启动器「开始游戏」 |
| `_tmp/maahub-import/maafw_final.log` | MaaEnd 完整执行日志（1.49 MB） |
| `_tmp/maahub-import/trace_export.txt` | MaaHub worker 侧 trace（含 16 条 post） |
| `_tmp/maahub-import/hub_state4.png` / `daily1-3.png` | GUI 状态序列 |
| `_tmp/maahub-import/high_info.txt` | 提权点击证据（`elevated=True`） |

## 7. 遗留问题

| 级别 | 问题 | 说明 |
|---|---|---|
| P0 | **MaaHub 未检测目标窗口完整性** | 游戏 elevated 时静默失败，表现为任务超时而非明确报错。建议连接时用 `OpenProcess` + 完整性级别探测，给出「请以管理员重启」提示。 |
| P1 | **AutoEcoFarm 三子任务失败** | 20:25:38 `WulingCityFarmFailed`/`SisPaintingFarmFailed`/`RanchFailed`。农场奖励弹窗（「获得奖励」）在该轮未被自动关闭；第 1 轮同位置卡死。需要 MaaEnd 侧补一个「通用奖励弹窗关闭」节点，或 MaaHub 侧加兜底点击。 |
| P2 | **CLI 会杀掉运行中的 worker** | 实跑期间执行 `MaaHub.exe --cli tasks` 触发 `hub dtor workers stopped`，把正在跑的 worker 全停了，导致任务中断。CLI 与 GUI 实例应当互不干扰（单实例检查或独立 state 锁）。 |
| P2 | **GUI 无「任务进度」视图** | 右侧 `暂无日志`；实跑中只能靠 `maafw.log` 判断进度。事件日志页与运行时日志未打通。 |
| P3 | **每次跑完 worker/agent 会失配** | 上一轮结束后 `maaend post` 成功但 agent 无响应，必须重启 MaaHub 才能再跑。worker 与 agent 生命周期管理需复查。 |

## 8. 复现步骤

```text
1. 确保游戏已登录进入主界面（不要在登录页/加载页）
2. 以管理员身份启动 build\MaaHub.exe（/RL HIGHEST）
3. 左栏选「全套日常」→ 右上「连接」（等状态变绿「已连接 · 核心 v5.15.0-beta.1」）
4. 点「Link Start!」
5. 等待 30–40 分钟；期间不要用 --cli 调用 MaaHub，不要手动抢焦点
6. 结束时游戏自动关闭（CloseGamePC），桌面只剩启动器
```

## 9. 复核方法

```powershell
# 任务是否在跑
Get-Content ...\maafw.log -Tail 5

# 16 条 post 是否发出
Select-String -Path ...\state\trace.log -Pattern "maaend post" | Select-Object -Last 16

# 游戏是否被关掉
if (Get-Process Endfield -ErrorAction SilentlyContinue) { "GAME_RUNNING" } else { "GAME_CLOSED" }
```
