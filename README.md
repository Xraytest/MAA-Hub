# MaaHub C++ — MAA / MaaEnd 统一管理器（原生 Win32 GUI）

**C++17 原生实现**，使用设备上的 **VS Code + Visual Studio 18 Community (MSVC 14.51)** 开发构建。
零第三方依赖：GUI 框架、HTTP 下载、JSON、SHA1/Base64 全部手写；**不依赖任何浏览器/网页技术**，
整个界面由 Win32 + GDI 双缓冲自绘。

## 核心概念

| 概念 | 说明 |
|---|---|
| **核心 (Core)** | 一个可用的 MAA / MaaEnd 程序目录。可「收养」本机已有安装，或由软件**内置拉取/更新**获得 |
| **执行器 (Executor)** | 一个**独立的原版软件实例**：独立 worker 子进程 + 绑定一个核心 + 可选绑定一个账号。多个执行器可同时运行，实现**多账号并行** |
| **实例 UI** | 每个执行器都是 MAA 风格界面（任务勾选、⚙ 参数、▶ 立即运行、🔗 链接开始）。MaaEnd 实例同样使用 MAA 风格 UI |

## 功能

- **执行器管理**：左侧创建/删除/切换执行器；每个执行器独立进程，独立连接游戏窗口
- **核心管理（内置拉取与更新）**：
  - 版本探测：子进程加载 `MaaCore.dll`（AsstGetVersion）/ `MaaFramework.dll`（MaaVersion）权威查询
  - 检查更新：GitHub API 多源回退（直连 → gh-proxy → ghfast → moeyy → Mirror酱 CDK）
  - 拉取安装：WinHTTP 下载 release zip（带进度）→ Windows 自带 `tar.exe` 解包 → 版本校验 → 登记新核心
  - 收养本机已有安装（默认自动收养 `D:\Application\MAA`、`D:\Application\MaaEnd`）
  - 默认核心切换；执行器绑定核心被删除时自动回退默认核心
- **任务目录**：MAA 13 个 AsstApi 任务；MaaEnd 44 个 pipeline 任务（解析 interface.json）
- **多账号**：账号 CRUD / 启用停用 / 切换；执行器绑定账号后，MAA 自动注入 `account_name`，
  MaaEnd 自动注入 `AccountSwitch` pipeline override；跟踪每个账号日常完成情况
- **LLM 规划**：全部执行器状态 + 账号 + 任务目录 → OpenAI 兼容接口（默认 DeepSeek）→
  结构化 JSON 计划 → 白名单校验（任务名/参数/执行器存在性）→ 人工确认执行；
  无 key / 不可达时自动回退启发式规划器并给出警告
- **MAA 风格自绘 UI**：执行器侧栏、实例页、账号管理、核心管理、智能规划、事件日志

## 构建（VS Code / MSVC）

```powershell
# 方式一：VS Code
#   双击 MaaHub.code-workspace 打开
#   Ctrl+Shift+B → "MaaHub: Build (Release)"
#   F5 → "MaaHub: Run (GUI)" / "MaaHub: Self-Check" / "MaaHub: Worker (debug)"

# 方式二：命令行（任意目录可用）
build.bat release     # 或 debug
```

产物：`build\MaaHub.exe`（约 700 KB，原生代码，零运行时依赖）。

## 运行

```powershell
build\MaaHub.exe                  # GUI 模式：原生窗口（无任何网页特性）
build\MaaHub.exe --check          # 干跑自检：加载核心 + 拉起全部执行器
build\MaaHub.exe --core-version maa "D:\Application\MAA"   # 探测核心版本（子进程）
```

首次启动自动收养本机安装并为每个类型创建一个默认执行器。
账号/核心/执行器持久化在 `build\state\`（`accounts.json` / `cores.json` / `executors.json` / `state.json`）。

## 架构

```
MaaHub.exe (GUI 进程, Win32 自绘)
├── 后台线程：快照刷新（500ms）+ 动作队列（连接/运行/拉取等）
└── Hub
     ├── CoreManager   核心登记/版本/更新检查/下载解包
     ├── AccountStore  账号 CRUD 与账号注入
     ├── Executors
     │    ├── exec-A (MAA, 核心A, 账号1) ──→ worker 子进程 ──→ MaaCore.dll
     │    ├── exec-B (MAA, 核心A, 账号2) ──→ worker 子进程 ──→ MaaCore.dll   ← 独立实例
     │    └── exec-C (MaaEnd, 核心B)      ──→ worker 子进程 ──→ MaaFramework.dll
     │                                                          └→ agent/go-service.exe <uuid>
     │                                                          └→ agent/cpp-algo.exe  <uuid>
     └── Planner (heuristic / LLM)
```

关键设计：
1. **一执行器一进程**：MaaCore.dll（MAA fork）与 MaaFramework.dll 的同名依赖（MaaUtils.dll 等）
   无法共存于同一进程，因此每个实例都是独立 worker 子进程。
2. **UI 线程零阻塞**：所有 worker 通信只发生在后台线程；UI 只读快照拷贝
   （快照整体替换，UI 取拷贝避免悬空指针）。
3. **DLL 搜索路径**：`AddDllDirectory`（cookie 保活）+ PATH 前置 +
   `LoadLibraryEx(LOAD_WITH_ALTERED_SEARCH_PATH)`；MaaEnd agent 子进程前注入 maafw 到 PATH。
4. **Base64 传配置**：worker config 以 base64 经命令行传递，避免 JSON 反斜杠被消费。
5. **核心解包**：用 Windows 自带 `tar.exe`（libarchive）解 zip，零第三方依赖。

## 源码结构

```
src/
├── main.cpp            入口：参数解析、--check、--core-version、GUI 消息循环
├── app_window.{hpp,cpp} 原生窗口：侧栏执行器/实例页/账号/核心/规划/事件
├── gui.hpp             零依赖立即模式 GUI（按钮/复选/输入/选项条/滚动/剪裁栈）
├── hub.hpp             执行器模型 + 事件总线 + 计划执行
├── core_manager.hpp    核心登记、版本探测、更新检查、下载解包
├── accounts.hpp        多账号存储与注入
├── planner.hpp         启发式 + LLM 规划器与输出校验
├── worker_main.{hpp,cpp}  worker 子进程协议
├── worker_client.hpp   主进程侧 worker 客户端
├── json.hpp            零依赖 JSON
├── util.hpp            UTF-8/Wide、Base64、SHA1、窗口查找、进程/管道、文件
└── adapters/
    ├── adapter.hpp     IAdapter 接口 + 统一事件/状态
    ├── maa_adapter.hpp MaaCore AsstApi 直调
    └── maaend_adapter.hpp MaaFramework + MaaAgentClient + agent 子进程
tests/
├── shot.py             PrintWindow 窗口截图工具（含 PNG 编码器）
├── interact_test.py    基础交互测试
├── exec_interact_test.py 执行器架构交互测试
├── executor_state_test.py 执行器/核心/多实例并发/核心回退测试
└── pxprobe.py, layoutprobe.py  截图像素分析探针
```

## 已验证（真机，不启动游戏）

- `--check` **ALL OK**：3 个执行器并发加载（2× MAA v6.13.0 + 1× MaaEnd v5.15.0-beta.1），任务目录 13/44
- **执行器全流程**：默认实例自动创建 → UI 新建第 3 个执行器 → 独立实例页 → 持久化 → 重启恢复
- **核心管理**：版本探测（子进程加载 DLL）、更新检查真实返回 **MAA v6.20.1**（经 gh-proxy 镜像）、
  下载地址解析正确；删除核心后执行器自动回退默认核心仍 ALL OK
- **MaaEnd 实例同样为 MAA 风格 UI**，44 个任务正常列出
- 悬空指针修复：快照整体替换不再导致 UI 冻结
- GUI 线程零阻塞：catalog 走快照缓存

## 未验证（需启动游戏 / 需 CDK）

- MAA Win32 挂窗对 `Arknights.exe` 的实际绑定、任务执行、账号切换的游戏内识别
- MaaEnd Win32 controller 连接与任务执行
- **核心 zip 的实际下载与解包**：更新检查与 URL 解析已验证；完整下载需要可用的网络出口
  （本机直连 GitHub 被阻断，需镜像或代理）
- 真实 LLM 调用（无 API key；回退链路已验证）

## 许可与免责

本软件仅作为两个开源项目（[MAA](https://github.com/MaaAssistantArknights/MaaAssistantArknights)
AGPL-3.0、[MaaEnd](https://github.com/MaaEnd/MaaEnd) AGPL-3.0）的**调用方**，通过其公开 C API 工作，
不修改、不分发它们的任何文件。请遵守各游戏的服务条款，自行承担使用风险。
