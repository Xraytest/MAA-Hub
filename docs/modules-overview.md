# MaaHub 模块分块与最近简报

> 分块依据：源码目录 `src/` + 构建单元。简报取最近一轮工作（11-04 ～ 11-10），**加粗**为本轮重点。

## 一、分层

```
入口   main.cpp（GUI / --cli 20+ 命令）        worker_main.cpp（子进程入口）
编排   hub.hpp（执行器 · 任务序列 · 定时调度线程 · 状态快照）
能力   core_manager · config_import · accounts · settings · planner
适配   adapters/{adapter, maa_adapter, maaend_adapter}   worker_client
界面   gui.hpp（自绘框架）  app_window（页面/导航）  task_panels（任务面板）
基础   json.hpp · util.hpp
验证   tests/
```

## 二、分块与简报

### 1. 入口与 CLI — `main.cpp`（42 KB）

- 职责：GUI 启动、参数分发、`--cli` 全部子命令、`--check/--self-test/--startup-bench`。
- **最近（11-09）**：`--cli` 扩到 20+ 命令（`import / sched-list|run|tick|serve / connect / run / seq / stop / events / get|set-connect / override / accounts / add-account / use-account / link`），CLI 从调试附属升为一等脚本入口。
- 11-09：`import <kind:auto|maa|maaend> <安装目录或配置文件>` 两种入参等价。

### 2. Hub 编排 — `hub.hpp`（62 KB）

- 职责：执行器模型（独立 worker + 绑定核心 + 可选账号）、任务序列、定时调度、状态快照。
- **最近（11-09，+539 行）**：`merge_schedules` 按 `exec_id + source` 去重并保留 `last_run_date`（重复导入幂等）；调度线程 10 s tick，命中条件 =「该执行器未在跑 + 今天没跑 + 到点」，**先写 `last_run_date` 再触发**。
- `--cli sched-tick` 手动扫一次、`serve` 常驻只跑调度器。

### 3. 核心管理 — `core_manager.hpp`（34 KB）

- 职责：核心登记 / 版本探测（子进程加载 DLL 查 AsstGetVersion、MaaVersion）/ 检查更新 / 下载解包 / 默认核心。
- **最近（11-08，+60 行）**：多源回退（GitHub 直连 → gh-proxy → ghfast → moeyy → Mirror酱 CDK）、`tar.exe` 解包、版本校验；来源 `adopt / download / manual`；被删核心自动回退默认。
- **11-10 GUI 首验**：核心管理页 8 列表格（类型/名称/版本/来源/路径/默认/更新/操作）与 `state\final_cores.json` 逐条一致（4 条：6.13.0、5.15.0-beta.1、6.21.1、6.22.0）。

### 4. 配置导入 — `config_import.hpp`（38 KB，新增未跟踪）

- 职责：解析 MAA `gui.json` / MaaEnd `mxu-MaaEnd.json` → 执行器 + 任务参数 + 定时计划报告（不直接落盘任务列表，由 GUI 合并）。
- **最近（11-10，修复 P0）**：
  - 现象：MAA 6.x 把配置迁到 `config\gui.new.json`，旧代码只找 `gui.json`，读到 11-05 的历史值 → 定时 3(14:00)/4(20:00) 误判启用。
  - 修复：路径候选改为 `gui.new.json` 优先；定时先解析 `Timers.List[]`（`Id/IsEnabled/Config/Hour/Minute`），解析不到才回退 `Global.Timer.TimerN`。
  - 验证：`source_file=...\gui.new.json`、Timer1/2 `enabled=1`、Timer3/4 `enabled=0`，与 MAA 原生界面逐条一致。
- 11-09 既有：`install_tag` 多安装隔离（执行器 `MAA Default @MaaAssistantArknights_5`，source 前缀 `MaaAssistantArknights_5:`）。

### 5. 适配层 — `adapters/`

- 职责：MaaCore / MaaFramework 统一抽象（`IAdapter` + `AdapterEvent`）。
- **`maa_adapter.hpp`（11-10）**：连接前 `AsstSetInstanceOption(TouchMode=2)` 设 `maatouch`；连接后轮询 `AsstConnected` 最多 20 s；失败信息带 `adb/addr/config + touch 备注 + 事件流原因`；从 `ConnectionInfo` 事件取 uuid。
- **`maaend_adapter.hpp`（11-05，+557 行）**：拉起 agent（`go-service`/`cpp-algo` 并行、50 ms 轮询）、参数映射、任务可见性按 `enabledByController["Win32-Front"]`（缺失回退 `enabled`）。

### 6. Worker 子进程 — `worker_client.hpp` / `worker_main.cpp`

- 职责：主进程 ↔ 子进程 JSON 行协议；子进程各自加载核心 DLL，多执行器互不干扰。
- 最近（11-04/11-05）：`worker_main` 与 Python 版协议一致可互换；命令 `discover/load/connect/disconnect/run/stop/status/tasks/unload/exit`。

### 7. GUI 框架 — `gui.hpp`（53 KB）

- 职责：零依赖自绘控件库（按钮/复选框/输入框/选项条/滚动区/卡片/CJK 字体/裁剪栈/热激活焦点）。
- 最近（11-04，+506 行；11-03 起）：官方亮/暗双主题、HandyControl 滚动条、SVG 图标集（Carbon Icons）、焦点跟踪。

### 8. 主窗口与页面 — `app_window.cpp/.hpp`（158 KB）

- 职责：左栏执行器列表 + 主区（实例页 / 账号管理 / 核心管理 / 智能规划 / 事件日志 / 设置）、标题栏、导航、导入回调。
- **最近（11-09）**：核心管理页（拉取/检查/设默认/删除，8 列表格）、导入回调 → `merge_schedules` + `WM_APP_IMPORT`；`page_` 路由 6 个页面。
- 11-03：视觉审计修复「自动战斗/欢迎页」三处遗留缺陷、自定义标题栏、任务切换过渡动画。
- 已知问题：设置页曾单帧 53 s；默认窗口 1006×639 装不下 8 列，无最大化按钮。

### 9. 任务面板 — `task_panels.hpp`（46 KB）

- 职责：MAA 官方 13 个任务面板 1:1 复刻（XAML 对照）；MaaEnd 66 任务；未定制任务走类型化通用编辑器。
- 最近（11-04，+341 行）：面板标签取官方 `zh-cn`；参数归一到核心真实参数（`normalize_task_params`）。

### 10. 账号 — `accounts.hpp`（11 KB）

- 职责：账号 CRUD / 启用停用 / 切换；执行器绑定账号。
- 最近（11-04）：MAA 注入 `account_name`，MaaEnd 注入 `AccountSwitch` pipeline override；记录每个账号日常完成情况。

### 11. 设置 — `settings.hpp`（7.6 KB）

- 职责：代理（none/http/socks5）、镜像源 CDK、LLM、主题、启动、更新策略；持久化 `state\settings.json`。
- 最近（11-08，+57 行）：补「启动设置」「更新设置」两组（含持久化与测试）。

### 12. 智能规划 — `planner.hpp`（20 KB）

- 职责：执行器维度规划（唤醒/切号 → 日常）；启发式 + LLM（OpenAI 兼容，默认 DeepSeek）。
- 最近（11-04）：白名单校验（任务名/参数/执行器存在性）；无 key 或不可达自动回退启发式并给出警告。

### 13. 基础库 — `json.hpp` / `util.hpp`

- `json.hpp`（16 KB）：header-only 极简 JSON（对象/数组/字符串含转义与 UTF-8/整数/浮点/布尔/null），零依赖。
- `util.hpp`（23 KB）：UTF-8↔Wide、Base64、SHA1、窗口查找、进程启动/句柄、文件读写、HTTP 请求；**11-08 扩充约 266 行**（HTTP、进程与文件工具）。

### 14. 测试 — `tests/`

- `headless_tests.py`（无头功能自检）、`shared_core_test.py`（共享核心）、`stress_test.py`（压力）、`maaend_mapping_test.py`（**11-05 新增**，MaaEnd 参数映射）、`settings_persist_test.py`（设置持久化）、`shot.py`（截图）/`audit_panels.py`、`final_audit.py`（视觉审计）。

## 三、状态与风险

| 项 | 状态 |
|---|---|
| 多安装导入（Maa1/Maa5）→ 连接 → 6 个一键长草 → 真实定时触发 | **11-09 端到端通过**（`trace.log` 有 `[sched] exec-... -> done`） |
| 定时数据与 MAA 原生一致性 | **11-10 已对齐**（gui.new.json 修复后） |
| 核心管理页视觉对比 | **11-10 PASS**（能力面超过 MAA 原生） |
| 设置页慢帧（曾 53 s） | 未修，P1 |
| 定时计划无 GUI 页（只能 `--cli sched-list`） | 未做，P2 |
| 表格列截断 / 窗口无最大化 | 未修，P2 |
| 11-05 以来全部改动仍在 working tree（未提交） | 需整理提交 |

## 四、证据

- `docs/verify-20261109-multi-install.md`：多安装 + 定时触发实机验证
- `docs/verify-20261110-visual-smoke.md`：核心管理 vs MAA 原生视觉对比（含 P0 根因与回归）
