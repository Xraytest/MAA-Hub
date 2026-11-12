# MaaHub 源码深度评审（2026-11-11）

评审对象：`D:\WorkSpace\maa-hub-cpp\src`（16 个 .hpp + 3 个 .cpp，约 15.7k 行）
触发背景：Maa1 验证跑出「第一顺位关卡不是 TL-4」的缺陷，要求对代码做深度 review，找出更多错误。
方法：逐文件精读 + 运行时证据对照（`asst.log` 的 `append_task` 实发参数、`state\*.json` 现场），
跨模块结论另做对抗复核；本文只收录**已用源码行号或运行日志证实**的缺陷。

状态：第 1 批（进程/IPC 层、适配器层、并发与状态机）已定稿；模块评审与跨模块对抗的其余条目见 §4。

---

## 1. 结论摘要

| # | 级别 | 位置 | 一句话 |
|---|------|------|--------|
| A1 | **P0** | `hub.hpp:351-431` | `normalize_task_params` 无条件用面板字段覆盖核心原生字段，导入/CLI 写入的 `Infrast.mode/drones`、`Recruit.confirm/select` 被静默改写 |
| A2 | **P0** | `hub.hpp:436-452` + `maa_adapter.hpp:441-455` | 缺参时用目录默认值补齐，默认值本身是"无操作"语义（`stage:""`、`Recruit.times:0`），整串任务"成功"但什么都没做 |
| A3 | **P0** | `hub.hpp:664-670`、`1067-1073`、`610-617` | 持 `mtx_` 调 `worker->call()`，而事件回调 `on_worker_event → get_executor` 也要 `mtx_`（代码自己在 `1105-1106` 写了这条禁令）→ 每次调用等满超时，目录/计划静默失败 |
| A4 | **P0** | `hub.hpp:639` vs `642-645` | `on_worker_event` 无锁遍历 `subscribers_`，`subscribe` 在锁内 `push_back` → 迭代中扩容 = UB |
| A5 | **P0** | `task_panels.hpp:223-236,240-245` + `config_import.hpp:152-156` | 「启用使用药剂/源石/指定次数」三个开关只写布尔键，对应的数量键无门控地照发：**关掉开关照样吃药、照样只打 N 次** |
| B1 | P1 | `maa_adapter.hpp:228` | `disconnect()` 只改状态字符串，从不真正断开；`st_.conn` 报 "disconnected" 是假话 |
| B2 | P1 | `maa_adapter.hpp:243/250-251` vs `:268-271` | `st_` 在命令线程与 MaaCore 回调线程并发写、`status()` 加锁读，`std::string` 数据竞争 |
| B3 | P1 | `maa_adapter.hpp:393-400` | `TaskChainStopped` 未处理：用户 Stop 后 `run` 永远停在 "running" |
| B4 | P1 | `util.hpp:293-296` | `spawn_process` 直接覆盖 `out` 里的旧句柄，同一 client 二次 `start()` 泄漏两条管道并留下第二个 core |
| B5 | P1 | `worker_client.hpp:123-133` | `shutdown()` 仅优雅退出时关句柄；kill/超时路径 detach 读线程且不关句柄，读线程可能读到被复用的句柄 |
| B6 | P1 | `worker_client.hpp:44-53` | 就绪超时返回 false 但不杀子进程 → 孤儿 worker，重试即两个 core 抢一个设备 |
| B7 | P1 | `worker_main.cpp:173` | stdin EOF 直接 `return 0`，不 `unload()`：DLL / `asst.log` 句柄 / adb 连接留在孤儿进程（已实测 798 s 卡死） |
| B8 | P1 | `worker_client.hpp:68-72,79` | 调用参数可覆盖 `cmd`/`id` 信封（应答无法关联、等满超时）；写失败路径不清理 `pending_` |
| B9 | P1 | `main.cpp:371-380`、`app_window.hpp:220` | 导入把任务名并进 `std::set`，**队列顺序丢失**；MAA 的 TaskQueue 顺序无法还原（Maa1 实跑顺序与 MAA 配置不同） |
| B10 | P1 | `adapter.hpp:80`、`hub.hpp:629` | 事件环形缓冲满 1000/500 时静默丢弃最旧的一半；长链跑动中 `asst.log` 关键事件可能在 Hub 侧不可见 |
| B11 | P1 | `hub.hpp:1225-1229` | `default_connect` 用函数级 `static` 缓存 adb 探测结果，模拟器后启动则永久退化为 win32，直到重启 Hub |
| C1 | P2 | `json.hpp:81-84,67,61-64` | 非 const `operator[]` 强制把节点改成 Object；`items()` 不校验类型；`as_string()` 用静态空串 —— 三类类型错误全部静默 |
| C2 | P2 | `json.hpp:96-99` | `get_int/get_bool/get_str` 严格按类型取，`"6"` 这类数字字符串直接落到默认值（`TaskQueue[i].Hour`） |
| C3 | P2 | `json.hpp:449-464` | `try_parse` 吞掉所有异常（含 `bad_alloc`），只返回 false 无诊断；导入解析失败会得到空对象继续跑 |
| C4 | P2 | `maa_adapter.hpp:452-453` | 目录把 `Recruit.select/confirm` 标成 required 却给 `[]` 默认值，与 A2 组合产出"空 Tag"请求 |
| C5 | P2 | `maa_adapter.hpp:74-77` | `resolve_symbols()` 失败早退不 `FreeLibrary`，重试会累加引用计数 |
| C6 | P2 | `maa_adapter.hpp:169-177,200-215` | `connect()` 靠扫历史 `events_` 找 uuid / AttachWindow 回执，可能读到上一次连接的残留；这些事件之后还会被 `drain_events` 再发一遍 |
| C7 | P2 | `worker_main.cpp:121` | `req["params"]` 是可变 `operator[]`：非对象 params 会被强制改成 `{}` 并继续执行，而不是报错 |
| C8 | P2 | `maa_adapter.hpp:236-243` | 参数通道纯透传、无键名白名单也无回执校验，写错键名（如 `use_medicine`）只会在核心侧被静默忽略 |

---

## 2. P0 详解

### A1 `normalize_task_params` 无条件覆盖核心原生字段

**机制**（`hub.hpp:351-431`）。函数本意是"把面板字段翻译成核心字段"，但四个分支里三个是无条件写：

```
:366-367   normalize 把面板语义的 mode 整数 0/1/2 改写成核心枚举 0/10000/20000，随后
           params["mode"] = <枚举值>          ← 不看 params 里原本是什么
:368-373   params["drones"] = kDrones[drones_idx]，drones_idx 默认 0 = "_NotUse"
:388-410   params["confirm"]/["select"] 由 confirm_3..6 / select_tags 重建
:378-386   只有 facility 分支带 touched 守卫（唯一正确的写法）
```

真正的问题不是"翻译错了"，而是**它把已经是核心格式的参数当面板格式再翻译一次**。
导入的 `Infrast` 参数用的是核心键（`mode:10000`、`drones:"PureGold"`），过一个
`normalize` 就变成"默认模式 + 不用无人机"。

**运行证据**：`asst.log` 实发
`append_task Infrast {"drones":"_NotUse","facility":[],"mode":0}`，
而 Maa1 配置是 `Mode="Normal"`、`UsesOfDrones="PureGold"`。核心接受了这个请求
（键名合法），所以链路照常 `TaskChainCompleted`——**幻觉式的成功**。

**修法**：三个分支全部加 `touched` 守卫（照抄 `:378-386`），只有面板键存在时才翻译；
核心原生键优先。

### A2 缺参用"无操作"默认值补齐

`fill_defaults`（`hub.hpp:436-452`）只做加法：`out` 里没有的键补上目录默认值。
目录默认值（`maa_adapter.hpp:441-455`）里有一串"看起来合理、实际什么都不做"的值：

| 任务 | 键 | 默认值 | 语义 |
|---|---|---|---|
| Fight | `stage` | `""` | 打**上一次**的关卡（本次事故的直接机制） |
| Fight | `times` | `2147483647` | 打到理智耗尽 |
| Fight | `series` | `1` | 开代理倍率（MAA 面板 `Series=0` 的语义未逐一核对，此处只作"默认值与配置不符"的记录） |
| Recruit | `confirm` / `select` | `[]` | 不选 Tag、不确认 → 只计算不招募 |
| Recruit | `times` | `0` | 仅计算 |

`task_params()`（`hub.hpp:985-994`）对没有条目的执行器返回 `{}`，
于是 `run_on_executor` 把整套默认值发下去。**这就是 Maa1 打 1-7 的完整链路**：
导入器没读到 6.x `TaskQueue` → `task-params.json` 没有 Maa1 条目 → 全默认 →
`stage:""` → 客户端沿用上次的 1-7。

**现场数据**（2026-11-11 22:19 读取 `build\state\task-params.json`）：
6 个执行器里只有 2 个有条目——

- `exec-02913292d1eb`（MaaEnd）：MaaEnd 原生参数，正常；
- `exec-8b1b54dccbeb`（旧的 MAA 制品导入）：只有 `Fight`，且键名混杂——

```json
"Fight": { "stage": "", "times": 999, "medicine": 0, "series": 1, "drops": {},
           "use_medicine": false, "use_stone": false, "allow_stone_save": false,
           "perform_battles": false, "assigned_material": false, "candidate_stages": "" }
```

后六个键**不是 MAA core 的任务字段**（core 的 `Fight` 只有
`stage/medicine/stone/times/series/drops/report_to_penguin`，见 `asst.log` 实发参数），
是某个旧导入器照 MAA **GUI 字段名**写进去的。参数通道是纯透传
（`maa_adapter.hpp:236-243`），没有白名单也没有回执校验，
所以这些键被核心静默忽略：`use_medicine:true` 不会让核心吃药，`stage:""` 会沿用它上次的关卡。
**写错键名不会报错，只会静默不生效**——这是比"值不对"更隐蔽的一类缺陷。

- Maa1（`exec-84691d0fba02`）与 Maa5（`exec-5fa8dd2145ee`）**完全没有条目**，
  即这两个执行器的每一次运行都是目录默认值。


**修法**：① 导入必须真正写出参数（见 §3）；② `fill_defaults` 对"语义性键"
（`stage/times/confirm/select`）不做静默补齐，宁可让该步失败并报"参数缺失"。

### A4 持 `mtx_` 调 worker，与事件回调互相阻塞

代码在 `hub.hpp:1105-1106` 明确写着这条禁令：

> start/discover/load 必须在 mtx_ 之外调用——worker 事件回调（on_worker_event）也要拿 mtx_，持锁调用会死锁。

但两处违反：

```
hub.hpp:665-670   propose_plan:  { lock_guard lk(mtx_); for (...) w->call("tasks", 15000, r); }
hub.hpp:1067-1073 refresh_catalogs: { lock_guard lk(mtx_); ... w->call("tasks", 15000, r); }
```

而 `on_worker_event → get_executor`（`hub.hpp:610-617` → `180-186`）确实要 `mtx_`。
`WorkerClient` 是**单读线程**：事件行和应答行走同一条 stdout。于是

1. GUI/命令线程持 `mtx_` 发 `tasks`；
2. 读线程先读到一行**事件**，进 `on_worker_event`，卡在 `mtx_`；
3. 应答行排在这个事件后面，永远轮不到处理 → `call()` 等满 15 s 返回 false。

结果：任务目录拉不到（`catalog` 保持旧值/空）、`propose_plan` 失败一次并留下 15 s 僵直。
`exec_worker` 里每步开头的 `status` 轮询也在跑，事件源源不断，撞上的概率不低。

**修法**：把 `call()` 全部移到锁外（先 `worker_of()` 取 shared_ptr，再调用）；
`on_worker_event` 里 `get_executor` 改用无锁快照或在锁内只拷贝 label 字符串。

### A4 `subscribers_` 无锁遍历

```
hub.hpp:639   for (auto& cb : subscribers_) cb(*he);        // 读线程，无锁
hub.hpp:642-645 subscribe(): { lock_guard lk(ev_mtx_); subscribers_.push_back(...); }
```

`subscribe` 在锁内 `push_back` 而遍历不加锁 —— 边跑边订阅时 `std::vector` 可能重分配，
迭代器失效 = 崩溃。GUI 侧确实会在启动/切页时注册回调。

**修法**：遍历前在锁内拷贝一份 `subscribers_`；或改用 `std::shared_mutex`。

### A5 "启用"开关被丢弃，数量键无条件生效

`task_panels.hpp:223-236` 的 `cb_num` 把"启用"和"数量"分成两个键存：

```
c.set_bool(key, st == CheckState::On);      // use_medicine / use_stone / perform_battles
int nv = c.get_int(nkey, def);
c.set_int(nkey, nv);                        // medicine / stone / times —— 与开关状态无关
```

`:240-245` 三处调用即 `use_medicine+medicine`、`use_stone+stone`、`perform_battles+times`。
`config_import.hpp:152-156` 也把这两类键各自照抄。而**没有任何一层**把
`use_medicine=false` 翻译成 `medicine=0`（全仓只有这两处出现这几个键，见 grep）。

结果：Maa1 配置 `UseMedicine=false, MedicineCount=5`、`UseStone=false, StoneCount=0`、
`EnableTimesLimit=false, TimesLimit=5`
→ 导入后 `medicine=5`、`times=5`
→ core 收到 `{"medicine":5,"times":5}`，**真的吃 5 颗药、只打 5 次**。
用户关掉的开关完全不生效。

注意这条与 A2 互补：A2 是"没有参数时用默认值"，A5 是"有参数时不做布尔门控"。
两者都会让 Hub 的实际行为偏离 MAA 面板上的显示。

**修法**：`normalize_task_params` 增加门控——
`use_medicine` 为 false ⇒ `medicine=0`；`use_stone` 为 false ⇒ `stone=0`；
`perform_battles` 为 false ⇒ `times` 取"无限"（与目录默认一致），否则取 `times` 原值。
导入端同时写 `use_*`，让门控有依据。

---

## 3. 与本次验证直接相关的两条

### B9 队列顺序在导入时被丢弃

- `main.cpp:371-380`：`std::set<std::string>` 求并集 → **按字典序**，且"只增不减"；
- `app_window.hpp:220`：GUI 侧 `task_list_` 也是 `std::set`；
- `hub.hpp:700-726` `canonical_task_order`：只做两个例外（StartUp 提到最前、CloseDown 放到最后），中间顺序照抄输入。

MAA 的 `TaskQueue` 是有序的（Maa1: StartUp → Recruit → Infrast → Fight → Mall → Award）。
Hub 实际跑的顺序（`state\task-lists.json`）是
`StartUp, Award, Fight, Infrast, Mall, Recruit` —— 字典序 + StartUp 提前。
顺序不同会改变游戏状态（先 Fight 后 Infrast 会改变基建心情/理智的分配）。
这与 A2 是**同一个根因**：6.x `TaskQueue` 没有被读取。

### B11 adb 探测结果被永久缓存

```
hub.hpp:1225-1229
  static const util::AdbDevice dev = [] { util::AdbDevice d; util::detect_adb(d); return d; }();
```

`default_connect` 每个执行器都要调，但静态变量只初始化一次：**Hub 启动时模拟器没开，
之后所有 MAA 执行器都按 win32 挂窗处理**，直到重启。反之亦然。
而且首次调用是在锁外的 GUI 线程，`detect_adb` 要跑 `adb devices`（秒级），
会和 A4 的 15 s 超时叠加成"启动卡住"。

**修法**：探测结果带 TTL（如 30 s）或按需重探；连接失败时强制重探。

---

## 4. 待合并

- 模块评审（json / util / hub / main / import / accounts / cores / app_window / gui / task_panels / planner / ipc / settings）
- 跨模块对抗（concurrency / persistence / security / silent-failure）
- 每条的独立复核结论（confirmed / adjusted / refuted）

合并时统一编号，并把被推翻的条目剔出正文，只在附录保留"曾怀疑但不成立"的清单，
避免把猜测当缺陷交付。

> **2026-11-12 状态更新**：上述两个 fan-out 评审 workflow（v1 `workflow-16f2b0dbe2c549c2`、
> v2 `workflow-d3288faf99b44972`）分别因失败与卡死（>24 h 停在"跨模块对抗"）被弃用，
> §4 的批量合并**不会发生**。本轮改为逐条直接实证：A1/A2/A5/B9 已在 11-12 的
> TaskQueue 导入版中修复并实机部署（见 `verify-20261112-deploy.md`），其余条目
> （A3/A4/B* / C*）维持原状、未在本轮处理。

## 4.1 已修复条目（2026-11-12 TaskQueue 导入版，编译 + 本机实测 + WorkCenter 实机部署）

| # | 修复 | 位置 | 验证 |
|---|---|---|---|
| A1 | `drones` 不再无条件覆盖：normalize 加 `in.find("drones_idx")` 存在性门；`kDrones` 扩 7 档（补 PureGold/OriginStone），面板 radio 同步扩 7 项 | `hub.hpp` normalize Infrast 分支；`config_import.hpp` kDrones；`task_panels.hpp` drones_idx | 探针：`drones_idx=5` → `drones:"PureGold"`；仅有 `drones:"Money"` 无 drones_idx 时原样保留 |
| A2 | 6.x `TaskQueue` 真正被读取：新增 `import_task_queue()`，maa1/3/5 各写 36 参数（stage/times/confirm/drones/facility/Award 7 键…），不再落 fill_defaults 空参数 | `config_import.hpp`（MAA 段）+ `import_maa` 的 `key=="TaskQueue"` 拦截 | 本机三份真实配置导入逐项吻合 `evidence\taskqueue-maa{1,3,5}.json`；部署后远端 task-params 三执行器 6 任务全落盘 |
| A5（导入侧） | 布尔门控在导入时完成：`medicine = UseMedicine ? MedicineCount : 0`（stone 同理），maa1 `UseMedicine=false` → `medicine=0` 实落盘 | `config_import.hpp` import_task_queue Fight 分支 | 远端核验：maa1 `medicine:0, use_medicine:false`；maa3/5 `medicine:5, use_medicine:true`。**面板侧 `cb_num` 的无条件写仍未加门控**，本条只闭环了导入路径 |
| B9 | 任务序列随导入写入 `c.task_lists`；`canonical_task_order` 对 maa 全序化 `StartUp→Recruit→Infrast→Fight→Mall→Award`（未知任务排中段后、CloseDown 末），maaend 保留 CloseGamePC 置末 | `hub.hpp` canonical_task_order | 探针 ORDER-MAA/MIX/END 三例全对；落盘 task-lists 虽经 std::set 字典序，运行期由该函数重排回官方序 |
| Mall buy_first | normalize 补 `buy_first_str → buy_first` 数组（原先只转 blacklist，hub.hpp:412 注释与代码不符的坑一并修） | `hub.hpp` Mall 分支（split_list lambda） | 探针：`blacklist:["加急许可"] buy_first:["招聘许可"]`；远端字节级确认 UTF-8 正确、无双编码 |
| StartUp client_type | 面板 int 下标 0..5 → MaaCore 字符串枚举，非法回退 `Official`；导入取 `Gui.RuntimeSettings.ClientType` 映射下标 | `hub.hpp` StartUp 分支 | 探针 SU0/SU2/SU9/SUS/SUBAD/SUEMPTY 六例全对 |

部署侧闭环（非代码缺陷，但属 A2 的现场后果）：maa1/maa5 补齐账号绑定（180/152）、
maa3 新建执行器并绑 151，三者统一 MuMu adb connect 模板；原 MAA 客户端进程/自启清零。


---

## 5. 附录：评审中推翻的猜想

这一节记录**看起来像缺陷、实际不成立**的怀疑，防止后续重复踩坑。

### R1 `exec_worker` 会不会把"链内任务切换"误判成"整体完成"？

**不成立**。初看 `maa_adapter.hpp:399` 把 `TaskChainCompleted` 一律当 `idle`，
而 MAA 的 `TaskChainCompleted` 是每个链内任务完成都发一次，似乎会让
`hub.hpp:1183` 的"1 秒 idle 就收工"提前触发。

但 Hub 的 `run_on_executor`（`hub.hpp:454-488`）**每步只 append 一个任务**再 `AsstStart`，
所以每一步的链里只有 1 个任务；`TaskChainCompleted` 就是这一步真正结束。
本次 Maa1 跑出的 6 次 `TaskChainCompleted` 对应 6 次独立 append，时间跨度
18:57:42→19:12:16 与 `SCHED done` 吻合。要触发误判，必须让一次 `AsstStart` 带多个任务，
当前代码路径不会这么做。**已从正文剔除。**

### R2 MaaCore 回调线程与命令线程的 `st_` 竞争会不会导致崩溃？

**部分成立，但不是"回调不加锁"**。`on_callback`（`maa_adapter.hpp:392`）确实持
`std::lock_guard<std::mutex> lk(st_mtx_)` 包住全部状态写入，且 `:401` 在锁释放后
才 `emit`，所以"持 `ev_mtx_` 时回调再进 `status()`"这类死锁不存在。
真正的竞争在另一侧：`run_task`（`:243/250-251`）、`connect`（全函数）**不加锁**写
`st_.conn/uuid/last_error`，而 `status()`（`:268-271`）加锁读同一个 `std::string`。
即"读在锁内、写在锁外"，仍是 UB，但触发窗口小、后果是脏读而非必然崩溃。
按 B2 严级记录，不升级为 P0。

### R3 事件只在命令返回时才 drain，长链期间 Hub 看不到进度？

**不成立**。`worker_main.cpp:157-171` 确实只在每次命令后 drain，但
`exec_worker`（`hub.hpp:1161-1184`）在等待期间每 500ms 发一次 `status`，
每次都把子进程事件队列搬过来。所以任务运行中事件是持续流动的。
真正会丢事件的是**无人轮询的空闲期**（GUI 关闭 + 核心在跑），
以及环形缓冲的静默截断（B10）——那是另一条问题，不是"只在命令后 drain"。

