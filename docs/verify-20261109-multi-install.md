# MaaHub 多安装配置导入 + 定时调度 实机验证报告

- 日期：2026-11-09 22:00–22:32（Asia/Shanghai）
- 目标机：ENG01 = WorkCenter，`DESKTOP-P72CMPJ`，`10.10.226.8`，账号 `xray`
- 程序：`C:\Users\xray\Documents\MaaHub\build\MaaHub.exe`
  - SHA-256 `e0efa886a8afcca236713c9cc95ae420a26cd858942598e06ac3d1f0a7e30c35`
  - 大小 1,475,584 B，构建时间 2026-11-09 22:12
- 环境：MuMu 12 无头运行，adb `127.0.0.1:16384`，游戏 `com.hypergryph.arknights` 1280×720
- 核心：两套执行器均绑定 MaaHub 内部 `maa-v6.13.0`（`core-e176a23d5660eb22`）

## 1. 本次改动

### 1.1 问题

配置导入原先用 `"MAA " + 配置名`（如 `MAA Default`）作执行器标签、`"Timer.TimerN"` 作定时器 source。
同一台机器装了 MAA1/3/5/6 且配置都叫 `Default` 时：

- 第二次导入会**复用第一次的执行器**（label 相同）；
- 两套定时器的 source 都是 `Timer.TimerN`，按 `exec_id + source` 去重 → **后导入的直接覆盖先导入的**，不可区分也不可同时管理。

### 1.2 修改（`src/config_import.hpp`）

引入 `install_tag`——从配置文件路径反推安装目录名，参与两处命名：

| 位置 | 无 tag（老行为） | 有 tag |
|---|---|---|
| 执行器标签 | `MAA Default` | `MAA Default @MaaAssistantArknights_5` |
| 定时器 source | `Timer.Timer2` | `MaaAssistantArknights_5:Timer.Timer2` |
| MaaEnd 实例标签 | `<实例名>` | `<实例名> @<安装目录>` |
| MaaEnd source | `schedulePolicies` | `<安装目录>:schedulePolicies` |

- `install_tag_of(file)`：`<安装>\config\gui.json` → 去掉最后两段再取末段目录名；反推不出（盘符根等）返回空，退回不做区分。
- **目录与文件两种入口等价**：选安装目录、或直接选 `config\gui.json`，tag 相同。
- 导入报告新增 `install_tag` 字段，通知栏显示为 `MAA 配置导入完成（MaaAssistantArknights_5）：…`。

### 1.3 未改动

- 导入仍然**不写任务序列**：MAA 定时器只表达"几点跑"，任务表来自 Hub 自己的 `state\task-lists.json`。
- 执行器仍绑定 MaaHub 内部核心（不复制安装目录的 `MaaCore.dll`），见 §4 限制 3。

## 2. 验证结果

两台安装各自完整走了一遍「导入 → 连接 → 六个一键长草 → 真实定时触发」。

| 验证项 | Maa5 `C:\Program Files\MaaAssistantArknights_5` | Maa1 `C:\Program Files\MaaAssistantArknights` |
|---|---|---|
| 安装目录导入 | ✅ 新建 1 执行器，8 条定时（4 启用） | ✅ 新建 1 执行器，8 条定时（1 启用 06:00） |
| 执行器 | `exec-5fa8dd2145ee` | `exec-84691d0fba02` |
| tag 隔离 | ✅ source 前缀 `MaaAssistantArknights_5:` | ✅ 前缀 `MaaAssistantArknights:` |
| 重复导入幂等 | ✅ 复用执行器，条数不变 | ✅ 同 |
| 连接模拟器 | ✅ `conn=connected` | ✅ `conn=connected` |
| StartUp | ✅ 3 s | ✅ 5 s |
| Fight | ✅ 50 s（理智不足正常退出） | ✅ 57 s（同） |
| Recruit | ✅ 5 s | ✅ 5 s |
| Infrast | ✅ 13 s | ✅ 11 s |
| Mall | ✅ 52 s | ✅ 33 s |
| Award | ✅ 14 s | ✅ 14 s |
| 整串 `sched-run` | ✅ `SCHED done`，RC=0 | ✅ `SCHED done`，RC=0 |
| 真实定时触发 | ✅ 常驻 `--cli serve`，22:21 Timer5 自动触发，`last=2026-11-09` | ✅ 22:31 Timer2 自动触发，`last=2026-11-09` |

定时触发的进程内证据（`state\trace.log`）：

```
[sched] exec-5fa8dd2145ee -> done      # Maa5，22:21:13
[sched] exec-84691d0fba02 -> done      # Maa1，22:31:05
```

六个任务的内核证据（`cores\maa-v6.13.0\cache\maahub\debug\asst.log`，
两轮都出现 `AllTasksCompleted` 且 `finished_tasks` 依次为 1..6）：

```
Maa5 Px224040: StartUp 22:23:58 / Fight 22:24:48 / Recruit 22:24:53
               Infrast 22:25:06 / Mall 22:25:58 / Award 22:26:12
Maa1 Px226992: StartUp 22:26:32 / Fight 22:27:29 / Recruit 22:27:34
               Infrast 22:27:45 / Mall 22:28:27 / Award 22:28:41
```

导入后 `schedules.json` 共 16 条，两条执行器的 8 条互不覆盖；最终 `last_run_date` 均已清空，
`task-lists.json` 两套均为 `StartUp, Fight, Recruit, Infrast, Mall, Award`。

## 3. 复现步骤

```bat
:: 导入（选安装目录）
MaaHub.exe --cli import auto "C:\Program Files\MaaAssistantArknights_5"
MaaHub.exe --cli import auto "C:\Program Files\MaaAssistantArknights"

:: 看计划
MaaHub.exe --cli sched-list

:: 立即跑该执行器的任务表
MaaHub.exe --cli sched-run exec-5fa8dd2145ee

:: 手动扫一次到点触发（返回触发条数）
MaaHub.exe --cli sched-tick

:: 常驻调度（10 s tick，Ctrl+C 退出）
MaaHub.exe --cli serve
```

## 4. 已知限制（如实记录，非缺陷掩盖）

1. **Fight 完整战斗只在一轮验证过**。22:00 那轮 `Fight` 跑了 **941 s**（正常打完）；
   22:24 / 22:27 两轮因游戏理智只剩 **10/205**（单次 1-7 需 18），MAA 检测到理智不足后
   50–57 s 内正常结束任务链（`AllTasksCompleted`，非错误）。因此"两套执行器各自完整打完一局"
   这一点未同时成立，属游戏资源限制。
2. ~~**StartUp 依赖游戏已在运行**~~ → **2026-11-10 已修复**。根因不是 MAA 能力问题，而是
   MaaHub 移植时把 `start_game_enabled` 的默认值写成了 `false`
   （`src/adapters/maa_adapter.hpp:430`），MaaCore 收到 `start_game_enabled=false` 就跳过
   "启动客户端"这一步、直接等游戏界面，于是表现为"拉不起游戏"。改为默认 `true` 后实测：
   模拟器内无游戏进程时，StartUp 在 **85 s** 内自动拉起游戏（PID 12664），无需手工 `am start`。
   详见 §7.1。
3. ~~**核心版本与安装目录不一致**~~ → **2026-11-10 已升级**。Hub 内默认核心已换成
   **MAA v6.22.0**（`core-13eb14b49797c81a`，由官方 `MAA-v6.22.0-win-x64.zip` 解压后
   `--import-core` 导入），三个 MAA 执行器全部改绑到它。安装目录里的 `gui.json` 属于
   客户端 6.21.x，与核心版本无关；导入只迁移配置，不复制安装目录的 `MaaCore.dll`。
   详见 §7.2。
4. ~~**GUI 文件选择对话框未实机点击**~~ → **2026-11-10 实机走通**。在 ENG01 的桌面
   session 1（`schtasks /RU xray /IT`）注入鼠标/消息，完整走了「设置 → 导入配置… →
   文件夹对话框 → 填安装路径 → 选择文件夹」，界面出现绿色提示
   `MAA 配置已导入 (MaaAssistantArknights_5)：执行器复用 1，任务 0 个，参数 0 项，跳过未知键 9 个，定时计划 8 条`，
   落盘的 `schedules.json` 与 CLI 导入结果逐字段一致。详见 §7.3。
5. **`sched-run` 与调度器触发共用 `run_task_sequence`**，两者都会重置全局停止标志，
   同一时刻只能由一个入口驱动；两个执行器操作的是**同一台模拟器**（127.0.0.1:16384），
   因此两套执行器必须串行跑，不能并行。

## 5. 部署与回滚

- 新 exe 已覆盖 `C:\Users\xray\Documents\MaaHub\build\MaaHub.exe`（hash 见文首）。
- 改动前状态备份（同名覆盖前自动新建）：
  - `C:\Users\xray\Documents\MaaHub\state.bak-20261109-221702`
  - `C:\Users\xray\Documents\MaaHub\state.bak-20261109-221727`
- 回滚：把上述任一备份目录内容复制回 `build\state\`，并把旧 exe 放回 `build\MaaHub.exe`。
- 本次删除了旧的 `exec-0a97079423f2`（label `MAA Default`，无 tag）。它只在 11-09 17:43
  由本会话导入产生（`params_written=0`，无用户参数），其数据在备份中。

## 6. 本地验证（无游戏环境）

在 `D:\WorkSpace\_tmp\maahub-import\test-installs\` 造了两个假安装目录：

- `MaaAssistantArknights_5\config\gui.json` → tag `MaaAssistantArknights_5`，8 条（4 启用）
- `MaaAssistantArknights\config\gui.json` → tag `MaaAssistantArknights`，8 条（1 启用 06:00）
- 分别用「目录」和「直接选文件」两种入口导入，tag 一致
- 重复导入两次，条数稳定在 16，不增长
- 把其中一条改到下一分钟并 `sched-tick`：`fired=1`、`last=2026-11-09`，再 tick 不重复触发

## 7. 2026-11-10 补充验证（推翻了 §4 的三条限制）

### 7.1 `start_game_enabled` 默认值修复（MAA 能自己拉起游戏）

**现象**：StartUp 只要游戏没在跑就失败，需要先手工 `am start`。

**根因**：`src/adapters/maa_adapter.hpp:430` 里 `start_game_enabled` 的默认值是 `mj::Value(false)`，
与 MAA 官方默认（开启"自动启动客户端"）相反。MaaCore 拿到 `false` 就完全跳过启动步骤。

**修复**：

```cpp
// src/adapters/maa_adapter.hpp:430
mj::Value(true),   // 之前是 mj::Value(false)
// 描述改为：是否自动启动客户端（MAA 默认开启）
```

**验证**（ENG01，MuMu 12 无头，`127.0.0.1:16384`）：

| 项目 | 结果 |
|---|---|
| 修复前 | `StartUp` 直接失败 / 空转，游戏进程不存在 |
| 修复后 | `StartUp` **85 s** 内把游戏拉起来，`PID 12664` |
| 调度入口 | `MaaHub.exe --cli sched-run exec-5fa8dd2145ee` 六任务串行 RC=0 |

修复后的 exe 已覆盖部署，旧 exe 备份在 `build\MaaHub.exe.bak-prestartupfix`。

### 7.2 核心升级到 v6.22.0

| 项 | 值 |
|---|---|
| 来源 | 官方 release `MAA-v6.22.0-win-x64.zip`（294,969,951 B，2026-11-09T14:51:41Z 发布，`target_commitish: dev-v2`） |
| 导入命令 | `MaaHub.exe --import-core maa "<解压目录>"` |
| 核心 id | `core-13eb14b49797c81a`，`version=v6.22.0`，`is_default=true` |
| 改绑 | `exec-5fa8dd2145ee`(Maa5) / `exec-84691d0fba02`(Maa1) / `exec-8b1b54dccbeb`(默认实例) 三个 MAA 执行器 |
| 连通性回执 | `connect=1 exec=exec-5fa8dd2145ee kind=maa conn=connected run=idle version=v6.22.0 tasks=13` |
| 控制单元 | `mode=adb screencap=32 touch=maatouch adb=…\nx_main\adb.exe addr=127.0.0.1:16384 config=MuMuEmulator12` |

升级后六步一键长草 + 单局 Fight 均通过：

```
[sched-run] AllTasksCompleted finished_tasks: 2 Fight / 3 Recruit / 4 Infrast / 5 Mall / 6 Award
            （07:30:08–07:31:42）
Fight: started task=Fight id=1 / finished task=Fight state=idle elapsed=941s / FIGHT_RC=0
```

> 注：`941 s` 是完整打完一局（理智充足时）；六步串行那轮 Fight 因理智不足 50 s 内正常结束。

### 7.3 GUI 完整走一遍 Maa5 配置导入

在真机桌面把「导入配置」整条链路点通了，结果与 CLI 一致。

**前置条件（踩过的坑）**：

1. **必须在交互式桌面会话**。SSH 登录是 session 0，`CopyFromScreen` 报"句柄无效"、
   `Get-Process MaaHub` 的 `MainWindowHandle=0`、注入的点击也到不了窗口。
   解法：`schtasks /Create /RU xray /IT /SC ONCE` + `schtasks /Run`，进程落在 session 1、
   `WinSta0\Default`。
2. **设置页不可滚动**。`page_settings` 没有 `begin_scroll`，窗口态下「导入配置…」按钮在
   可视区外；滚轮注入 18 次无效。解法：`ShowWindow(hwnd, SW_MAXIMIZE)`，最大化后按钮在
   客户区 (416, 947)。
3. **坐标即物理像素**。`GetDpiForSystem=96`、`dpiForWindow=96`，无缩放，左菜单「设置」
   在窗口态 y=625、最大化后 y≈331。
4. **.ps1 必须是纯 ASCII**。PowerShell 5.1 按 ANSI/GBK 解析 `.ps1`，含中文字符串会语法报错
   （`"选择文件夹"` 被读成乱码导致 `UnexpectedToken`）。改用 `GetDlgCtrlID()` 判 IDOK=1 定位按钮。
5. **模态对话框会累积**。每次点「导入配置…」都会 `Show()` 一个新的模态对话框；反复尝试后
   现场堆了 **28 个**。清理用 `PostMessage(WM_CLOSE)` 逐个关闭——`post_action` 里
   `if (path.empty()) return;`，取消分支不写库，安全。

**操作序列**（全部通过 Win32 消息，不依赖前台焦点）：

```
SW_MAXIMIZE                       → 1920x1080
mouse_event 点击 左菜单「设置」      → 设置页渲染（含「导入配置…」）
mouse_event 点击 (416, 947)        → 弹出 CLSID_FileOpenDialog 文件夹选择器
EnumWindows 找 #32770 + 有 Edit 子窗口的 → 拿到 picker hwnd
EnumChildWindows 取 Edit + Button(GetDlgCtrlID==1)
SetWindowText(edit, "C:\Program Files\MaaAssistantArknights_5")
PostMessage(button, BM_CLICK)      → 对话框确认
```

**结果**（界面底部绿色提示，截图 `gui_msgok.png` / `gui_clean.png`）：

```
MAA 配置已导入 (MaaAssistantArknights_5)：执行器复用 1，任务 0 个，参数 0 项，
跳过未知键 9 个，定时计划 8 条
```

**落盘核对**（`build\state\schedules.json`，时间戳 07:51）：

| Timer | 时间 | enabled | exec_id | source |
|---|---|---|---|---|
| Timer1 | 02:00 | true | `exec-5fa8dd2145ee` | `MaaAssistantArknights_5:Timer.Timer1` |
| Timer2 | 08:00 | true | 同 | `…:Timer.Timer2` |
| Timer3 | 14:00 | true | 同 | `…:Timer.Timer3` |
| Timer4 | 20:00 | true | 同 | `…:Timer.Timer4` |
| Timer5 | 12:00 | false | 同 | `…:Timer.Timer5` |
| Timer6 | 15:00 | false | 同 | `…:Timer.Timer6` |
| Timer7 | 18:00 | false | 同 | `…:Timer.Timer7` |
| Timer8 | 21:00 | false | 同 | `…:Timer.Timer8` |

与 §2 表格里 CLI 导入 Maa5 的结果**逐字段一致**，说明 GUI 与 CLI 走的是同一条
`import_config::import_from_path`，tag 隔离、执行器复用、定时条数都对得上。

「跳过未知键 9 个」指 `Configurations.Default` 里 MaaHub 尚未支持的 9 个键
（如 `TaskConfig`、`StartUp` 等未建模项），属预期行为，不是解析失败。
