# MaaHub C++（maa-hub-cpp）深度代码 Review 报告（修订版）

> **审查范围**：`src\` 全部 19 个源码文件（共 **14,142 行**，约 667 KB）、`build.bat`、`.gitignore`、`tests\headless_tests.py`、`docs\modules-overview.md` 与 `docs\review-20261112-lighten.md`。
> **真实仓库路径**：`D:\WorkSpace\maa-hub-cpp`（初稿误写为 `y:\maa-hub-cpp`，本版全部改为相对本仓的路径与经逐行核对的真实行号）。
> **审查原则**：只报告不改码；以「**代码简化（更少代码实现同等能力）**」「**代码模块化**」为主线，同时标出真实存在的并发、资源释放与逻辑对齐缺陷。
> **前置上下文**：`docs/review-20261112-lighten.md`（r1–r22）已完成局部消重与构建期优化（`kDrones/kFacility/kClients` 抽取、`util::random_hex`、`locale_files` 去重、`parse_url` 抽取、`/GL /LTCG /GR-`）；其后 r23（提交 `c083d22`，MaaEnd pretask 子进程 + 客户区尺寸校正）已提交但未在该文档登记。本次从**架构与模块级**继续深入。
> **修订说明**：本版对初稿做了逐条源码复核（4 路独立核查 + 逐行取证）。初稿的行号锚点绝大多数本来就对；被推翻或修正的结论集中列在 §0，正文位置已全部换成当前 HEAD（`4983cc2`）实测行号。

---

## 〇、对初稿的勘误（先读这节）

| # | 初稿说法 | 复核结论 | 证据（当前 HEAD） |
|---|---|---|---|
| E1 | 所有路径 `y:\maa-hub-cpp\...` | **错**。仓库实际在工作区 `D:\WorkSpace\maa-hub-cpp`。本文一律用 `src\...` 相对路径 | git 与文件系统实测 |
| E2 | 「无人机加速目标标签与核心枚举**错位**」（§二.5 第一条） | **推翻**。UI 标签序（不使用/贸易站/制造站/发电站/办公室/加工站/会客室）与 `kDrones`（`_NotUse/Money/SyntheticJade/CombatRecord/Chip/PureGold/OriginStone`）是**刻意的设施名↔产出物对照**：`hub.hpp:382-384` 注释明文声明「贸易站=Money 制造站=SyntheticJade 发电站=CombatRecord 办公室=Chip 加工站=PureGold 会客室=OriginStone」，`config_import.hpp:196-205`、`:480-484` 的反向映射与之一致，该语义被 r 系列 normalize 探针字节冻结。用户按「制造站」选择得到 `SyntheticJade`，与 MAA 官方 `DroneUsage` 语义一致，**不是缺陷**。保留一条低优先建议：面板标签表与 `kDrones` 仍是两份手工数据，漂移风险真实存在（见 §三.5） | `task_panels.hpp:413-416`、`hub.hpp:34-44`、`config_import.hpp:196-205` |
| E3 | 「`_download\tmp-<hex>` 遗留 GB 级 `.tmp`」 | **机制属实、命名有误**。真实临时目录是固定路径 `<cores_root>\.tmp`（`core_manager.hpp:484`），全仓不存在 `_download` 或 `tmp-<hex>` 命名。遗留点与初稿列的一致：6 处提前 return 均不清理（逐处行号见 §二.3）。另有初稿未提的：`remove_tree` 忽略 `SHFileOperationW` 返回值，被占用文件会静默留垃圾 | `core_manager.hpp:484-485, 489-491, 499-502, 517-526, 531-534, 586-591, 133-144` |
| E4 | 「`exec_thread_` 覆盖触发 `std::terminate()`；若上一次计划仍在运行或未曾调用 stop_execution()」 | **属实，且比初稿更严重**。`stop_execution()`（`hub.hpp:958-961`）确实有 joinable→detach，但**自然跑完的路径永远不会 join/detach**：`exec_worker` 结束后 `exec_thread_` 仍处 joinable，因此**只要不点 Stop，任何第二次 `execute_plan` 必 terminate**（无需并发、无需上一计划仍在跑）。`main.cpp:653-656` 的 `set_terminate` 记 crash log 后 `abort()`。已排查全仓其余 7 处 `= std::thread(...)`：`sched_thread_`（构造一次，析构 join-or-detach）、`backend_`（`create()` 单调用）、`reader_`（每个 WorkerClient 恰好一次 start）等均安全，唯 `exec_thread_` 裸奔 | `hub.hpp:942-956, 958-961, 1136-1225`；调用面 `app_window.cpp:2501-2503`（confirmed 态按钮仍可点） |
| E5 | 「先 LoadLibrary 后 AddDllDirectory，顺序反了」 | **顺序属实，影响降级为「防御性修正」**。两处 LoadLibrary 均带 `LOAD_WITH_ALTERED_SEARCH_PATH`（`maa_adapter.hpp:44`、`maaend_adapter.hpp:660,672`），该标志已让静态导入沿被载模块同目录解析，主路径实际能工作；真正暴露的是 ① 失败回退的裸 `LoadLibraryW`（`:45/:661/:673`）不含安装目录，② worker 侧从未调用 `SetDefaultDllDirectories`（全仓仅 `main.cpp:99` 的 `--core-version` 分支调过，且 `main.cpp:98` 注释自证「先设默认搜索策略再注册目录，反序 cookie 失效 err=87」），③ PATH 前置后从不还原。修法见 §二.3 | 见左 |
| E6 | 「`AsstAsyncAttachWindow` 失败仍干等 15 秒」 | **属实**（`maa_adapter.hpp:179-192` 只在 ok==true break）。追加一个更尖锐的疑点：匹配条件是 `it->task_id == call_id`，而 `ev.task_id` 取自回调 JSON 的 `"taskid"`（`:360`）；MaaCore 的 AsyncCallInfo 按约定用 **call id**，全仓没有任何代码读 `"callid"` 键，也没有 AsyncCallInfo 抓取样本可证实键名。**若键名真是 callid，则每次 win32 连接都白等满 15 s 再报 "attach rejected"**——需真机 connect 取证定案，修复时两个键名都要匹配 | `maa_adapter.hpp:178-192, 330, 356-360` |
| E7 | 「MaaControllerDestroy 从未调用；重复 connect 覆盖 ctrl_」 | **属实**，并补充：`connect()` 内 L961 赋值后的**每条失败 return 路径**（`maaend_adapter.hpp:965-968, 972-975, 977-980`）同样不清理，每次连接失败也漏一个 controller；`MaaControllerDestroy` 符号其实已解析（`:1256`），只是零调用 | `maaend_adapter.hpp:955-997, 749-769, 1198, 1256` |
| E8 | 「重复 `CreateProcessW+WaitForSingleObject` 共 2 处」 | **实为 3 处**：`core_manager.hpp:510-525`（tar）、`maaend_adapter.hpp:1051-1076`（run_pretask）、初稿漏掉的 `core_manager.hpp:246-288`（`probe_version`，自带管道读 stdout）。三处都绕过了现成的 `util::spawn_process / ProcHandle::wait_exit / util::run_capture` | 见左 |
| E9 | 「UTF-16→UTF-8 手写转换 55 行可整体换 `util::wide_to_utf8`」 | **半对**。`config_import.hpp:694-748` 中只有编码步（`:717-743`）重复；`:707-715` 的**字节序感知装配**（支持 UTF-16 BE）是 `wide_to_utf8` 做不到的真逻辑，必须保留 | 见左 |
| E10 | 「`new_uuid` 复用 `util::random_hex(16)` 格式化即可」 | **部分重复**。输出格式不同（UUID 段式 vs 无连字符 hex），且 `new_uuid` 的 `static mt19937_64` 无锁；收口到 `util` 时顺带继承 `random_hex` 的互斥保护，算安全修复而非纯消重 | `maaend_adapter.hpp:772-781`、`util.hpp:17-29` |
| E11 | 「page_copilot/page_toolbox 无法通过正常 UI 到达」 | **GUI 面属实**（tab 注释在 `app_window.cpp:537-541`），但 `--dump-render <png> <page>` **不校验页面白名单**（`main.cpp:691-695` → `app_window.cpp:136` 直接 `page_ = page`），两页仍可被离屏渲染到。属「隐藏半成品」而非「纯死代码」，清理决策要把这条后门算进去 | 见左 |
| E12 | 「每次重绘（TIMER_REFRESH 每 250ms + 鼠标悬停/移动）…」 | **每帧读写属实（P0 成立），触发源描述有误**。`WM_MOUSEMOVE` 只存坐标、不 Invalidate（`app_window.cpp:247-250`）；真实重绘节奏来自 `backend_loop` 空闲轮：`refresh_snapshot()` 递增版本 → `PostMessage(WM_APP_SNAP)` → `InvalidateRect` 全窗 + `Sleep(500)`，外加 250 ms 定时器按 `snap_version_` 变化条件触发（`:126, 293-308, 2811-2816`）。结论不变：执行器页开着，`task-params.json` 每 800 ms 被全量重写一次，与用户操作无关 | 见左 |
| E13 | 文件行数表 | **复核全部吻合**：`app_window.cpp` 3,439 / `hub.hpp` 1,514 / `maaend_adapter.hpp` 1,362 / `main.cpp` 975 / `core_manager.hpp` 950 / `config_import.hpp` 926 / `gui.hpp` 920 / `task_panels.hpp` 898；`worker_main.cpp` 实为 183 行（初稿把外围打包成 2,866/190 略有出入，不影响结论）。前 8 文件占 10,984/14,142 = **77.7%** ✓ | `src` 全量统计 |
| E14 | 「设置页底部按钮与状态提示重叠」 | **属实**：按钮 `content.left+156..+296`（y..y+30），提示文本同起点 x、y+6..y+28，文字直接压在按钮上 | `app_window.cpp:3303-3313` |
| E15 | 「`--dump-render` 作验收门」 | 存在且可用 ✓；**不存在** `--render` 旗标（核查补记）。基线实测（HEAD 干净重建）：`build.bat release` → EXIT=0 / 0 error / 13 条已知警告（C4996×6+C4244×7），`build\MaaHub.exe` = **1,467,904 B** | 本轮实测 |

---

## 一、代码规模与结构现状

| 模块 | 文件 | 行数 | 字节 | 职责堆叠现状 |
|---|---|---:|---:|---|
| 窗口与全部页面 | [app_window.cpp](../src/app_window.cpp) / [app_window.hpp](../src/app_window.hpp) | 3,439 + 292 | 166,476 + 13,511 B | Win32 消息/离屏 PNG 编码、后台调度、5 套状态文件持久化、8 个页面、硬编码中文任务表、右键/弹窗全在一个类 |
| 核心编排层 | [hub.hpp](../src/hub.hpp) | 1,514 | 65,698 B | 执行器 CRUD、worker 协议组装、任务参数归一化/默认值、任务序列执行器、定时调度器、4 份 JSON 落盘 |
| MaaEnd 适配器 | [maaend_adapter.hpp](../src/adapters/maaend_adapter.hpp) | 1,362 | 64,669 B | DLL/FFI 加载、locales i18n 缓存、`interface.json` 目录与 override 编译、Agent 子进程、Win32 控制器与 Tasker |
| 主入口与 CLI | [main.cpp](../src/main.cpp) | 975 | 46,009 B | 启动配置、20+ `--cli` 命令分发、6 个测试/基准/离屏调试模式、GUI 启动 |
| 核心下载与管理 | [core_manager.hpp](../src/core_manager.hpp) | 950 | 45,493 B | `cores.json` 管理、子进程版本探测、GitHub/Mirror 酱 API 轮询、WinHTTP 下载与 `tar.exe` 解包、SHA-256 |
| 配置导入 | [config_import.hpp](../src/config_import.hpp) | 926 | 45,263 B | UTF-16→UTF-8 解码、MAA `gui.json`/`gui.new.json` 解析、MaaEnd `mxu-MaaEnd.json` 解析、定时任务转换 |
| 自绘 GUI 库 | [gui.hpp](../src/gui.hpp) | 920 | 44,377 B | 主题配色、GDI/GDI+ 图元、矢量图标、组合框/输入/三态复选框/数字框/滚动区/裁剪栈 |
| 任务设置面板 | [task_panels.hpp](../src/task_panels.hpp) | 898 | 45,969 B | MAA 13 个定制面板 + MaaEnd 面板 + 通用 `catalog` 参数表单生成器 |
| 基础与外围 | `maa_adapter.hpp`(486)、`util.hpp`(562)、`json.hpp`(455)、`planner.hpp`(438)、`accounts.hpp`(262)、`worker_client.hpp`(194)、`settings.hpp`(183)、`worker_main.cpp`(183)/`worker_main.hpp`(7)、`adapter.hpp`(96) | 2,866 | ~130 KB | 工具函数、WinHTTP 重复、各模块自管 JSON 持久化 |
| **合计** | **19 个文件** | **14,142** | **667,440 B** | **前 8 个文件占全仓 77.7% 行数**；仅 3 个 `.cpp` 编译单元（`build.bat` SRCS：main / worker_main / app_window），其余全在 `.hpp` 内联 |

---

## 二、优先处理：已发现的真实缺陷与并发/资源隐患

> 以下问题均已在 HEAD 逐行对证（初稿勘误见 §0，此处只保留成立项）。建议先于大规模重构单独修复并做回归。

### 1. [P0] UI 绘制循环中无条件读写持久化文件
- **位置 1**：`src\app_window.cpp:1792-1833`
  - 执行器页每次重绘都会：
    1. `hub_->task_params(id, sel_task)` 加锁拷贝当前任务全部参数并逐键覆盖 `ui.params`（1796-1801）；
    2. `draw_task_panel` 之后再遍历 `ui.params` 的每个键调用 `hub_->set_task_param(...)`（1830-1833）；
    3. `Hub::set_task_param`（`hub.hpp:1031-1038`）无条件下调 `schedule_save_task_params()`——即使**零变化**，每满 800 ms 把整份 `state\task-params.json` 全量 `dump_pretty` + 原子重写 + `FlushFileBuffers` 一遍（`hub.hpp:1384-1407`、`util.hpp:407-445`）。
  - 重绘驱动源是 `backend_loop` 的快照版本广播（约 250–500 ms 一帧，`app_window.cpp:2811-2816` + `WM_APP_SNAP:306-308`），鼠标移动不触发（勘误 E12）。页面开着 = 后台持续写盘。
- **位置 2**：`src\app_window.cpp:1010`
  - `draw_post_action` 弹窗打开期间**每帧**无条件 `save_post_action(id)` 全量重写 `state\post-actions.json`（`:1040-1046`，且 `(void)id` 忽略入参）。`gui_.checkbox3` 本身返回 `changed`（`gui.hpp:647-655`），当前被丢弃。
- **修法**：加载只在事件上做——首次选中（已有先例 `task_ui_init_` insert-once，`app_window.cpp:1616-1620`）、执行器/任务切换（切换条件在 `:1502-1515` 已经算过）、导入完成（`WM_APP_IMPORT:309-337`）、`flush_task_params` 之后。写回改成变更驱动：给 `PanelCtx` 的 `set_bool/set_int/set_str`（`task_panels.hpp:33/48/55`）置 `dirty` 标记，只回写变化键；`draw_post_action` 聚合 `changed` 后再落盘。

### 2. [P1] 并发安全与 `std::thread` 生命周期
- **`exec_thread_` 覆盖触发 `std::terminate()`**：`src\hub.hpp:954`。比初稿判断更强：**完成路径不清 joinable**，第二次 `execute_plan`（不点 Stop）必崩；UI 在 confirmed 态仍允许再次执行（`app_window.cpp:2501-2503`）。修法：赋值前 `if (exec_thread_.joinable()) exec_thread_.detach();`，或 `exec_worker` 尾部自 detach / 由调度方 join。
- **无同步节流时间戳**：`last_tp_save_`（`hub.hpp:1496`）、`last_save_`（`:1503`）为裸 `uint64_t`，被 UI 线程（`set_task_param→schedule_save_task_params:1384-1389`、`flush_task_params:1095-1098`）、backend 线程（`config_import` 批量写参）、每个 executor 一条的 worker 事件回调线程（`on_worker_event→schedule_save:677→:1456-1461`）无锁读写——形式数据竞争。同文件里 `schedule_save_schedules()`（`:1340-1348`）已示范正确做法（`sched_save_mtx_` 保护）；把这两个也换成 `std::atomic<uint64_t>` 或同款互斥即可。
- **锁外写文件**：`save_task_params():1405` 与 `save_state():1473` 都在**所有锁之外** `write_file`。叠加上一条节流竞态，两线程可同时进入：`util::write_file` 对 `<path>.tmp` 以 share=0 打开（`util.hpp:426`），失败方**回退为非原子的直接 `CREATE_ALWAYS` 写目标文件**（`util.hpp:429-433`），与胜方的 `MoveFileExW` 交错 → 丢更新或半写。`executors.json`/`schedules.json` 的写入在持锁路径内，不受此影响（初稿把这四份打包批评，范围略有超出）。修法：把「快照序列化 + write_file」整体圈进一把 per-file 保存锁。
- **`core_msg_` 跨线程裸读写**：写方 backend/动作线程 `app_window.cpp:2846-2850`（`set_notice` 先赋值**后**才进 `snap_mtx_`，锁根本没罩住赋值）；读方 UI `:2275-2280`（page_cores）、`:3307-3313`（page_settings）均无锁。`std::string` 撕裂读/扩容中途读是实际风险。修法：用 `upd_mtx_` 或新 `notice_mtx_` 包住写与两处读，或把提示并入快照下发。

### 3. [P1] 句柄泄漏、DLL 路径顺序与大文件残留
- **DLL 搜索路径时机**（按 E5 降级为防御性修正）：`src\adapters\maa_adapter.hpp:41-66`、`src\adapters\maaend_adapter.hpp:655-694`。统一改成 `main.cpp:98-105` 已验证的顺序：`SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS)` → `AddDllDirectory` → `LoadLibraryExW`；PATH 前置那段建议整体删除（`AddDllDirectory` 生效后它就是冗余，且从不还原）。
- **`MaaController` 句柄泄漏**（E7）：`maaend_adapter.hpp:955-997` 每条失败路径与 `unload():749-769` 都不销毁 `ctrl_`；符号已解析未调用（`:1198, :1256`）。`disconnect()` 两处（`maa_adapter.hpp:205`、`maaend_adapter.hpp:1000`）只改状态字符串，且下一次 `is_connected()` 轮询（`maa:207-209` / `maaend:1002-1004` 走真实 API）会立即把假状态洗回 true。修法：`connect()` 开头与 `unload()` 补 `MaaControllerDestroy`；`disconnect()` 真正销毁并置空，让 `is_connected()` 如实回报。
- **attach 失败干等 15 s + 关联键疑云**（E6）：`maa_adapter.hpp:178-192`。修法：匹配到任何同名 AsyncCallInfo 事件即终止等待（成功/失败分开置位）；匹配键同时接受 `"taskid"` 与 `"callid"`。
- **`.tmp` GB 级残留**（E3）：`core_manager.hpp:484-613`。提前 return 且不清理的 6 处：`:489`（下载失败）、`:499`（哈希失败，**完整 zip 已落盘**）、`:517-519/:526`（tar 启动失败/非零退出）、`:531`（包内未找到核心目录，**zip+全量解包双份都在**）、`:586-590`（MoveFileEx 失败）。修法：函数入口放一个 scope-guard（`[&]{ remove_tree(tmp); }`），成功路径在 move 之后自然无事；`remove_tree:133-144` 顺带检查 `SHFileOperationW` 返回值并 `util::trace` 失败原因。

### 4. [P1] `LlmPlanner::chat` 忽略代理与错误响应解析失效
- **位置**：`src\planner.hpp:242-322`
  1. `:243-244` 硬编码 `WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`，完全不使用 `CoreManager::open_session`（`core_manager.hpp:810-835`）已实现的 `AppSettings` HTTP/SOCKS5 命名代理与 `WINHTTP_OPTION_PROXY_USERNAME/PASSWORD` 认证——设置了代理的用户「更新能走代理、LLM 不走」；
  2. 全程未查 `WINHTTP_QUERY_STATUS_CODE`（`core_manager.hpp:869-871` 的 http_get 有现成写法），非 2xx 也按 JSON 解析；
  3. `:314` `j.get_str("error", "no choices in response")`——OpenAI/DeepSeek 错误体是 `{"error":{"message":"..."}}`（对象），`json.hpp:87-90` 的 `get_str` 对非字符串一律回退默认值，真实错误永远被吞成 "no choices in response"。同文件 `json.hpp:101-117` 的 `find_path/get_path_str` 就能取 `"error.message"`，当前零使用。
- **修法（顺带消掉三份 WinHTTP 之一）**：`chat` 改走 `open_session` + 状态码检查；错误提取 `get_path_str("error.message", 再试 get_str("error", "http <code>"))`。

### 5. [P2] 逻辑错位与未接入后端的「空转」UI 控件
- ~~无人机标签错位~~ → **不成立**（E2）。保留一条防漂移建议：把「设施名↔枚举」对照收敛到单一声明（如 `kDrones` 旁并排 `kDronesZh`），面板与导入两侧从同一处取，杜绝下一次「注释说一套、表画一套」。
- **两处表格时间显示为当前时钟**：`app_window.cpp:2059`（账号「最近运行」——2045-2046 明明读出了 `last_run_at` 却弃用）与 `:2534`（计划历史「时间」——行对象自带 `ts`，`planner.hpp:40/52/73`→`hub.hpp:967` 快照里就有）。修法：新增 `util::fmt_time(double epoch)`（现成范式见 `app_window.cpp:1327-1335`）格式化条目自身时间。
- **设置页按钮与提示重叠**（E14）：`app_window.cpp:3303-3313`。修法：提示移到 `content.left + 312` 或下一行。
- **只存盘不执行**：`post_action_`（`app_window.cpp:961-1056`，UI 里还会以「完成后：锁屏、关机」字样展示给用户，`post_action_desc:1014-1037`，**误导性比纯死控件更强**）与 `auto_reload_`（`:1930-1934`，连持久化都没有，重启即复位）全工程无执行/下发路径（`InitiateSystemShutdown/SetSuspendState/ExitWindowsEx` 全仓 0 命中；`post-actions.json` 只有写 `:1044` 读 `:1050` 两点）。需要产品决策：在 `run_task_sequence` 收尾处接执行器，或摘掉入口。
- **i18n 缓存锁定首次路径**：`maaend_adapter.hpp:140-302`。`locale_flat():140-155`、`option_label():183-197`、`case_label():212-237` 各自持有函数内 `static bool loaded`，进程内首次加载后永久锁定第一次的 `label_dir_ref()`；`set_label_install_dir`（`:308`，由 `discover:42` / `load:657` 调用）改目录后**不会**让缓存失效。首次加载 `locale_files()` 被三个函数各调一遍：3 次全目录遍历 + 每份 `zh_cn.json` 解析 3 次。修法：共享一个 `LocaleData{dir, files, 三份 map}`，dir 变了才重载，一次遍历三表同出。

---

## 三、代码简化与消重机会（用更少代码做同样的事）

> 各项均可在不改变外部契约的前提下削减合计约 **1,200–1,600 行**。

### 1. 抽取统一网络层 `net/http.hpp`（三处 WinHTTP 样板收口，约 −130~−160 行）
- `core_manager.hpp:836-881`（`http_get`）、`:883-942`（`http_download`）、`planner.hpp:242-322`（`chat`）。三份手写 `WinHttpOpen → URL 拆分 → Connect → OpenRequest → Send → Receive → 循环 ReadData → 每个早退分支手动 CloseHandle`。
- planner 这份是**已发生危害的重复**：它复刻时丢了代理配置与状态码检查（§二.4）。`parse_url` 已在 `core_manager.hpp`（r13 抽出，`:780-807` 一带），planner 却在 `:249-266` 又手写一遍内联拆分。
- 建议：RAII `WinHttpHandle` + `http::request(method, url, headers, body, const AppSettings*, progress_cb / file_sink)`；`chat` 变成 ~10 行调用，代理/状态码/错误体解析随基础层一次到位。

### 2. Win32 工具归拢进 `util.hpp`（约 −150~−200 行）
| 重复实现 | 当前位置（HEAD 实测） | 合并去向 |
|---|---|---|
| 手写 `FindFirstFileW` 遍历，4 处 | `maaend_adapter.hpp:118-138`（locale_files，含嵌套探测）、`core_manager.hpp:108-131`（copy_tree）、`:703-713`（find_core_dir BFS）、`:735-744`（rebuild_from_cores_dir） | `util::list_entries(dir, dirs_only)`（util.hpp 目前**没有任何**目录枚举助手，这就是四处各自手写的原因） |
| 手写 `CreateProcessW`+`WaitForSingleObject`，3 处（E8） | `core_manager.hpp:510-525`（tar）、`maaend_adapter.hpp:1051-1076`（run_pretask）、`core_manager.hpp:246-288`（probe_version） | `util::run_process(cmd, work_dir, timeout_ms, hide)`；至少复用现成的 `util::spawn_process:208-284` / `ProcHandle::wait_exit:190-193` / `run_capture:455-476` |
| UTF-16 BE→LE + 编码，`config_import.hpp:694-748` | 其中 `:717-743` 编码段与 `util.hpp:31-37` `wide_to_utf8` 重复（E9：`:707-715` 字节序装配是真逻辑，留） | 换 `util::wide_to_utf8(w)` |
| 自建 `mt19937_64`，`maaend_adapter.hpp:772-781`（new_uuid） | 与 `util::random_hex`（`util.hpp:17-29`）同 RNG 模式，且缺后者带的互斥（E10） | `util::random_uuid()`（hex(16)+插桩），顺带修线程安全 |
| 列表拆分/标量转换 | `config_import.hpp:29-115`、`hub.hpp:412-441`、`task_panels.hpp:852-860` | 统一 `util::split_tokens(s, delims)` |

### 3. Adapter 公共基类 + 表/宏驱动 FFI 绑定（约 −200~−250 行）
- `maa_adapter.hpp` 与 `maaend_adapter.hpp` 重复：load 里的 DLL 目录注册块、`status()/task_catalog()/drain_events()/disconnect()`、`ev_mtx_/events_/cfg_/st_` 维护 → 提炼 `AdapterBase : IAdapter` + `DllGuard`（配 §二.3 的顺序修正一起做最省）。
- `maa_adapter.hpp:261-311` 与 `maaend_adapter.hpp:1176-1281` 共约 160 行手写「成员声明 + `ok &= sym(fn_.X, "X")`」；`#define LOAD_SYM(f)` 或表驱动可压到 1/3。
- **解析但全仓零调用的导出，实测 15 个**：maa 侧 `AsstCreate(:262)、AsstSetStaticOption(:268)、AsstSetTaskParams(:270)、AsstLog(:276)、AsstGetImage(:279)、AsstGetNullSize(:280)`；maaend 侧 `MaaTaskerStatus(:1186)、MaaTaskerWait(:1187)、MaaTaskerInited(:1188)、MaaResourceStatus(:1195)、MaaStringBufferSize(:1207)、MaaAgentClientCreateTcp(:1213)、MaaAgentClientAlive(:1217)、MaaAgentClientIdentifier(:1219)`，外加初稿没列的 `MaaControllerDestroy(:1198)`——**这一个不许删**，它是 §二.3 泄漏修复要用的。注意每个成员都配 `ok &=` 硬门槛：删成员必须同步删 sym 行；删掉冷门导出还能顺带放宽旧核心「缺导出即 load 失败」的脆性（`maa:51-53`、`maaend:667-669`）。

### 4. 统一 `JsonStore` 持久化组件（消除十余份手写 load/save，约 −220~−280 行）
- 现状：每个类各自「读文件 → `try_parse` → 回填 → `dump_pretty` → `write_file` → 自管锁」——`accounts.hpp`、`settings.hpp`、`core_manager.hpp(cores.json)`、`hub.hpp` 内 4 份（`executors/task-params/schedules/state`，函数与节流明细见 §二.2）、`app_window.cpp` 内 4 套同构读写（`save/load_post_action:1040-1056`、`save/load_task_list`、`save/load_task_checks`、`save/load_task_alias`）。
- 另一个隐性事实：`state.json` **只写不读**——全仓没有 `load_state()`，它是纯历史快照文件；四份 Hub 持久化里它最该进 JsonStore 或降级为日志。
- 建议：~60 行 `JsonStore`：原子写（复用 `util::write_file` 但要**去掉静默的非原子回退**，改为失败重试+trace）、`.bak` 兜底（当前只有 `settings.hpp` 有）、脏标记节流（把 §二.2 的 atomic 节流收进来）、per-file 保存锁。`app_window.cpp` 4 套可并成一份 `ui-state.json` 或共用泛型 `load/save_string_sets`。

### 5. 数据驱动替代大段重复 if 与 UI 样板（约 −300~−400 行）
- `display_name`：`app_window.cpp:763-850` 实测 88 行、82 条链式 `if (task == "...")`；maa 分支 13 条硬编码（`:765-777`），maaend 分支先查 locales 动态表（`:779-781`，表在 `:734-760` 随快照重建）再落 68 条硬编码兜底。改 `static const std::unordered_map<std::string_view, const wchar_t*>`（maa/maaend 各一张，按 kind 选表），行数减半、每帧线性扫描变 O(1)。
- 页/表渲染样板：`page_accounts/page_cores/page_planner` 的列头/分隔线/`row_fully_visible` 滚动剔除大量重复；`page_settings` 手工维护 22 个 `set_*_` 成员（`app_window.hpp:231-251`）并与 `AppSettings` 双向搬运两遍（`app_window.cpp:3085-3120` 读侧、`:3319-3361` 写侧），直接编辑一份 `AppSettings edit_settings_` 副本可省 ~80 行。
- `draw_task_panel` 分发：`task_panels.hpp:872-896` 20 个 `if (c.task == ...)` → 函数指针表。
- 面板选项与 `hub.hpp:34-44` 三张表：现状「贸易站=Money…」是**正确**的官方语义（E2），但仍是两份手工数据——从同一处生成标签（见 §二.5 建议），把漂移风险关掉。

### 6. CLI / 诊断模式与主入口解耦
- `main.cpp:647-649` 在入口最顶端无条件 `GdiplusStartup`（且全仓无 `GdiplusShutdown`；勘误：入口是 `main` 不是 `wWinMain`，`:645`）——`--worker`、`--cli`、`--core-version` 等纯控制台模式也白白初始化 GDI+。改为进入 GUI / `--dump-render` 分支时 RAII 初始化。
- `main.cpp:32-82` `load_config()` 仍保留 `D:\Application\MAA`（:42）/ `D:\Application\MaaEnd`（:54）兜底；系统已全面转向 `CoreManager`（`cores\`），本机这两个目录也已不存在（实测 False），该兜底只剩误导。
- `run_cli` 实测 `main.cpp:158-643`（485 行、23 个 `if (cmd == ...) return` 顺序块）→ 「命令名 → handler」注册表；`task-lists.json` 读写与 UI 共用同一套辅助接口，防 CLI/GUI 队列口径分叉。

---

## 四、代码模块化与重构方案（核心架构建议）

### 1. 解除「纯头文件架构」，改为 `.hpp` 声明 + `.cpp` 实现
- 现状：除 `main.cpp / worker_main.cpp / app_window.cpp` 外 **16 个模块全部在 `.hpp` 内联**（3 个编译单元）；改任意一行 `hub.hpp`/`util.hpp`，三个 TU 全部重编（release 还带 `/GL /LTCG`，整程序重分析）。`docs/review-20261112-lighten.md` 的历史注记显示团队已多次规避 /GL 跨 TU 发码膨胀。
- 建议：重实现逐步移入对应 `.cpp`；worker 子进程只链接 `adapters + json + util + http`，保持 `--worker` 依赖面最小（现在 `worker_main.cpp:13-18` 只 include adapters/json/util，已达标，模块化时守住这条线）。

### 2. 四大巨石文件拆分蓝图

```text
src/
├── core/                      # 基础设施与领域状态（无 Win32 GUI 依赖）
│   ├── json.hpp/.cpp
│   ├── util.hpp/.cpp          # 补 list_entries / run_process / random_uuid / fmt_time
│   ├── http.hpp/.cpp          # 统一 WinHTTP RAII + 代理 + 状态码 + 流式下载（CoreManager & LlmPlanner 共用）
│   ├── store.hpp/.cpp         # 通用 JsonStore（原子写 / .bak / 节流脏标记 / per-file 保存锁）
│   ├── settings.hpp/.cpp
│   ├── accounts.hpp/.cpp
│   ├── core_manager.hpp/.cpp  # 版本探测、安装与更新元数据（scope-guard 清 .tmp 后 ~450 行）
│   └── config_import.hpp/.cpp
├── engine/                    # 编排与调度
│   ├── task_normalizer.hpp/.cpp # kDrones/kFacility/kClients（+中文标签同源）+ 归一化 + 默认值 + 顺序
│   ├── scheduler.hpp/.cpp     # merge_schedules / scheduler_tick / schedules.json
│   ├── planner.hpp/.cpp
│   ├── worker_client.hpp/.cpp
│   └── hub.hpp/.cpp           # 执行器编排、快照与事件（从 1,514 行降到 ~550 行）
├── adapters/                  # 核心运行时适配（worker 侧）
│   ├── adapter.hpp/.cpp       # IAdapter + AdapterBase（事件队列/状态/DllGuard）
│   ├── maa_adapter.hpp/.cpp
│   ├── maaend_i18n.hpp/.cpp   # locales 单次解析 + dir 变更重载（替代 3 份 static 缓存）
│   ├── maaend_interface.hpp/.cpp # interface.json 目录发现与 pipeline_override 构建
│   └── maaend_adapter.hpp/.cpp   # 纯 MaaFramework + Agent 生命周期（从 1,362 行降到 ~400 行）
├── gui/
│   ├── gui.hpp/.cpp
│   ├── task_panels.hpp/.cpp   # 面板控件 + 分发函数指针表
│   ├── window_shell.cpp       # 消息循环、双缓冲与离屏 PNG、左栏、顶栏、GIF 挂件
│   ├── page_executor.cpp      # 三列实例页、任务拖拽/勾选/右键/别名弹窗（脏标记参数回写）
│   ├── page_managers.cpp      # 账号 / 核心 / 规划 / 事件 / 设置页
│   └── window_backend.cpp     # backend_loop、快照同步、post_action 队列
└── app/
    ├── cli.cpp                # --cli 23 个子命令表驱动分发
    ├── diag.cpp               # --check / --self-test / --startup-bench / --dump-render（页面白名单）
    ├── worker_main.cpp
    └── main.cpp               # 仅参数解析与路由（~80 行；GDI+ 进 GUI 分支才初始化）
```

---

## 五、死代码与仓库清理清单

1. **半成品页面（155 行）**：`app_window.cpp:2090-2210`（page_copilot，121 行）+ `:2212-2245`（page_toolbox，34 行）；tab 入口注释于 `:537-541`。注意 E11：`--dump-render <page>` 不校验页面名，两页离屏仍可渲染——要么给 dump 加白名单再谈删除，要么移特性分支。`copilot_path_/copilot_opt_`（`app_window.hpp:117-118`）只被这两页使用、从不持久化、从不进 Hub。
2. **未使用的 FFI 导出**：§三.3 的 15 个实测清单（`MaaControllerDestroy` 除外——它是 §二.3 的修复件）。
3. **测试/诊断钩子与生产窗口耦合**：`app_window.hpp:43-60` 的 `debug_*` 接口族与 `main.cpp:700-701` `--dump-update-demo` 等建议 `#ifdef MAAHUB_DIAG` 隔离（注意 `--dump-render` 是四重验收门的组成部分，隔离时保留）。
4. **本地垃圾与残留（全部未被 git 跟踪，清理不产生仓库变更）**：
   - `_cmp\`：实测 **300 个文件 / 203.1 MB**（历史像素对照截图 + 16 个 `splice_*.py` 一次性脚本），`.gitignore:43` 已忽略——本地归档/删除即可；
   - `D\`：空残留目录（实测 0 项），可删；
   - `vc140.pdb`（根目录 1,445,888 B，`.gitignore:5` 已忽略）与 `build\flagtest\`（在已忽略的 `build\` 内）：本地清理项。

---

## 六、量化收益预估与实施优先级

| 阶段 | 内容 | 预计源码变化 | 风险与回归策略 |
|---|---|---|---|
| **第 1 步：修真实 Bug** | 每帧读写盘（§二.1 两处）、`exec_thread_`（§二.2）、节流戳与锁外写（§二.2）、`core_msg_`（§二.2）、DLL 顺序与 PATH（§二.3）、`MaaControllerDestroy` 全路径补漏（§二.3）、attach 早退+双键匹配（§二.3/E6）、`.tmp` scope-guard（§二.3）、`LlmPlanner` 代理/状态码/`error.message`（§二.4）、`fmt_time` 两处时间列 + 设置页重叠（§二.5）、locales 缓存随目录失效重载（§二.5） | 约 ±60 行 | **极低风险**；`build.bat release`（门1）+ normalize 探针（门2）+ `tests\headless_tests.py` 沙箱（门3）+ `--dump-render` 全页差分（门4：executor/accounts/cores/planner/settings 基线 PNG 已在本轮建立） |
| **第 2 步：横切层抽离** | `http.hpp`、`store.hpp`、util 四件套（list_entries/run_process/random_uuid/fmt_time）、`AdapterBase` + FFI 宏表 + 删 15 个死导出、`display_name` 查表化、settings 表单直接编辑 `AppSettings` 副本 | 约 −950 ~ −1,250 行 | 低~中风险；每拆一件跑一次四重门 |
| **第 3 步：大文件模块化拆 `.cpp`** | `app_window.cpp`→4–5 文件、`maaend_adapter.hpp`→i18n/interface/runtime、`hub.hpp`→normalizer/scheduler/hub、`main.cpp`→cli/diag/main；同步 `build.bat`（或转 CMake/Ninja 保增量） | 结构重整，再消 −300 ~ −500 行；单文件最大 3,439 → ~700 | 中风险；`/GL /LTCG` 下跨 TU 变化务必重跑门 2/4 |
| **第 4 步：功能/仓决策** | `post_action_`/`auto_reload_` 接执行还是摘 UI（产品决策，当前状态对用户有误导）；`state.json` 只写不读的存废；dump 页面白名单；清理 `_cmp\`(203 MB)/`D\`/`vc140.pdb` | −~160 行 + 本地 203 MB | 低风险，需逐条确认 |

---

### 附：本轮修订的工作底账
- 行号基准：HEAD `4983cc2`，工作树干净；全文件 raw 行数统计（含空行）与初稿表格一致。
- 基线构建：`build.bat release` EXIT=0，13 条已知警告（C4996×6 + C4244×7，与 r22 登记口径相同），`build\MaaHub.exe` = 1,467,904 B（r23 后未重建过，本轮刷新）。
- 基线截图（`build\_review1115\`，沙箱 state/cores 副本，未触碰 `build\state` 本体）：executor(Infrast)/accounts/cores/planner/events/settings 六页 PNG + MD5，供第 1 步差分门复用。
- 初稿逐条判定汇总：**成立** §二.1(两处)/二.2(三条)/二.3(除命名)/二.4(三条)/二.5(除无人机项)/§三 全部方向/§五.2/.3；**修正** E1–E15 各条；**推翻** 仅「无人机标签错位」一项（E2）。
