# MaaHub C++ — MAA / MaaEnd 统一管理器（原生 Win32 GUI）

**C++17 原生实现**，使用设备上的 **VS Code + Visual Studio 18 Community (MSVC 14.51)** 开发构建。
零第三方依赖：GUI 框架、HTTP 下载、JSON、SHA1/Base64 全部手写；**不依赖任何浏览器/网页技术**，
整个界面由 Win32 + GDI 双缓冲自绘。

## 核心概念

| 概念 | 说明 |
|---|---|
| **核心 (Core)** | MAA / MaaEnd 的程序本体，**由 MaaHub 内部管理**：拉取后存放在 `<exe_dir>\cores\<kind>-<version>\`，不依赖、不使用系统中已有的安装 |
| **执行器 (Executor)** | 一个**独立的原版软件实例**：独立 worker 子进程 + 绑定一个核心 + 可选绑定一个账号。**多个执行器可同时运行，实现多账号并行** |
| **实例 UI** | 每个执行器都是 MAA 风格界面（任务勾选、⚙ 参数、▶ 立即运行、🔗 链接开始）。MaaEnd 实例同样使用 MAA 风格 UI |

> **核心共享（重要）**：多个执行器可绑定**同一个**核心（同一 `core_id`）。磁盘上只保存一份程序文件，
> 每个执行器是独立进程各自加载 DLL（Windows 下 DLL 代码段共享、数据段按进程私有），
> 因此**不会重复拉取/占用磁盘，又能让多个账号同时运行**。

## 单实例配置页面（与 MAA 官方 1:1）

依据 MaaAssistantArknights 官方 GUI 源码复刻（`src/MaaWpfGui/Views/UI/TaskQueueView.xaml`、
`Views/UserControl/TaskQueue/*.xaml`、`Res/Localizations/zh-cn.xaml`、HandyControl Light 主题）：

- **布局**：外框 Margin 20 + 白色卡片 + 圆角 4；左列 **190px** 任务列表（官方 `TaskList Width=190`），
  左键选中=右侧显示该任务设置，勾选=启用；底部控制栏（连接/断开 + 状态点 + 🔗 Link Start!）
- **配色**：Primary `#326cf3`、Background `#eeeeee`、Border `#e0e0e0`、PrimaryText `#212121`、
  SecondaryText `#757575`、ThirdlyText `#bdbdbd`、Success `#2db84d`、Warning `#e9af20`、
  Danger `#db3340`、Accent `#f8491e`；控件高 28、圆角 4、内边距 (10,5)
- **官方任务名**：MAA 13 个（开始唤醒/理智作战/基建换班/…）；MaaEnd 66 个（🏍️抢委托送货/💰售卖弹性物资/…），
  直接读本机 cores 的 `locales/*/zh_cn.json`；隐藏内部入口任务（SellProduct 等）
- **任务专属设置面板**（`src/task_panels.hpp`，逐项复刻官方控件与文案）：

  | 任务 | 面板内容 |
  |---|---|
  | 开始唤醒 | 账号切换 / 是否启动客户端 / 客户端类型（官服·Bilibili·txwy·YoStarEN/JP/KR）/ 自动检测连接 |
  | 理智作战 | 理智与材料（最大理智药、最大吃石头、上报企鹅）/ 关卡与次数（关卡、作战次数、代理倍率 ×1~×6）/ 行为（博朗台模式、隐藏代理倍率、源石保存、隐藏不开放关卡） |
  | 基建换班 | 基建模式（默认/自定义/轮换）/ 无人机（不使用·贸易站·制造站·发电站·办公室）/ 宿舍心情阈值 / 会客室与宿舍 / 跨设施组合（红松骑士团·感知信息·人间烟火·深海猎人） |
  | 自动公招 | 招募次数 / 刷新 3 星 Tags / 保留许可 / 自动确认 3-6 星 / 加急许可 / Tag 策略 |
  | 信用收支 | 访问好友 / 一日一次 / 借助战赚信用 / 信用交易所 / 只买打折 / 溢出无视黑名单 / 信用下限 / 黑名单 |
  | 领取奖励 | 每日每周、邮件、免费单抽、幸运墙、开采许可、周年月卡、限时签到（含官方提示） |
  | 自动肉鸽 | 基础（主题/难度/模式/分队）/ 开局（开局干员、直升精二、只凹精二、期望奖励）/ 投资与结束（投资源石锭、探索次数、投资锭数、备用上限）/ 行为（刷新商店、非好友助战、BOSS 前暂停、满级停止、月度小队、深入调查） |

- **MaaEnd 任务面板**（选项名与标签取自本机 cores 的 `tasks/*.json` + `locales/*/zh_cn.json`）：
  日常奖励领取（领取项 6 + 每日任务 5）、自动采集（区域 6 + 模式/目标库存/存放背包）、
  协议空间（自动战斗/队伍/模式/成功失败上限/培养道具目标）、赠送干员礼物（干员/数量/只收/全收）、
  拜访好友（备注好友/优先好友/搜索定位）、账号切换（启用账号切换）

- **未定制任务**：类型化通用编辑器按 catalog 声明的 type 渲染
  （bool → 三态勾选、number → 数字步进、select → cases 单选条、array → 分号列表），
  标签用官方 description / `option.<key>.label`，cases 用 `option.<O>.cases.<C>.label`
  （含 UTC↔GMT 跨版本键名归一化）

### 官方 TaskQueueView.xaml 元素逐项覆盖

| 官方元素 | 实现 |
|---|---|
| 背景层级 RegionBrushOpacity25 / BorderBrush | ✅ 浅灰白体系，左列独立 210px 卡片 |
| 左列 `Width=190` + `NoSelectedHighlightListBoxStyle` | ✅ 190px、无选中高亮 |
| `Common.LabelSequence.Tip`（顺序可拖动） | ✅ 拖拽排序（按执行器持久化） |
| `TaskItemStatus.InProgress/Completed/Error` | ✅ 任务项背景三色（官方色值） |
| `TaskQueue.LeftClick/RightClick/TaskSettings` | ✅ 左列提示 + 右键菜单 |
| `RunOnce/RunFromHere/Copy/Rename/Delete` | ✅ 右键菜单 5 项（含重命名内联编辑） |
| `CheckBoxesNotSavedAsNull`(+Invert) | ✅ 右键「仅一次」三态 |
| `AddTask` / `SelectAll` / `InverseSelected` | ✅ 添加新任务浮层 + 全选/反选 |
| `TaskSettings` 标题 + 来源 | ✅ 右侧头部 |
| `RunOnce` / 收起展开参数 | ✅ 单次运行 + 展开参数 |
| `GeneralSettings` / `AdvancedSettings` | ✅ 设置区顶部切换标签（宽 220 右对齐） |
| `LinkStart` / `Stop` / `WaitAndStop` | ✅ 居中 100×50 按钮组 |
| `PostAction.Settings` | ✅ 完成后动作 8 项 + 描述 + ⚙ 齿轮 |

### 官方交互语义

- **右键 = 仅生效一次**：左列任务与面板勾选项均为三态（关 / 开 / 仅一次），
  对应官方 `TaskQueue.CheckBoxesNotSavedAsNull`「该选项右键选中时仅生效一次」；
  「仅一次」运行后自动复位且不写入配置
- **左列拖拽排序**：按住左列任务拖动可调整执行顺序（对应官方 `dd:DragDrop`），
  「运行勾选」与「Link Start!」按排序后的顺序执行
- **左列右键菜单**：单次运行 / 从此处运行 / 重命名 / 复制 / 移出列表（官方 ContextMenu 5 项）
- **任务参数持久化**：面板值持久化在 `state\task-params.json`，重启后保留；
  任务别名在 `state\task-alias.json`，左列与右栏标题同步显示

### 参数归一化

面板使用友好键名，下发前由 `Hub::normalize_task_params()` 转换为 MaaCore 真实参数
（如 Infrast `mode` 0/1/2 → 0/10000/20000、`drones_idx` → `_NotUse/Money/...`、
Recruit `confirm_3..6` → `confirm` 数组、`select_tags` → `select` 数组、
Mall `blacklist_str` → `blacklist` 数组）。

## 功能

- **执行器管理**：左侧创建/删除/切换执行器；每个执行器独立进程，独立连接游戏窗口
- **核心管理（内置拉取与更新，全部由 MaaHub 内部管理）**：
  - 版本探测：子进程加载 `MaaCore.dll`（AsstGetVersion）/ `MaaFramework.dll`（MaaVersion）权威查询
  - 检查更新：GitHub API 多源回退（直连 → gh-proxy → ghfast → moeyy → Mirror酱 CDK）
  - 拉取安装：WinHTTP 下载 release zip（带进度）→ Windows 自带 `tar.exe` 解包 → 版本校验
    → **移入 `<exe_dir>\cores\<kind>-<version>\`** → 登记新核心
  - 不使用系统中已有的 MAA/MaaEnd 安装；如需迁移可用 `--import-core` 一次性复制进内部目录
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
build\MaaHub.exe --import-core maa "D:\Application\MAA"    # 一次性迁移：复制到内部 cores 目录
build\MaaHub.exe --self-test      # 无窗口功能自检
build\MaaHub.exe --startup-bench  # 启动性能基准（无窗口）
```

**启动性能**：渐进式加载——启动先产出「即时快照」（只读持久化文件，**约 3ms**），
左侧执行器立即可见；worker 进程与核心加载在后台**并行**进行（约 1.3s），
就绪后增量刷新版本/任务数。基准：`--startup-bench`（要求 `first_paint < 200ms`）。

关键优化：
- 执行器 worker 并行启动（原为串行，N 个执行器耗时线性叠加）
- MaaEnd agent（go-service / cpp-algo）并行拉起，且用 50ms 轮询替代固定 2.5s 空等
- `catalog` 30s TTL、`status` 0.4s TTL 缓存，避免每次快照做大 JSON 管道往返
- 退出时并行关停 worker（避免 MaaEnd agent 拖住进程）

首次启动会为每个**已安装的内部核心**创建一个默认执行器；没有核心时 UI 引导去「核心管理」拉取。
账号/核心/执行器持久化在 `build\state\`（`accounts.json` / `cores.json` / `executors.json` / `state.json`），
核心本体在 `build\cores\`。

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

- `--check` **ALL OK**：5 个执行器并发加载（3×MAA v6.13.0 + 2×MaaEnd v5.15.0-beta.1），
  **磁盘上仍只有 2 份核心**（多执行器共享同一 `core_id`，不重复拉取）
- **核心内部管理**：`--import-core` 把安装复制进 `build\cores\<kind>-<version>\`，
  之后完全不引用系统路径；远端（192.168.1.12）同样验证通过
- 更新检查真实返回 **MAA v6.20.1**（经 gh-proxy 镜像），下载地址解析正确
- 设置：代理串生成（`socks=host:port` / `http=host:port`）、CDK、LLM 配置落盘
- 性能：catalog 30s TTL 缓存、status 0.4s TTL 缓存，快照平均耗时 0ms
- MaaEnd 实例同样为 MAA 风格 UI，44 个任务正常列出

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
