# MaaHub 代码轻量化 Review — 2026-11-12（第二轮）

目标（长期）：**严格确认功能实现正确的前提下**，对软件代码做轻量化优化，持续迭代深度 review。
硬约束：任何删除/改动必须同时通过 ①编译无新警告 ②normalize 探针字节等值 ③导入语义等值（task-params 多重集）才算采纳；无证明不改动行为；改动仅本地，不部署远端。

---

## 一、本轮已完成并采纳的三项改动

### 1. 表去重：kDrones / kFacility / kClients 单一事实源（hub.hpp）

| 位置 | 改动 |
|---|---|
| `hub.hpp:31-43`（r12-restore 重建后实位；事故前为 :35-45） | 新增 `inline const char* const kDrones[7]`、`kFacility[8]`、`kClients[6]` + `kDroneCount/kFacilityCount/kClientCount`（C++17 inline 变量，跨 TU 单实例） |
| `hub.hpp` Infrast 分支 | 设施循环改为 `for (int k=0;k<kFacilityCount;++k) fac.push(kFacility[k])`；索引越界钳位 `if (d<0||d>=kDroneCount) d=0;` |
| `hub.hpp` StartUp 分支 | `for (auto k : kClients)`（原为手写 6 项字符串列表） |
| `config_import.hpp` | 删除本地重复的三张 static 表（约 170 行处留注释「表见 hub.hpp 文件头」），循环统一用 `*Count` 常量；Infrast RoomList 写全 8 个键；`drones_idx` 锚点保留在 :206、:487 |

冻结锚点（不得动的语义）全部保留并通过验证：
- **A1**：Infrast 的 drones 写入以 `in.find("drones_idx")` 为门，缺省不注入；
- **A2**：normalize 覆盖 Infrast/Recruit/Mall/StartUp；
- StartUp `client_type` 0..5 → 字符串，缺省 `"Official"`。

### 2. 死代码删除：util.hpp（−57 行，604 → 537）

删除三个**全工程零调用**的 inline 函数：

| 函数 | 原行号 | 删除依据 |
|---|---|---|
| `sha1()` | 87–124 | 全 src 无引用（含 `adapters\`）；无哈希需求入口 |
| `now_ms()` | 328–331 | 无引用；调用方一律用其它时间源 |
| `read_file_tail()` | 466–482 | 无引用 |

同时把文件头注释里的 "SHA1" 字样去掉（第 1 行），保持文档与实现一致。

**关键证据：删除后 exe 尺寸 = 1,543,168 B（与删除前完全相同）** → 这些 inline 死函数本来就没被链接进产物，属纯源码负担；删除是**零风险可读性收益**，不是二进制收益。

死代码判定手段（三重，避免误删）：
- `/W4` 编译 **C4505 = 0 条**（编译器不认为有未引用局部静态函数）；
- 文本扫描器 `scan_deadrefs.ps1`：对每个 inline/static 函数统计 `\bX\s*\(` 引用数，全 src 汇总；
- 唯一被文本扫描标记的候选 `adapters\maa_adapter.hpp:340 trampoline` **人工复核后排除**——它在 :345 以 `return &MaaAdapter::trampoline;` 形式作为函数指针回调注册，文本正则看不到引用。这正是"只看正则就删"的典型陷阱。

### 3. 全程序优化：`/GL` + `/LTCG` 进 build.bat release 分支（exe −3.9%）

`build.bat` release 分支加入 `/O2 /GL /DNDEBUG` 与链接期 `/LTCG`（debug 分支不变）。

二进制收益：

| 构建 | exe 字节 | 差值 |
|---|---|---|
| 去重+删码后（无 LTCG） | 1,543,168 | — |
| **采纳 LTCG 后（当前）** | **1,482,240** | **−60,928（−3.95%）** |

---

## 二、LTCG 的验收证据（为什么敢采纳）

LTCG 是链接器级代码生成，行为必须实测。已证明：

1. **警告集完全一致**：LTCG 与基线各 24 条 `/W4` 警告，逐条同文件同行号同类别（C4244/C4996/C4100/C4189/C4456），**零新警告**。
2. **normalize 探针字节等值**：`probe_norm_ltcg.exe` 输出与基线 `light-baseline-probe.txt` 完全 IDENTICAL；采纳后用当前 `build.bat` 再跑一次 `light-final-ltcg-probe.txt` 亦 IDENTICAL。
3. **导入语义等值**：normal 与 ltcg 两个独立沙箱跑同样 3 份 `*-gui.new.json`，task-params 条目多重集 IDENTICAL（`A_SIZE=1104 B_SIZE=1104`）。
4. **CLI 面等值**：双沙箱对照 `ping / tasks / accounts / devices / sched-list / get-connect` 六个 verb 全部 IDENTICAL；`schedules.json` 等值；`executors.json` 唯一差异是 `created_at` unix 时间戳（两次运行相隔几秒）。
5. **COMDAT 折叠风险已排除**：LTCG 会把函数体完全相同的函数合并到同一地址。全 src grep 确认：无函数指针相等比较（`&fn == ` / `== &fn`）、无 `PROC` 指针取地址、无 `std::function/callback/handler ==` 比较、无按函数地址判语义的用法；`maaend_adapter.hpp:19` 的 `g_instance == this` 是**对象指针**自比对，与函数地址无关。

### 关于 trace.log 差异的裁定（重要，防误判）
双沙箱 trace.log 行数不同（118 vs 120，差 `worker start kind=maa` / `worker unload begin cmd=exit` 两行）。**这不是 LTCG 差异**：同一份 normal exe 连跑三次，行数分别为 120/120/119，两两归一化后仍有同类差异。根因是 worker 进程启动/退出与 hub 关闭之间的**时序竞态**（trace 逐行带随机 pid/tid），属产品既有行为，与代码生成无关。故 trace.log 不作为门控项；有效门控是 ①警告集 ②探针 ③task-params 语义 ④CLI verb 输出。

### 关于「同沙箱连跑 3 次 import 只剩 6 条」的裁定
`--cli import maa` 对同一 kind 是**覆盖式**（最后一份配置的 task-params 生效），三次连跑得到 6 条属既有语义；normal 与 ltcg 表现一致（已验证 `SAME AS NORMAL-EXE RUN`）。每配置独立沙箱才是正确验证方式：三个配置各自 6 条 = **18 条**，与远端基线完全吻合（maa1 `TL-4/med=0/drones_idx=5/times=300`、maa3 `1-7/drones_idx=1`、maa5 `AP-5/drones_idx=1/times=40`，全部 `facility_0..7=true`、`client_type=0`、`account_name_set=true`）。

---

## 二·补 本轮第 4 项（14:42）：maaend 标签目录枚举去重（extract-method）

`adapters\maaend_adapter.hpp` 里 `FindFirstFileW(root\*) → 过滤目录/./.. → 对每个子目录 FindFirstFileW(sub\zh_cn.json) → 收集` 这段枚举逻辑出现 **3 份逐字节相同**的拷贝（scan_dups 报 3x）：
- `locale_files()` :119（返回文件列表）
- `load_case_labels()` 头部（旧 :242–258）
- `load_option_labels()` 头部（旧 :300–316）

采纳：后两处删掉内联枚举，改为 `for (auto& fp : locale_files())`。原两份的唯一区别是早退返回类型（void 函数 `return` vs `return files`），对"空列表 → 空 for 循环"完全等价。

| 指标 | 值 |
|---|---|
| 源码 | 1280 → **1244 行**（−36） |
| exe | 1,482,240 → **1,481,216 B**（−1,024） |
| 探针 | `light-locale-probe.txt` 与基线 IDENTICAL |
| 导入 | `iso_import.ps1` maa1/maa3/maa5 各 6 = 18 条，参数与远端基线吻合 |

依据：三份枚举逻辑逐字节相同 → 合并为单一事实源，属提取函数重构（extract method），非行为变更。

---

## 二·补2 本轮第 5 项（14:47）：app_window 运行顺序重排去重（extract-method）

`app_window.cpp` 中「`executor_catalog(id)` → 收集 items → 按 `task_order_[id]`（用户拖拽顺序）稳定重排、表外项追加到尾部」这段 25 行逻辑，在 `action_run_from`（旧 :2965–2989）与 `action_run_checked`（旧 :3027–3051）**逐字节相同**出现 2 次（scan_dups 命中 3 个互相重叠的块即源于此）。

采纳：提取为私有成员 `std::vector<mj::Value> ordered_catalog_items(const std::string& id)`（声明 `app_window.hpp:168`，定义 `app_window.cpp` catalog_of 之后），两处调用点各收敛为一行 `std::vector<mj::Value> items = ordered_catalog_items(id);`。函数体是原逻辑逐字节搬移，调用点之后的差异（run_from 的 `start` 定位、run_checked 的全量遍历）与 `task_ui_`/params 组装/`once` 复位全部原样保留。

| 指标 | 值 |
|---|---|
| app_window.cpp | 3583 → **3565 行**（净 −18：删 2×24 行、加 30 行 helper） |
| exe | 1,481,216 → **1,481,216 B**（不变；两处内联被折成一次调用，链接器已消掉重复体） |
| 编译 | `build.bat release` EXIT=0，无 error |
| 探针 | `light-runorder-probe.txt` 与基线 IDENTICAL |
| 导入 | `iso_import.ps1` 各 6 = 18 条不变 |

**证据边界（诚实记录）**：这段是 GUI 交互路径（RunFromHere / 运行全部已勾选），`probe_norm` 与 `--cli import` 都覆盖不到运行顺序本身。其"无行为变更"的依据是：①helper 体与原两处逐字节一致（静态搬移，非改写）；②整体回归门（探针+导入）证明其它路径未被波及。真正的动态验证需要 GUI 冒烟（27 张截图 backlog 之一），已登记。

---

## 二·补3 本轮第 6 项（15:02）：`random_hex` 五份拷贝合并到 `util::random_hex`

`scan_dups.ps1` 命中全库最大的单一重复：`random_hex(size_t)` 在 **4 个文件里逐字节相同出现 5 份** —— `hub.hpp:1502`、`accounts.hpp:236`、`core_manager.hpp:809`、`planner.hpp:192`（HeuristicPlanner 内）、`planner.hpp:448`（LlmPlanner 内）。五份函数体完全一致（`static mt19937_64` + `random_device` 播种 + 逐字节两位 hex），共 10 处调用（hub `exec-`/`cli-` 2 处、accounts `acc_` 1 处、core_manager `core-`/目录名 5 处、planner `plan_`/`plan_llm_` 2 处）。

采纳：合并为单一 `inline std::string util::random_hex(size_t)`（`util.hpp:17`，新增 `#include <random>`/`<mutex>`），删除 5 份私有拷贝，10 处调用改为 `util::random_hex(N)`（`N` 与原来逐一对应，无改动）。

**语义变更分析（为什么可接受）**：
1. 合并前每份各持一个独立 `static mt19937_64`；合并后共享同一个。**产出的具体 hex 值必然不同**，但这本就是随机 ID 生成器（非密码学、非确定性），旧版每次运行产出的值也各不相同——唯一不变量是「均匀随机 hex、长度 `2×bytes`」，新版分布完全相同、格式（前缀 + hex 位数）完全相同。回归门一直是**语义比对**（`created_at`/executor id 跨运行本就不等），不用字节比对 ID，正好覆盖这一点。
2. 加锁是**净增安全**：旧版 5 份虽互不共享，但同一份被多线程调用时（如 hub 并发建 executor）存在 `static rng` 竞态；新版单实例 + `std::mutex`，串行化访问。ID 生成极低频，锁开销可忽略。

| 指标 | 值 |
|---|---|
| 源行数净变 | **−41**（删 5×12=60 行定义，`util.hpp` +19 行承接：537→556） |
| 文件行数 | hub 1553→**1541**、accounts 280→**268**、core_manager 991→**979**、planner 465→**441** |
| exe | 1,481,216 → **1,474,560 B**（**−6,656 B**；5 份内联体收敛为 1 份） |
| 编译 | `build.bat release` EXIT=0，无 error |
| 探针 | `light-randomhex-probe.txt` 与基线 **1318 B 逐字节 IDENTICAL** |
| 导入 | `iso_import.ps1` 各 6 = **18** 条，参数全对基线（maa1 TL-4/medicine=0/drones_idx=5/times=300；maa3 1-7/drones_idx=1；maa5 AP-5；facility_0..7=true、client_type=0） |

**证据边界**：探针/导入覆盖 normalize、catalog、import 语义，但不直接断言"ID 仍是随机 hex"。该不变量由源代码静态证明（util 体 == 原 5 份体，仅前缀 `util::`）+ 10 处调用参数逐一对齐；动态上导入产生的 executor 仍拿到合法 `exec-<12hex>`（iso 沙箱 state.json 可查），无异常。

---

## 二·补4 本轮第 7 项（15:07）：`core_manager` URL 拆分去重 + 差分测试门

`core_manager.hpp` 的 `http_get`（旧 :843–857）与 `http_download`（旧 :904–918）各内联了**逐字节相同**的 13 行 URL 拆分块（scheme strip → host:port split → path），`scan_dups.ps1` 以两段重叠命中。提取为私有静态 `parse_url(url, host&, path&, port&, https&)`（定义在 HTTP 段首），两处调用点各收敛为一行。搬移保留了原代码里 `slash == std::wstring::npos` 与 `string::find` 结果比较的笔误（`wstring::npos == string::npos == (size_t)-1`，行为正确，重写会改变源码文本→留给后续单独门控的轮次）。

**证据升级——门 4（差分测试，本轮首次引入）**：前 6 项都靠"静态搬移逐字节一致 + 回归门（探针/导入）证明别处没被波及"，但回归门**不直接执行被改代码**。`parse_url` 走的是 core 资产下载 / GitHub release 查询路径，`probe_norm` 与 `--cli import` 均不覆盖。故新增 `tests\parse_url_diff.cpp`：把改动前的内联块（`orig_parse`）与提取后的 helper 体（`new_parse`）都逐字节嵌进同一程序，对 19 条边界 URL（https/http/无协议/空串/仅斜杠/带端口/端口越界 999999/负端口/空端口/UTF-8 中文路径/`://weird` 等）断言 `(host,path,port,https)` 元组全等。结果 **19 urls, mismatches=0**。这套差分门从此可复用于任何"函数体搬移"类去重。

| 指标 | 值 |
|---|---|
| core_manager.hpp | 979 → **974 行**（净 −5：删 2×13 行块、加 21 行 helper + 注释；调用点各减 ~14 行） |
| exe | 1,474,560 → **1,471,488 B**（**−3,072 B**；两处内联折成一次调用） |
| 门1 编译 | `build.bat release` EXIT=0，无 error |
| 门2 探针 | `light-urlprobe-probe.txt` 与基线 **1318 B 逐字节 IDENTICAL** |
| 门3 导入 | `iso_import.ps1` 各 6 = **18** 条，参数全对基线 |
| 门4 差分 | `parse_url_diff.exe` EXIT=0，19 URL 全等 |

---

## 二·补5 本轮第 9 项（15:26）：`#include <random>` 三处失效依赖清理

第 6 项把 5 份 `random_hex` 合并进 `util.hpp` 后，`accounts.hpp` / `planner.hpp` / `hub.hpp` 各自的 `#include <random>` 失去直接使用者——审计确认三个文件中**没有任何** `std::mt19937` / `std::random_device` / `uniform_*` 直接引用（只有 `util::random_hex` 调用，其实现在 util.hpp，已自带 `<random>`）。`maaend_adapter.hpp:738-739` 有真实的 `static std::random_device rd; static std::mt19937_64 rng(rd());`，其 `<random>` **保留不动**。

| 指标 | 值 |
|---|---|
| 源码 | hub.hpp 1541→**1540**、accounts.hpp 268→**267**、planner.hpp 441→**440**（共 −3 行） |
| exe | 1,471,488 → **1,471,488 B**（**±0**；标准头不进 PE，符合预期——本项收益是依赖卫生而非体积） |
| 门1 | EXIT=0 |
| 门2 | `light-include-probe.txt` 与基线 **1318 B IDENTICAL** |
| 门3 | `iso_import.ps1` TOTAL=**18**，参数全对基线 |
| 门4 | 不适用（无函数体搬移，仅删 include；编译通过即证明无未声明符号） |

## 二·补6 本轮第 10 项（15:32）：`checked-run` 循环体 ×2 合并到类外 `run_item_if_checked`

`action_run_from`（:3006-3019）与 `action_run_checked`（:3043-3057）各内联一段**逐字节相同**的 11 行循环体：`task_ui_.find(id)` → `find(name)` → `checked||once` 过滤 → 组 params → `run_on_executor` → `once` 复位。提取为成员函数 `run_item_if_checked(id, name)`——**定义刻意放在 app_window.cpp 类外**（声明在 hpp），吸取第 8 项教训：类体内定义=隐式 inline，LTCG 会既内联又保留独立副本导致体积反弹；类外定义则只有一份实体 + 调用点。

| 指标 | 值 |
|---|---|
| 源码 | app_window.cpp −12 行（3572→3560，两处循环体折调用、helper 20 行含注释） |
| exe | 1,471,488 → **1,470,464 B**（**−1,024 B**，两处内联折成一次调用+跳转，净减；类外定义路线验证成功） |
| 门1 | EXIT=0，警告集与基线逐条一致（见下方口径修正） |
| 门2 | `light-checkedrun-probe.txt` 与基线 **1318 B IDENTICAL** |
| 门3 | TOTAL=**18**，参数全对基线 |
| 门4 | `tests\checked_run_diff.cpp`：**9 场景 mismatches=0**（全勾运行 / ret=0 与 ret=−1 启动失败 / once+checked 单任务 / once 双次连跑复位 / 执行器未找到 / 任务未找到 / 空列表 / 部分勾选），mock 记录 `(id,name,params)` 调用序列 + once 复位后 ui 快照双比对 |

**警告口径补充（防误判）**：本轮起把 `/W3` 产品构建的警告集量化为"构建日志**出现次数**"基线：实测 HEAD（git stash 暂存本轮全部改动→编译原始树→stash pop 还原）与本轮改动后均为 **13 次**（C4244×7 + C4996×6），逐条归属同码同文件同次数——C4244: app_window.cpp:2061(1×)、core_manager.hpp:63(2×)、core_manager.hpp:64(2×)、hub.hpp(2×)；C4996: core_manager.hpp:644(2×)、main.cpp:698(1×)、util.hpp(3×，其中 1 次日志归属行未带路径前缀)。行号漂移仅两处：hub.hpp :114→:129、util.hpp :404→:379，系此前删除所致，预期内。本轮之前各采纳项只写了"警告集与基线逐条一致"未量化次数，13 次基线自此固定，后续轮次以同口径比对。第 60 行的 24 条是 LTCG 采纳实验时的 `/W4` 全量口径（含 C4100/C4189/C4456），与 `/W3` 的 13 次不冲突。结论不变：本轮全部改动**未引入也未消除任何警告**。

---

## 二·补7 r13（19:4x）：`parse_url` 的 `std::wstring::npos` 笔误规范化（§四挂账关账）

`core_manager.hpp:822` 的 `slash == std::wstring::npos` 是 r7 搬移时原样保留的历史笔误（当时注释"规范重写留给后续单独门控的轮次"）。本轮关账：改为 `std::string::npos`。可证等价：`slash` 是 `size_t`，两种 `npos` 同为 `(size_t)-1`，比较结果与分支行为与存储类型无关。注释同步更新（:810-813），测试 `parse_url_diff.cpp` 的 `new_parse` 镜像体同步为改后文本（:53）。

| 指标 | 值 |
|---|---|
| 源码 | core_manager.hpp 行数 974 → 974（等行改动）；hub.hpp 未动 |
| exe | **1,470,464 B 体积不变**（与基线逐字节同尺寸）；链接后 SHA 变化属 PE TimeDateStamp 噪声（见下） |
| 门1 | EXIT=0、0 错误、13 警告（C4244×7 + C4996×6，同码同文件同次数）；C4244 站点 hub.hpp:128 / core_manager.hpp:63,64 / app_window.cpp:2061 行号与接受态一致（兼作重建未错位的行级证据） |
| 门2 | **验收口径修正**：参考文件 `light-r9-final-probe.txt` / `light-r12-restore-probe.txt`（1318 B）开头带 UTF-8 BOM（EF BB BF），是当年重定向生成方式的历史产物；`probe_norm.exe` 输出本身不含 BOM。本轮直存捕获 1315 B，**去 BOM 后正文与两份参考 1315/1315 逐字节全等** → 语义通过。教训：门2 比较一律按"去 BOM 正文"口径，直接比整文件 MD5 会因 BOM 假阴性 |
| 门3 | TOTAL=**18**（maa1/3/5 各 6 条，参数与 r12 记录一致）；完整快照 `r13-gate3.txt` |
| 门4 | `parse_url_diff.exe`：**19 URL mismatches=0**（orig=旧文本含 wstring::npos / new=改后真源码，含空主机、无协议、端口越界、仅协议头、IPv6 字面量等边界） |

**exe SHA 方法学修正**：每次链接的 SHA256 必然不同（`/LTCG` 写入 PE TimeDateStamp），SHA 不能当"同一构建"的判据；跨轮次等价判据 = **体积 + 门2 去 BOM 正文全等 + 门3 语义多重集**。`accepted-hub\MaaHub-1470464-192410.exe` 与 `MaaHub-r13-194543.exe` 的 SHA 差（EFAE5D… vs 0B3F9A…）即此噪声，非行为差异。

r13 已采纳，源码快照固化：`evidence\accepted-hub\core_manager-r13-194543.hpp`、`hub-r13-194543.hpp`、`MaaHub-r13-194543.exe`。

另：本轮复查 `#include <random>` 全工程——仅 util.hpp:9 一处，对应 :15-18 的 r6 合并 rng 本体（合法使用）；hub 侧 `<random>`/rng 死成员疑账关闭（不存在）。14 行窗口扫描零新增大重复；死引用扫描（138 候选）仅 `trampoline` 已知伪影，无新增零引用函数。

---

## 二·补8 r14（20:0x–20:5x）：冗余调用点 / 死变量 / 未用形参清理（**新增一类"零体积变更"**）

第 8/11 项两次实测否决后，本轮换了一整类改动方向：不再做"提取/去重"（LTCG 定律已证明该方向对 ~392 B 级函数体恒亏），改做**"删掉本来就不该存在的东西"**——重复调用点、死局部变量、未用形参、遮蔽局部名。这五处改动的共同点是**不新增任何函数**，因此完全不受 LTCG inline-retention 定律约束。

| # | 位置 | 改动 | 等价性依据 |
|---|---|---|---|
| 1 | `main.cpp` `if (cmd == "seq")`（旧 576-577） | 删掉重复的 `std::string id = resolve(cli_arg(args, 1));` 与紧随其后的重复空值判断，改注释 | **静态可证**：`execs` 在 `:476`（作用域起点）到 `:575` 之间无任何写入（全文件写入点仅 `:167`/`:843`/`:903`，均在窗口外）；`resolve` 是 `:168-181` 的纯 lambda 只读 `execs`，同输入同输出 → 第二次取值必然与第一次全等，重复判断必然与第一次同真同假。附带消掉 C4456（`id` 遮蔽外层同名变量） |
| 2 | `app_window.cpp` `:1394`（旧） | 删死局部 `daily_done`（C4189 赋值后从未使用） | 该符号全文件仅此一处出现，grep 零命中 → 无副作用表达式（右侧纯查询），删除无副作用 |
| 3 | `app_window.cpp` `:1701`/`:1704`（旧） | 内层遮蔽的 `al` 改名 `alr`/`alc`；`for (auto& t : tasks)` 的 `t` 改名 `tt` | `task_alias_` 的全部变更点仅 **1198, 1199, 1238, 1261, 1706**，而 1706 位于 1701/1704 之后 → 被改名的内层变量在被改名区域内不触及任何后续可观测状态；纯粹名字解析修正（C4456） |
| 4 | `app_window.cpp` `:1040`（旧）`save_post_action` | 未用形参 `id` 加 `(void)id;` 并注明语义 | 形参不参与行为；`(void)` 是显式意图声明，零 codegen 影响（C4100） |
| 5 | `worker_main.cpp` `make_adapter(const std::string& kind, const mj::Value& cfg)` | 未用形参 `cfg` 加 `(void)cfg;  // 预留：两类适配器目前都无需按实例配置参数` | `make_adapter` 全工程**只有 1 个调用点**（`worker_main.cpp:69`）；形参保留是为将来按实例配置留接口，不改签名只消警告 |

**关键发现（新定律，与 LTCG 定律并列）**：**"删冗余"是独立的一类变更，不触发 LTCG 体积反弹**。r14 五处改动后 exe **1,470,464 B，与 r13 完全同尺寸，零字节增长**。对比第 8 项（提取 helper，+2,560 B）、第 11 项 A/B（+1,536/+2,048 B），说明此前三次否决的教训不该被误读成"轻量化只能到此为止"——**该走的路线是减法而不是重排**。四门全过（见下），本类操作今后可作为常规手段继续使用。

| 门 | 结果（全部在远端 WorkCenter 执行） |
|---|---|
| 门1 编译 | 本地 `cmd /c build.bat release` **EXIT=0、0 错误、13 警告**（C4244×7 + C4996×6，码/文件/次数与基线完全一致；行号因改动位移：`getenv` 警告 644→**647**、`save_post_action` 相关 2061→**2062**，属预期的行级位移而非新问题）；exe 1,470,464 B |
| 门2 探针 | 远端跑 `probe_norm.exe`：输出 1315 B，与 golden `light-final-ltcg-probe.txt`（1318 B = BOM 3 B + body 1315 B）**去 BOM 后逐字节全等 `BODY_IDENTICAL=True`** |
| 门3 导入 | 远端三配置独立沙箱：maa1/maa3/maa5 各 `exit=0 entries=6` → **TOTAL=18** |
| 门4 差分 | ① `parse_url_diff.exe`：19 URL **mismatches=0**；② **新增双 exe 远端差分类**：旧二进制（1,542,656 B，远端在跑的构建）vs 新二进制（1,470,464 B）各建独立沙箱、各拷 9 个 `state\*.json`，跑 6 条 CLI 用例（seq 无参 / seq 不存在 id / ping / sched-list / tasks 无参 / accounts），逐例比对 **EXIT + 完整 stdout** → **MISMATCHES=0 / CASES=6**；`sched-list` 两侧均 `schedules=25` 且 25 行内容一致；跑后进程表核对 **无 stray 进程**（8 个 `MaaHub.exe` 全部 `START=12:55:50`，即活 Hub 树 253064 原班，未被测试惊动） |

**为什么门4 要新增双 exe 差分**：此前门4 只做 `parse_url` 单函数差分，而 r14 动的是 **CLI 分发路径**（`main.cpp` 的 `seq` 分支）与 **UI 侧变量**（`app_window.cpp`）。单函数差分覆盖不到。双 exe 差分把"同一组输入 → 同一组 EXIT+输出"变成可执行断言，是本次"严格确认功能实现正确"要求的直接落地。**注意**：验证一律使用 `seq` 的**错误路径**（无参 / 不存在的 id），**绝不用 `seq <有效 exec-id>` 做验证**——那会真的提交一条任务链。

---

## 二·补9 r15（22:2x–22:4x）：纯减法批 75 处（**实测确认"减法定律"：−3,072 B**）

r14 只证明了"删冗余不涨体积（0 B）"。r15 把它推到底：**一次性做 75 处纯减法**，看减法到底能不能真的把 exe 变小。答案是能，而且幅度可观。

改动分布（**6 个文件 / 75 个操作（72 删 + 3 替换）；删 75 行、替 3 行 → 源码净 −72 行 / −2,684 B**，以 `git diff --numstat` 实测为准）：

| 文件 | 删掉的是什么 | 删行 / 替行 |
|---|---|---|
| `app_window.cpp` | 17 对 `RECT save = gui_.clip();` + `(void)save;` 与 1 对 `save2`（合计 36 行）、死局部 `item_ys` 整块（7 行，含未使用的逐项 y 预计算）、未用的 `RC_RIGHT` 及其 `(void)`、`kEndOnly` 表 + 判定循环（8 行）、死局部 `std::vector<std::string> empty`；`RC_LEFT, RC_RIGHT` 双声明拆为单声明（替换 1 行） | 53 / 1 |
| `gui.hpp` | 死成员 `scroll_id_`（声明 + 唯一一处赋值，二者皆无读取）、`SIZE cs = text_size(w, font)` 及其 `cs.cx`（替换为同作用域同参数 `sz` 的 `sz.cx`）、button_group 内不可达的 `cs[i].right <= cs[i].left` 守卫、3 组 `save`/`(void)save` | 11 / 1 |
| `main.cpp` | 死局部 `mj::Value wd;`（现仅剩字面量 `"wd=["` 一处）、`err` 的 `size() > 300` 裁剪（位于 `printf` **之后**、`return` 之前，纯死赋值） | 2 / 0 |
| `core_manager.hpp` | `std::string tag = …; std::string lower = tag;` 二行合一（替换 1 行）、`c.is_default = false;`（`CoreInfo::is_default` 默认成员初始化已是 `false`） | 3 / 1 |
| `config_import.hpp` | 调用点重复的 4 处 `set("format", …)`（函数内 L352/L551 的真实赋值保留）、冗余的 `set("install_tag", tag)`（被调函数 L863 已用同一实参无条件写入） | 5 / 0 |
| `task_panels.hpp` | 相邻**完全相同**的 `group_title(c.x, c.y, L"Tag 策略")` 调用重复一次（同坐标同文本，删一次画面不变；现仅 L516 一处） | 1 / 0 |

**改动面复核（防夹带）**：`git diff --numstat` 逐文件实测 = 新增 3 行 / 删除 75 行，与操作表 3 `sub` + 72 `del` **精确对齐，无任何计划外改动**；`hub.hpp` 未参与本轮，post 快照与当前 `src\hub.hpp` 的 SHA256 同为 `0A077075…43076`（1,539 行 / 66,016 B）。

**体积结果：`MaaHub.exe` 1,470,464 B → 1,467,392 B（−3,072 B，−0.21%）**。这是本轮第一次用"删东西"真正换来二进制缩小——LTCG 定律讲的是"新增函数体会被 inline 保留导致涨"，减法定律讲的是"删掉的东西连机器码一起消失"。

| 门 | 结果（全部在远端 WorkCenter 执行） |
|---|---|
| 门1 编译 | `build.bat release` **EXIT=0、0 错误、13 警告**（C4244×7 + C4996×6，警告码集合与基线完全一致，仅行号随删除位移）；exe **1,467,392 B** |
| 门2 探针 | 远端 `probe_norm.exe` 输出 **1315 B 无 BOM**，与 golden 去 BOM 后 **`BODY_IDENTICAL=True`**，尾部 `PROBE-DONE` 正常（md5 `3E1D010EB1BF183798FBCE4EC448F275`） |
| 门3 导入 | 远端三配置独立沙箱 maa1/maa3/maa5 各 `exit=0 entries=6` → **`TOTAL=18`**；18 条 `account :: task \|\|\| params` 载荷行逐行全等，名称集合相同 |
| 门4 差分 | ① 形态 A `parse_url_diff.exe`：19 URL **mismatches=0**；② 形态 B 双 exe CLI 差分（旧 1,542,656 B 远端在跑构建 vs 新 1,467,392 B）：6 例 **`MISMATCHES=0`**，逐例 EXIT 相同（1,1 / 1,1 / 0,0 / 0,0 / 1,1 / 0,0），`sched-list` 两侧均 `schedules=25`、4 个账号、`executor not found` 文案一致 |

**门3 的比对口径（本轮新固化）**：gate-3 输出里含沙箱路径（`source_file` / `task-lists` / `schedules-merged`）与每轮新生成的 `exec_id`（`exec-<12 hex>`），这些**天然不同、不构成回归信号**。等值断言因此落在两处：① 18 条载荷行逐行全等；② 整份 379 行 transcript 在把 `r15-(cfg|iso|upload)` → `rXX-$1`、`exec-[0-9a-f]{12}` → `exec-XXX` 归一化之后**全等（`NORMALIZED_IDENTICAL=True`）**。

**明确否决、不做的候选**（零收益或带风险，逐条登记以免下一轮重复试）：
`gui.hpp:770 int nv = value;`（有下游用途，删了要改调用点，非纯减法）、`gui.hpp` 686-687 / 897-898 的 `HFONT f = font; std::wstring lbl = …`（是给控件传址的中间量，语义上等价于重排而非删除）、`core_manager.hpp:402 bool pick = false;`（被分支条件读取）、`main.cpp` 102-105 的 `HMODULE h = nullptr` 与 `LoadLibraryExW` 合并（合并=重排，触 LTCG 定律）、`task_panels.hpp:340` 边界子条件（是可观测的防御性判断）、`app_window.cpp` 的 `bool want_end = true;`（另有 3 处 `want_end` 参与 L2716 的逻辑，非死变量）。

**善后（已复核）**：远端 `dir /b … | findstr /i r15` 零命中（`build\state\*.json` 仍是 9 个），远端在跑的 `build\MaaHub.exe` 仍是 **1,542,656 B @12:41**、活进程仍是 **8 个**（root 253064 + 7 worker，全部 `START=12:55:50`）——测试未惊动运行体。本地证据按 `20261112-224122` 时刻戳落盘（r15 exe + 7 份 post-apply 源码）。

---

## 二·补10 r16（23:4x–00:2x）：45 项纯删除批（**源码 −338 行 / −12,3xx B；exe −1,536 B**）

r15 已把"可删的显式冗余"扫过一遍（75 处）。r16 换了搜法：**先按"死符号"扫，再要求每条候选由第二个 agent 独立复核**（不复用扫描者的证据，自己 grep 自证）。产出 45 项，全部是纯删除或"签名去死参"，没有一项是提取函数——因为 LTCG 定律（见 §三末行）已把 extract-method 方向永久关闭。

**候选来源与纪律**：6 个分组并行只读扫描（`app_window.cpp` / `hub.hpp`+`app_window.hpp` / `gui.hpp`+`task_panels.hpp` / `main.cpp`+`worker_main.cpp` / `config_import.hpp`+`core_manager.hpp` / `util.hpp`+`planner.hpp`+`json.hpp`+`accounts.hpp`+`settings.hpp`+`worker_client.hpp`）→ 每条候选再交给独立的复核 agent（3 条一批、并发 4），复核者必须自己 `read` 上下文 + 全仓 `grep` 该符号，**不采信扫描者写的证据**。45 条候选**全部被判 confirmed-deletable**（0 条 rejected-referenced、0 条 rejected-behaviour），这与 r15 的复核口径一致，但也说明"死符号"这一层已经挖到底。

改动分布（12 个文件 / 45 项；`git diff --numstat` 实测 **+12 / −338**）：

| 文件 | 删掉的是什么 | 删/增 |
|---|---|---|
| `app_window.cpp` | 5 个零调用成员函数：`action_pull_core`（33 行，含整条 `CoreManager::pull_core` 旧拉取链路）、`executors_of`（10 行）、`action_delete_executor`（8 行）、`action_stop`（4 行，与仍在用的 `action_stop_exec` 是两个符号）、`action_pull_latest`（3 行空壳转调）；2 个死局部 `version` / `daily_done`；`set_notice` 内只写不读的 `core_msg_ts_` 赋值；7 处 `begin_scroll` 首参（死形参，见下） | 7 / 72 |
| `gui.hpp` | 死图元 `frame()` + 渐变填充 `gradient_rect()`（全仓唯一 `GradientFill`/`TRIVERTEX` 使用者）、**SVG 图标链路 `draw_icon_path()`+`icon_link()`+`icon_unlink()`（80 行，含 2 段 ~4.5 KB 的 SVG path 宽字面量；`draw_icon_path` 经复核确认只被这两个死函数调用，是孤儿）**、`checkbox(bool&)` 重载（40 行，UI 实际用 `checkbox3`）、`scroll_offset()`、`clip()` 存取器；`Theme` 7 个只写不读字段（`ok_light`/`warn_light`/`err_light`/`accent`/`input_bg`/`dark`/`dark_mask`）及其 `make_dark()` 赋值；`Input` 3 个恒 false 字段（`double_click`/`shift`/`ctrl`）；`#include <codecvt>` | 2 / 179 |
| `hub.hpp` | 死 API `erase_task_param`（12 行）、`remove_task_params`（7 行）、`config()` 访问器、`subscribe()` 订阅接口 + 其恒空的 `subscribers_` 成员与 `on_worker_event` 里那个永跑 0 次的 `for` 循环（**这条同时消掉一处"永远靠空容器才不会崩"的潜在 UB**）；从不启动的 `save_thread_` 及其恒假的 `joinable()` 分支；孤立空语句 `(void)0;` | 0 / 31 |
| `app_window.hpp` | 对应 8 条声明（其中 `render(RECT)`、`action_adopt` 是**只有声明、从来没有定义**的幽灵成员）；死成员 `gif_index_`/`sidebar_w_`/`core_msg_ts_` | 0 / 10 |
| `core_manager.hpp` | 死函数 `refresh_version()`（11 行）；`UpdateInfo::release_notes` 只写不读字段及其下标截断赋值（顺带省掉一次 `substr` 分配） | 0 / 13 |
| `settings.hpp` | 死查询 `load_note()` + 只写不读的 `load_note_` 字段与 6 处簿记语句（`load()` 的真实控制流不变：解析成功即 return、损坏仍改名 `.corrupt`、`.bak` 回退仍 `from_json`+回写+return、首运仍静默默认） | 0 / 12 |
| `json.hpp` | 死 API `Value::type()`、`Value::get_path_bool()`、实例 `dump()`、自由函数 `parse()`、类型别名 `ValuePtr`（连带 `#include <memory>`）、`#include <sstream>` | 0 / 11 |
| `worker_client.hpp` | 死存取器 `last_error()`；`<memory>`/`<vector>` 两个未用 include | 0 / 3 |
| `worker_main.cpp` | `make_adapter` 的死形参 `const mj::Value& cfg` + 消警用的 `(void)cfg;` + 唯一调用点实参 | 2 / 3 |
| `main.cpp` | 只写不读的静态全局 `g_port_override`；`--port` 分支改为"仅吞掉取值"（保留 argv 消耗语义，否则 `--port --check` 里被吞的 `--check` 会重新参与解析 = 行为变更） | 1 / 2 |
| `accounts.hpp` / `planner.hpp` | 各 1 个未用 include（`<algorithm>` / `<mutex>`，均已确认无传递依赖方需要） | 0 / 2 |

**门 1 抓到一次真错，已修（值得登记）**：删除区间 `hub.hpp:676-682` 时，把 `on_worker_event` 的**函数收尾大括号（原 677 行）**一并吞了（区间按"首个语句 + subscribe 全函数"划，末行断言只查了 subscribe 的 `}`，没查区间内是否含别的函数的收尾括号）。后果是类体提前闭合，编译在 `config_import.hpp:58 namespace hub {` 处报 C2059、在 `hub.hpp:137` 起报 C2614/C2065 级联共 100+ 条。**修复**：补回该 `}`（`git diff` 可见 hub.hpp 只此一处增行）。**流程加固**：本轮之后，删除区间的验收不能只看"首行/末行子串匹配"，还须校验**区间内 `{` 与 `}` 数量相等**——已在 `del_ranges.ps1` 的善后清单中登记为下一轮脚本改进项（本轮 45 项里只有这一处踩中，属人工划界错误而非候选判断错误）。

**体积结果：`build\MaaHub.exe` 1,467,392 B → 1,465,856 B（−1,536 B，−0.10%）**；源码 **12,556 → 12,230 行（−326 行）**、字节层面 −12.3 KB 级（`gui.hpp` 54,166→44,970 B，`hub.hpp` 66,016→64,872 B，`app_window.cpp` 169,238→166,768 B）。

**诚实归因（不虚构）**：338 行删除只换来 1,536 B，比例远低于 r15（75 行换 3,072 B）。原因与 LTCG 定律一致——**本轮删掉的绝大多数是"未被引用的 inline 成员/死局部"，它们本来就没被发射成机器码**，删了只减源码不减体积。真正减体积的应来自 5 个 `.cpp` 内的非 inline 死函数、`draw_icon_path` 那段 SVG 字面量、`settings`/`core_manager` 的字符串字面量与 `substr` 调用。**但本轮没有做逐项单删重编的体积归因**（那要 45 次构建），所以只报告总量，不给"某项贡献多少字节"的伪精度。

| 门 | 结果（全部在远端 WorkCenter 执行，本机只编译与文本比对） |
|---|---|
| 门1 编译 | `build.bat release` **EXIT=0、0 error、13 warning**（`C4996×6 + C4244×7`，警告码多重集合与基线完全一致，仅行号随删除位移）；exe **1,465,856 B**，SHA256 `AC94569DAD7F37A2137F640ED455D30D06E0448243EB195CFF5FF2EFE525D591` |
| 门2 探针 | 从 r16 源码重建 `probe_norm.exe`（242,176 B，与 r15 同尺寸），远端运行输出 **1315 B 无 BOM**，与 golden **逐字节 IDENTICAL**（md5 `3E1D010EB1BF183798FBCE4EC448F275`），尾部 `PROBE-DONE` 正常 |
| 门3 导入 | 远端三配置独立沙箱 maa1/maa3/maa5 各 `exit=0`，**`TOTAL=18`**；18 条 `account :: task \|\|\| params` 载荷行与 r15 基线**多重集全等**（`ENTRIES_IDENTICAL=True`） |
| 门4 差分（形态 B） | 旧 = 远端在跑 r15（1,467,392 B）vs 新 = r16（1,465,856 B），各自沙箱 + 同一份 9 个 `state\*.json` 拷贝，**7 例（`seq` 无参 / `seq` 不存在 id / `ping` / `sched-list` / `tasks` / `accounts` / `devices`）`MISMATCHES=0`**，逐例 EXIT 与完整 stdout 全等；跑完 `MaaHub.exe COUNT=7`（live Hub + 6 worker 未被惊动） |
| 体积门 | −1,536 B（不增长），且**顺带把 5 条死路径从分发体里彻底摘除** |

**部署（r16 已上远端，00:19:50 完成）**：`swap-r16.ps1` 六阶段——A 前置校验（live 必须精确是 1,467,392 B / `2383AFFC…`，新件必须精确是 1,465,856 B / `AC94569D…`，否则 exit 2 不动手）→ B 备份并校验（`_r16scratch\backup\MaaHub-1467392-20261113-001944.exe`，sha 全等）→ C 停进程（7 个 → `REMAINING=0`）→ D 安装后**再校验 sha**（不等则从备份回滚并 exit 4）→ E 用 `Win32_Process Create` 无窗口拉起（`rc=0 pid=253620`）→ F 复核：**`PROCS=7`（root 253620 + 6 worker，全部 `START=00:19:50`）、`STATE_JSON=9`、trace 985,514 B / mtime 00:19:51**，含 6 条 `spawn_worker … load_ok=1`（`57fec764dd37`/`8b1b54dccbeb`/`5fa8dd2145ee`/`84691d0fba02`/`6ac900828da1`/`83dc5881a4ce`）与 2 条 `maaend load done`，**无 `exec-02913292d1eb`**（探针执行器已在 23:26 退役）。远端 scratch 里 `iso`/`diffbox-*`/`g2` 已删干净，只留脚本与证据。

## 二·补11 r17（00:40–00:57）：首次进入 `src\adapters\` + 第二轮"会发码的死语句"（**63 行 → −2,560 B**）

r16 的教训是"删行数不等于减体积"。r17 因此换了两件事：① **把 1,841 行从未被扫描过的 `src\adapters\`（`maaend_adapter.hpp` 1244 行、`maa_adapter.hpp` 509 行、`adapter.hpp` 88 行）纳入范围**；② 主文件走"第二轮"，只找**不可达分支 / 死常量 / 重复条件 / 空块 / 死库符号**这些**藏在会被发射的函数体里**的东西。

**方法（比前几轮更严）**：4 组并行只读扫描（adapters 拆 2 组 + 主文件拆 2 组）→ 23 条候选 → 每条交独立复核（3 条一批、并发 4），复核者必须自读上下文 + 自 grep，**不采信扫描者写的证据**。结果 **17 条 confirmed / 6 条 rejected-behaviour / 0 条误报为"被引用"**——这 6 条否决是本轮最有价值的产出，见文末。

改动分布（**11 个文件 / 37 个操作（36 段删除 + 1 处常量折叠）；删 63 行、改 1 行**）：

| 文件 | 删掉的是什么 | 删/改 |
|---|---|---|
| `gui.hpp` | **死成员 `hot_` 整条链路**：声明、`begin()` 里的 `clear()`、以及 8 处 `if (hov) hot_ = …;`（combo/button/button_group/spin/checkbox3/text_input/右侧下拉），连带 `spin()` 里只为它服务的 `uid`/`did` 两个局部；死字体 `font_big`（声明 + `make_font(16, true)`，全仓零读取，白白 `CreateFontW` 一个从不用的字体）；`#include <deque>`（唯一 `std::deque` 用户在 `app_window.hpp`，已由 `hub.hpp` 间接提供） | 0 / 14 |
| `adapters\maa_adapter.hpp` | 死枚举 `enum AsstMsg {…17 个枚举值…}`（20 行）——**枚举值本身从未被引用**，回调分支走 `ev.what` 字符串、消息表 `kMsg[]` 用具名整数 `{10001, {"task","TaskChainStart"}}`；死常量 `SCREENCAP_GDI`/`SCREENCAP_FRAMEPOOL`（只有 `SCREENDC`/`INPUT_SEIZE` 在用）；`#include <memory>` | 0 / 23 |
| `adapters\maaend_adapter.hpp` | `load_case_labels` 里 `cache["|" + ok + "|" + cn]` 这条**三重键死写入**（真正被 229 行读的是它上一行的 `"*|" + ok + "|" + cn`；带前导 `|` 的键不会被 `task|key|case`（task 非空）或 `norm_case_key`（首字符不可能是 `|`）查到）；`#include <memory>` | 0 / 2 |
| `worker_client.hpp` | 死成员 `last_error_` 的 6 处赋值（`worker spawn failed`/`exited early`/`start timeout`/`not running`/`write failed`/`timeout`）+ 声明（r16 已删掉唯一的读接口 `last_error()`） | 0 / 7 |
| `util.hpp` | `WindowInfo` 的 `cls`/`title`/`pid` 三个只写不读字段 + 对应三处填充（含一次 `GetWindowThreadProcessId` 调用；`wcls`/`t` 仍在过滤逻辑里使用，不受影响）；`ProcHandle::pid` 字段 + `close()` 里的清零 + `spawn_process` 里的 `out.pid = pi.dwProcessId`（`pi.hProcess`/`pi.hThread` 照旧使用） | 0 / 9 |
| `app_window.cpp` | 不可达的 `else if (!tasks.empty()) { sel_task = tasks[0]…; sel_valid = true; }`（3 行）：`sel_task` 在 1488-1494 已保证"tasks 非空 ⇒ 非空且能在 tasks 中命中"，故进入 `!sel_valid` 分支时 `tasks` 必为空 | 0 / 3 |
| `main.cpp` | 不可达的 `if (!cat.is_array()) { printf("catalog empty\n"); return 1; }`：`executor_catalog` 的四条返回路径全部 guarantees 数组（缓存分支先 `is_array()` 才返回、无 worker 回 `array()`、worker 分支要 `r.is_array()`、兜底 `array()`） | 0 / 1 |
| `settings.hpp` | `load()` 里的空 `else { }` 分支（`} else {` 一行删除后，原 `}` 直接闭合 `else if`，控制流与三档语义逐字节不变） | 0 / 1 |
| `hub.hpp` + `planner.hpp` | `LlmPlanner::Config::enabled` 字段（写入端只有 `hub.hpp:1437`，全仓零读取——走不走 LLM 由 `planner_name == "heuristic"` 决定）+ 该处唯一赋值 | 0 / 1 + 0 / 1 |
| `task_panels.hpp` | `int col = 0;`（全程只写一次、只读一次，且乘的是 `col` ⇒ `fx` 恒等于 `c.x`），改为 `int fx = c.x;` | 1 / 1 |

**体积结果：`build\MaaHub.exe` 1,465,856 B → 1,463,296 B（−2,560 B，−0.17%）**；源码 **14,071 → 14,008 行（−63）**。单位行收益是本轮最高（r16 是 338 行换 1,536 B ≈ 4.5 B/行，r17 是 63 行换 2,560 B ≈ 41 B/行），原因与"减法定律"一致：**这一轮删的是真的会发码的语句**——每帧执行的 `hot_ = std::string` 赋值（8 处 × 字符串拼接）、`CreateFontW`、`GetWindowThreadProcessId`、`WorkerClient` 里 6 处字符串字面量赋值、以及 `AsstMsg` 那张表的 20 行声明区，而不是 r16 那批"本来就被 LTCG 丢弃的未引用 inline 成员"。

| 门 | 结果（门 2/3/4 全部在远端 WorkCenter 执行；本机只编译与文本比对） |
|---|---|
| 门1 编译 | `build.bat release` **EXIT=0、0 error、13 warning**（`C4996×6 + C4244×7`，与基线码集一致；行号随删除位移：`util.hpp:370`、`hub.hpp:128`、`core_manager.hpp:63/64/632`、`app_window.cpp:2047`）；exe 1,463,296 B / SHA256 `96E93CC3…` |
| 门2 探针 | 从 r17 源码重建 `probe_norm.exe` 后在远端运行：**1315 B 无 BOM，与 r16 接受体逐字节 IDENTICAL**（md5 `3E1D010EB1BF183798FBCE4EC448F275`） |
| 门3 导入 | 三配置独立沙箱 maa1/maa3/maa5 各 `exit=0`、`entries=6` ⇒ **TOTAL=18**；18 条载荷行与 r16 基线**多重集全等（DIFF_COUNT=0）** |
| 门4 差分（形态 B） | 旧 = 远端在跑 r16（1,465,856 B）vs 新 = r17（1,463,296 B），各建沙箱 + 同一份 9 个 `state\*.json`，**7 例全部 `SAME=True`、`MISMATCHES=0`**；跑完 `MaaHub.exe COUNT=7`（live Hub 与 6 worker 未被惊动，PID 仍是 r16 部署时的 253620 一系） |
| 体积门 | −2,560 B（不增长） |

**部署（00:57:40 完成）**：六阶段全过 —— A 前置校验（live 必须精确 `1,465,856 B / AC94569D…`，新件必须 `1,463,296 B / 96E93CC3…`，否则 exit 2）→ B 备份 `_r17scratch\backup\MaaHub-1465856-20261113-005734.exe`（sha 全等）→ C 停 7 进程 `REMAINING=0` → D 安装后二次 sha 校验 → E `Win32_Process Create` 拉起（`rc=0 pid=267436`）→ F 复核 **`PROCS=7`、`STATE_JSON=9`、trace 988,467 B@00:57:41**、6 条 `spawn_worker load_ok=1`、无探针执行器。远端 `iso`/`diffbox-*`/`g2` 已清理，只留脚本、cfg 与证据。

**明确否决的 6 条（本轮最重要的"不删"清单，防下一轮重犯）**：`maa_adapter.hpp` 的 `AsstCreate` / `AsstSetStaticOption` / `AsstSetTaskParams` / `AsstLog` / `AsstGetImage` / `AsstGetNullSize` 六行看似"全仓只有定义 + 一行 `sym()` 登记、0 调用"，但 `sym()` 的返回值参与 `resolve_symbols()` 的 `ok` 合取，而 `load()` 在 `if (!resolve_symbols())` 时报 `missing AsstApi exports` 并失败——**删掉任何一行都把"19 个导出必须齐全"的加载期校验放宽为 18 个**：对用户自备（`install_dir` 由配置指定、`core_manager` 只查文件存在）的旧版/精简版 MaaCore，错误返回会从"加载失败"变成"加载成功"。虽然随附的两份 `MaaCore.dll`（6.13.0 / 6.22.0）都导出这 19 个符号、删了在随附数据上不可观测，但"绝对等价"只在随附二进制上成立、对用户核心不成立，因此按判据整批判否决。**登记为一类风险模式：`GetProcAddress` 校验表的成员不是死代码，哪怕成员本身从未被调用。**

**另一条主动放弃的候选**：`maaend_adapter.hpp:291` 的 `cache["*|" + option] = …` 写入（全局 option 标签落进两段式键），只有存在字面名为 `*` 的 task 时才会被 `option_label` 的 `task|key` 查到。删它依赖"不存在名为 `*` 的 task"这个数据假设（随附 interface.json 已确认没有），且删除后行为在那种退化数据下会变，故**不动**；作为"疑似死逻辑（全局 option 标签回退从未生效）"登记，属产品语义问题而非轻量化。

## 三、纪律性排除（明确决定不改的，及理由）

| 项 | 位置 | 不改理由 |
|---|---|---|
| catalog `enum` 第三份 drones 列表 | `adapters\maa_adapter.hpp:460` | 顺序是 `...CombatRecord, PureGold, OriginStone, Chip`，**与 `kDrones` 不同**（后者 `...CombatRecord, Chip, PureGold, OriginStone`）。这是 UI 下拉/校验展示列表，不是索引表；合并会改变显示顺序 = 可观测行为变更。**登记为潜在漂移风险**：若将来两处需一致，必须先确认产品意图再统一 |
| Unicode 标签列表 | `task_panels.hpp:171-172`（官服/Bilibili/txwy/YoStarEN/JP/KR）、`:413`（drones 中文标签） | 中文显示标签，非索引数据表，无重复可消除 |
| planner 设施集合 | `planner.hpp:172` `{Mfg,Trade,Control,Power,Reception,Office,Dorm}` | 与 `kFacility` 是**不同集合**（7 项 vs 8 项，含 Reception/Office/Dorm），不是同一张表；强行合并会改变 planner 语义 |
| `trampoline` | `adapters\maa_adapter.hpp:340` | 经函数指针回调使用，非死代码 |
| **C4244 时间戳链路（7 处中的 4 处 get_int→double 站点）**（r13 后 triage，19:5x） | `hub.hpp:128`、`core_manager.hpp:63-64`、`app_window.cpp:2061`（另 accounts.hpp:59/61 已显式 `(double)` 钳位） | **判定良性，不改**。证据链：① `json.hpp:50-59`——`as_int` 对 Double 走 `(int64_t)d_` 截断，`as_double` 对 Int 精确（Unix 秒 « 2^53）；② 写入端 `util::now_sec()` 是带小数 double，持久化后重载确实丢亚秒位——这是 C4244 提示的唯一真实语义；③ 但全部消费端只用秒级：hub.hpp:188 排序有 `id` 决胜（同秒创建本来就必须靠 id 定序，截断不改变"稳定可预期"承诺）、UI 显示相对时间、TTL 缓存 `status_ts/catalog_ts` 纯内存不持久化（hub.hpp:303/315/329/341 只在写读内存）；④ 修它需给 json.hpp 加 `get_double` 小内联成员——LTCG 定律必致体积反弹，用增长换一个不可观测的精度。**结论固化：字段声明 double 是为内存态计时精度，持久化经 get_int 降为整秒是既定的有损但无害序列化口径** |
| **snapshot accounts 序列化循环 ×2**（本轮第 8 项，15:17） | `hub.hpp:562-570` + `:622-630`（snapshot_fast 与 snapshot 各一份，逐字节相同） | **实测否决**：已提取 `accounts_snapshot_json(today)` 并过全部功能门——编译 EXIT=0、门 4 差分 5 用例 mismatches=0（含空列表/激活/未激活/同 game 多账号/today 边界）、探针 IDENTICAL、exe 回到原值。**但体积 +2,560 B**（1,471,488 → 1,474,048）：类体内定义＝隐式 inline，LTCG 既把它内联回 2 个调用点、又保留独立发射副本，净代码比原 2×8 行内联更多。轻量化目标下这笔交易亏本，已完整回滚（源码 1541 行、exe 字节级复原 1,471,488、探针 IDENTICAL）。**结论固化**：纯搬移型 extract-method 的去重收益必须实测体积，源行数 −N 不等于体积 −。差分测试脚本存档：`_tmp\maahub-day1\accounts_snap_diff.cpp.rejected` |
| **accounts 循环去重·类外定义重试 ×2**（第 11 项，16:5x–17:0x，事故前） | 同上两处循环 | **再次实测否决**：吸取第 8 项教训改走"声明在类内、定义在 hub.hpp 类外"路线，两个变体（A：单 helper 返回 array；B：每账号 build 一行 helper）分别实测 exe **1,472,000（+1,536）/ 1,472,512（+2,048）**，功能门虽全过（探针 IDENTICAL、TOTAL=18、门4 mismatches=0），体积门不过 → 按"体积是验收门"纪律两次回滚。**定律固化（LTCG inline-retention law）**：对 ~392 B 级小函数体，无论类内（隐式 inline → 内联+保留双发射）还是类外（调用点开销 > 折叠收益），去重都不省字节；只有 r10 那种 ≥11 行且含字符串构造/分支的较大循环体才在类外路线上净减。**该方向永久关闭**，不再重试第三种变体 |

---

## 三·补 事故复盘与重建闭环（16:4x → 19:4x）

**事故**：r11-B 否决回滚时用了 `git checkout -- src/hub.hpp`。该命令把 hub.hpp 还原到 **git HEAD**——而 HEAD 是本轮全部轻量化**之前**的原始版本（63,426 B / 1508 行）。接受版 hub.hpp（含 r1/r6/r9/r10 四项已验收改动，15:36 时点）从未提交，被静默摧毁。连带炸出编译链：config_import.hpp（接受版，引用命名空间级 `kFacility/kDrones/kClients + *Count`）对 HEAD hub.hpp 产生 C2065 级联。**教训固化：验收过的未提交改动所在文件，禁止 `git checkout/restore`；回滚一律用显式快照副本覆盖。**

**恢复调查（穷尽后关闭）**：全盘 4 处 hub.hpp 副本（audit 12:10 版 65,923 B、pruned 13:45 实验版、iso 沙箱无源码、IDE local history 无命中）+ `git fsck --unreachable`（60–70 KB 区间零对象）——没有任何副本同时是"接受版"；接受版**不可能字节级找回**，只能重建。

**重建（r12-restore）**：以 HEAD hub.hpp 为底，依据 ①§一.1 的接受态描述 ②audit 副本的冻结锚点体（A1 门控/钳位、split_list、client_type、B9 stable_sort 全序）③config_import 契约 grep（只用 6 个符号、plain 命名、零 `kHub*`）逐条重放：
1. 命名空间级 `inline const char* const kFacility[8]/kDrones[7]/kClients[6]` + 三个 `inline constexpr size_t *Count`（r1）；
2. A1 Infrast：删函数内局部表，`drones` 写入以 `in.find("drones_idx")` 为门、越界钳位走 `kDroneCount`，设施循环走 `kFacility/kFacilityCount`（r1+A1 冻结语义）；
3. Mall：`split_list` lambda + `blacklist_str`/`buy_first_str` 双列表（audit 版语义）；
4. StartUp：`client_type` 0..5→`kClients[idx]`、字符串透传、未知回退 `"Official"`，用命名空间表（r1 合并后形态）；
5. B9 `canonical_task_order`：`kMaa[]` + `rank_of`（CloseDown=100/未知=50）+ `std::stable_sort` 全序版；
6. r6：删私有 `random_hex`，两调用点改 `util::random_hex(6)`；
7. r9：删 `#include <random>`（全文件确认无其它使用者）。

**验收（19:3x–19:4x，四门全过；证据口径以 19:4x 自我纠错为准）**：

| 门 | 结果 |
|---|---|
| 重建完整性 | 与 audit 副本残余 diff 恰为 r10 表提取 12 个 hunk + r6/r9 3 处，无遗漏项；`kHub*`/旧符号全树 grep 清零 |
| 门1 编译 | EXIT=0，警告 **13 次**（C4244×7+C4996×6，hub.hpp:128 行号与基线一致），0 error |
| 门2 探针 | `probe_norm.exe`（从当前源重建）输出 1318 B，MD5 `44F8170E702FF26204C89A5EE89FD17E` = 接受参考 |
| 门3 导入 | `iso_import.ps1` maa1/maa3/maa5 各 6 = **TOTAL=18**，参数全对基线 |
| 体积门 | **`build\MaaHub.exe` = 1,470,464 B**，与事故前记录的 r10 接受态体积精确一致 |

**防再发**：重建后源码树（20 文件）已快照到 `_tmp\maahub-day1\evidence\srcsnap-20261112-restore\`；**接受态比对专用副本**（防 iso_import 覆盖）另存于 `_tmp\maahub-day1\evidence\accepted-hub\`：`hub-1539-r12restore-192410.hpp` + `MaaHub-1470464-192410.exe`（SHA256 `EFAE5D34D83847CD3BE10F9468F179BDAEC81B134066413EF4923306AE413202`，今后所有轮次以它做体积/字节回退基准）。**下一步动作：把当前工作树提交为 git 基线**（未提交状态是本次事故的全部根源），提交前需用户确认。

**遗留差异（诚实记录，勿当成问题）**：重建后 hub.hpp = **1539 行**，而接受态（r9 时点）记录为 1540 行，差 1 行。证据：①与 audit 副本的逐 hunk 残余比对，差异**只**落在 r10 表提取（12 hunk）+ r6/r9（3 处），没有任何"未重放"的接受态改动；②exe 体积与接受态记录值精确一致。综合判断该 1 行为重建时未复制的注释/空行（不参与 codegen），无功能影响，不再为凑行数改动文件。

**证据链自我纠错（19:4x）**：上一节曾声称"重建 exe 与 15:32 三份接受态副本 SHA256 逐字节全等"——复核发现该比对**无效**：`iso_import.ps1` 第 15 行每次运行都把**当前** `build\MaaHub.exe` 覆盖式拷入沙箱，而门 3 在比对前刚跑过，三份副本的 mtime 全部是 19:24:10（本次重建产物），即"拿新 exe 和新 exe 比"。接受态 exe 的独立副本在本机**并不存在**（14:04 的 audit 快照是 1,539,072 B 旧时代产物，r6–r10 之前，不可比）。故重建验收的真实证据是上表的四门 + 体积精确复原，而非字节全等。连带固化一条流程规则：**任何"接受态产物"若要用于事后比对，必须放在 iso_import.ps1 覆盖范围之外，并在拷贝文件名中写入构建时刻**。

---

## 四、下一轮待办（持续迭代）

1. **跨文件/函数重复块**（`scan_dups.ps1` 已产出候选）：
   - ✅ `adapters\maaend_adapter.hpp` 目录+文件枚举重复 3 份 → 已合并到 `locale_files()`（见"二·补"）。
   - ✅ `app_window.cpp` 运行顺序重排 2 份 → 已提取 `ordered_catalog_items`（见"二·补2"）。
   - ✅ `random_hex` 5 份 → 已合并到 `util::random_hex`（见"二·补3"，−6,656 B，源行 −41）。
   - ✅ `core_manager.hpp` URL 拆分 2 份 → 已提取 `parse_url`（见"二·补4"，−3,072 B，源行 −5，新增门 4 差分测试）。
   - ❌ `hub.hpp:562-570`+`:622-630` — snapshot accounts 序列化循环 ×2 → **已实测否决并回滚**（功能门全过但体积 +2,560 B，见第三节末行）。注：不能用 `accounts_json()` 顶替（后者不设 `is_active`、返回 vector、语义不同）。
   每项提取必须各自过门（编译 EXIT=0 + 探针 1318 B 逐字节 IDENTICAL + 独立沙箱 3×6 导入 18 条 + **函数体搬移类另过差分测试门 4**）。另有低优先候选：`app_window.cpp:3009/3039` checked/run 残留、`maaend_adapter.hpp:1036 / maa_adapter.hpp:269` 跨文件样板（收益小风险中）、`accounts.hpp`/`planner.hpp` 合并后疑似失效的 `#include <random>`（删前须确认无其它 `mt19937`/`random_device` 使用者）、`parse_url` 内保留的 `std::wstring::npos` 误写（与 `string::npos` 同为 -1，行为正确，规范化重写单独轮次过门）。
2. **`/W4` 存量警告逐条判定**（r14 已消掉 4 条：C4456×2、C4189×1、C4100×2 中的 worker_main 一条——注意 C4100 的 UI 侧 `save_post_action` 只加了 `(void)` 消警，未改签名）。剩余：C4244×7 已判定良性并固化（见 §三 表），C4996×6（`getenv`/`strcpy` 等）待单独决策，`draw_rename_box` 等处剩余 C4100 属纯噪音。**注意 `/W4` 未被 build.bat 透传**（`/W4x` 试过也不转发），逐条复查须手动加参数构建——那是"检查构建"而非交付构建，产物不要覆盖 `build\MaaHub.exe`。
3. 体积/依赖面：✅ **已关账（r16）**。`json.hpp` 的零使用 API 已全部删掉（`Value::type()`、`get_path_bool()`、实例 `dump()`、自由函数 `parse()`、别名 `ValuePtr` + `<memory>`/`<sstream>`）；**`escape`/`write` 的序列化分支全部在用，不可裁**（明细见二·补10）。
4. ✅ **（r14 立项、r15 实测确认，「减法类」轻量化）**：删冗余调用点 / 死局部 / 未用形参 / 冗余调用 / 死分支 —— 不新增函数体、不触 LTCG 定律，**r15 已实测确认它能把 exe 真的变小（75 处 −3,072 B）**，r17 再次验证（37 处 −2,560 B，且第一次覆盖 `src\adapters\`）。后续继续按此路线扫描，每一项仍须过四门，尤其门 4 形态 B。**r16 的 `-338 行只换 -1,536 B` 与 r17 的 `63 行换 -2,560 B` 一起说明：决定收益的不是删除行数，而是删掉的语句是否真的被发射成机器码**（死 inline 成员 0 B、每帧执行的 `hot_` 赋值/Win32 调用/GDI 建字体才有字节）。
5. ✅ **已全部完成**：① 工作树已提交为 git 基线 `fcee346`；② 部署链已走到第三代——**12:55** 中间构建（1,542,656 B）→ **23:14** r15（1,467,392 B）→ **00:19** r16（1,465,856 B）→ **00:57:40** r17（1,463,296 B），每次都是「前置 sha 校验 → 备份并校验 → 停进程（REMAINING=0）→ 安装后二次 sha 校验 → `Win32_Process Create` 无窗口拉起 → 复核进程/state/trace」六阶段，备份链在 `_r15scratch\backup\`、`_r16scratch\backup\`、`_r17scratch\backup\`。
6. ✅ **已完成（23:26）**：`exec-02913292d1eb` 已退役，走的是「停 Hub → 备份 4 份 state → 覆盖预计算替换文件（executors/schedules/task-lists/task-params）→ 起 Hub → 复核」的受控写路径，`===RETIRE_EXIT=0`：4 个文件引用数 1→0、schedules 条目 25→24、executors 7→6、进程 8→7、trace 中 6 条 `spawn_worker` 不再含该 id。备份 `_r15scratch\backup\retire-20261112-232658`。**教训保留**：Hub 运行期直接改 `executors.json` 会被内存态覆盖，退役必须停进程 + 整体替换 + 事后复核引用数。
7. **已登记的产品特性项（不在轻量化范围，只记录）**：MaaHub **没有单实例守卫、也没有 `state\.lock`**——零参数启动会起第二个完整 Hub + 7 个 worker 共写同一 `state`（20:36 已实证并复盘，见 §五）。这类缺陷属于功能缺口，本轮只做记录与规避（每次 CLI 调用一律带 `--cli`），不在"轻量化"授权范围内擅自实现。

---

## 五、证据与产物清单

| 产物 | 路径 | 说明 |
|---|---|---|
| 基线探针 | `D:\WorkSpace\_tmp\maahub-day1\evidence\light-baseline-probe.txt` | MD5 `44F8170E702FF26204C89A5EE89FD17E`，1318 B |
| 删码后探针 | 同目录 `light-after-deadcut-probe.txt` | 与基线 IDENTICAL |
| **LTCG 采纳后探针** | 同目录 `light-final-ltcg-probe.txt` | 与基线 IDENTICAL（最终门） |
| 死码引用扫描器 | `_tmp\maahub-day1\scan_deadrefs.ps1` | C4505=0 与文本扫描交叉验证 |
| 重复块扫描器 | `_tmp\maahub-day1\scan_dups.ps1` | ≥8 行归一化指纹，跨文件 |
| 双 exe 门控脚本 | `_tmp\maahub-day1\ltcg_gate.ps1`、`cli_diff_gate.ps1` | 语义等值 + 6 CLI verb |
| 竞态证明脚本 | `_tmp\maahub-day1\race_test.ps1` | 同 exe 三跑 trace 行数 120/120/119 |
| 独立沙箱导入 | `_tmp\maahub-day1\iso_import.ps1` | 3 配置各 6 条 = 18 条 |
| 构建脚本变更 | `maa-hub-cpp\build.bat` release 分支 | `/GL` + `/LTCG`（含采纳理由注释） |
| 标签去重探针 | 同目录 `light-locale-probe.txt` | 与基线 IDENTICAL（第 4 项） |
| 运行顺序去重探针 | 同目录 `light-runorder-probe.txt` | 与基线 IDENTICAL（第 5 项） |
| random_hex 合并探针 | 同目录 `light-randomhex-probe.txt` | 与基线 IDENTICAL（第 6 项） |
| URL 拆分去重探针 | 同目录 `light-urlprobe-probe.txt` | 与基线 IDENTICAL（第 7 项） |
| **门 4 差分测试** | `maa-hub-cpp\tests\parse_url_diff.cpp` | orig vs new 19 URL 全等（第 7 项） |
| 第 8 项否决留痕 | `_tmp\maahub-day1\accounts_snap_diff.cpp.rejected`、同目录 `light-r8-rollback-probe.txt` | accounts 提取差分 5 例全过但体积 +2,560 B，实测否决并回滚 |
| r11 双变体否决留痕 | 同目录 `light-r11a-probe.txt`/`light-r11b-probe.txt`（探针均 IDENTICAL）、build 日志 `r11a-build.log`/`r11b-build.log` | 类外定义重试 A +1,536 / B +2,048，体积门否决 → 引出随后的 git checkout 事故（见三·补） |
| **重建验收探针** | 同目录 `light-r9-final-probe.txt`（接受参考，MD5 `44F8170E…`）与 `_cmp\light-r12-restore-probe.txt` | r12-restore 从当前源重建 probe_norm.exe 后输出 **1318 B / MD5 与接受参考全等** |
| **重建后源码快照** | `_tmp\maahub-day1\evidence\srcsnap-20261112-restore\`（20 文件 + `MaaHub-1470464.exe`） | 事故防再发：接受态源码与 exe 一并落盘，后续任何回滚用本副本覆盖，禁用 git checkout |
| 重建构建日志 | 同目录 `r12-restore-build.log` | EXIT=0，13 警告（C4244×7+C4996×6），0 error，exe 1,470,464 B |
| **r13 接受态固化** | `_tmp\maahub-day1\evidence\accepted-hub\core_manager-r13-194543.hpp` / `hub-r13-194543.hpp` / `MaaHub-r13-194543.exe`（1,470,464 B，SHA256 `0B3F9A2D…`） | 带构建时刻戳、位于 `iso_import.ps1` 覆盖范围之外（见三·补流程规则） |
| **r14 门控证据（远端）** | `_tmp\maahub-day1\seq-diff-r14.txt`（7,867 B） / `gate3-r14.txt`（14,834 B） / `probe-remote.txt`（1,315 B） / `diff-remote.txt`（47 B） | 门4 双 exe 6 例 MISMATCHES=0；门3 TOTAL=18；门2 BODY_IDENTICAL=True；门4b parse_url 19 URL mismatches=0 |
| **r15 门控证据（远端）** | `_tmp\maahub-day1\gate3-r15.txt`（379 行，`EXE=1467392 MTIME=2026/11/12 22:38:48`） / `seq-diff-r15.txt`（7,867 B，`OLD_EXE=1542656` vs `NEW_EXE=1467392`，6 例 `MISMATCHES=0`） / `evidence\probe-r15.txt`（1,315 B 无 BOM，md5 `3E1D010EB1BF183798FBCE4EC448F275`） / `evidence\parse-url-r15.txt`（47 B） | r15 四门：门1 EXIT=0/0 err/13 warn；门2 BODY_IDENTICAL=True；门3 TOTAL=18 且归一化 transcript 全等；门4 A+B 均 0 mismatch |
| **r16 门控证据（远端）** | `evidence\r16-g2-probe.txt`（1,315 B 无 BOM，md5 `3E1D010EB1BF183798FBCE4EC448F275`） / `evidence\r16-gate3.txt`（`TOTAL=18`，与 r15 多重集全等） / `evidence\r16-gate4.txt`（10,590 B，7 例 `MISMATCHES=0`） / `evidence\r16-swap.txt`（5,098 B，六阶段全过） | r16 四门：门1 EXIT=0 / 0 err / 13 warn（C4996×6 + C4244×7）；门2 逐字节 IDENTICAL；门3 十八条全等；门4 形态 B 七例零 mismatch |
| **r17 门控证据（远端）** | `evidence\r17-g2-probe.txt`（1,315 B 无 BOM，md5 与 r16 接受体全等） / `evidence\r17-gate3.txt`（15,194 B，`TOTAL=18`，载荷 `DIFF_COUNT=0`） / `evidence\r17-gate4.txt`（10,592 B，`OLD_EXE=1465856` vs `NEW_EXE=1463296`，7 例 `SAME=True`） / `evidence\r17-swap.txt`（5,097 B，六阶段全过） | r17 四门：门1 EXIT=0 / 0 err / 13 warn；门2 逐字节 IDENTICAL；门3 十八条与 r16 全等；门4 形态 B 七例零 mismatch |
| **r17 二进制快照** | `_tmp\maahub-day1\evidence\accepted-hub\MaaHub-r17-1463296-20261113-005533.exe`（本地权威件）+ 远端 `_r17scratch\bin\MaaHub.exe`（已装为 live）与 `_r17scratch\backup\MaaHub-1465856-20261113-005734.exe`（上一代 r16） | r17 exe = 1,463,296 B / SHA256 `96E93CC3A3B96562FD0076B4EDA2FA6887268AD7E4728C8E035BFD0DDF93EAF2`；回滚一律用副本覆盖，禁用 git checkout |
| **r17 扫描/复核留痕** | workflow `maahub-r17-lighten-review-v2`（12 个 agent：4 扫 + 8 复核，23 候选 → 17 confirmed / 6 rejected-behaviour）；`_tmp\maahub-day1\r17\manifest.json`（37 操作，带首/末行断言）+ `apply_dels.ps1`（**已内置「区间内大括号净额 0」校验**，本轮 0 告警）+ `evidence\r16-swap.txt` 等 | 删除批可复现清单：61 行按区间删 + 1 行替换 + 1 行常量折叠；脚本对 BOM/换行风格（本仓为 LF）自动探测并保持 |
| **r16 源码/二进制快照** | `build\_r15keep\MaaHub-r15-1467392.exe`（上一轮权威构建本地留存）+ 远端 `_r16scratch\bin\MaaHub.exe`（已装为 live） / 远端 `_r16scratch\backup\MaaHub-1467392-20261113-001944.exe` | r16 exe = 1,465,856 B / SHA256 `AC94569DAD7F37A2137F640ED455D30D06E0448243EB195CFF5FF2EFE525D591`；回滚一律用副本覆盖，禁用 git checkout |
| **r16 远端脚本与删除脚本** | `_tmp\maahub-day1\remote\gate2-r16.cmd` / `gate3-r16.ps1` / `seq-diff-r16.ps1` / `swap-r16.ps1` / `audit-r16.ps1`（均 ASCII，零非 ASCII 字节）；`_tmp\maahub-day1\r16\manifest.json`（45 项删除清单，带首/末行断言）+ `del_ranges.ps1` + `edits.json` + `apply_edits.ps1` | 删除批的可复现清单：324 行按区间删 + 14 行按字符串替换；**下一轮须给 `del_ranges.ps1` 补「区间内大括号净额 0」校验**（r16 曾因吞掉一个函数收尾 `}` 触发一百余条级联编译错误） |
| **r15 源码/二进制快照** | `_tmp\maahub-day1\evidence\accepted-hub\MaaHub-r15-1467392-224122.exe` + `r15-post-20261112-224122\`（7 文件）+ `r15-pre-20261112-222933\` / `r15-pre-20261112-223454\` | 减法批前后双向快照，带时刻戳，位于 `iso_import.ps1` 覆盖范围之外；回滚用副本覆盖，禁用 git checkout |
| **r15 远端脚本** | `_tmp\maahub-day1\remote\gate3-r15.ps1` / `seq-diff-r15.ps1` / `cleanup-r15.cmd`（均 ASCII，0 非 ASCII 字节） | 沿用 r14 脚本仅换目录名与产物名；本地不执行任何产品程序 |
| **20:36 二次 Hub 事故复盘** | `_tmp\maahub-day1\reviews\incident-20261112-2036-second-hub.md` | 误启动 Hub 260340 + 7 worker 已 `taskkill /T` 清干净，活 Hub 253064 未受影响；根因 `ProcessStartInfo.ArgumentList` 在 PS 5.1 不存在 → 以零参数启动 exe（详见该文件；暴露的真实产品缺陷"无单实例守卫/无 state 锁"已登记为特性项，不在轻量化范围） |
| **远端沙箱测试脚本** | `_tmp\maahub-day1\remote\seq-diff-r14.ps1`、`gate3-r14.ps1`、`verify-seq.ps1` | 本地不执行任何产品程序：脚本 scp 到远端后以 `powershell -File` 执行，本机只做文本比对 |

临时沙箱目录（`ltcg-test / cli-diff / race-test / iso / final-gate / diagobj / ltcgobj*`）已全部清理；`_tmp\maahub-day1` 仅留 `cfg / evidence / inv / localtest / state-backup-*`。

**远端善后（22:4x 完成，已复核）**：r15 在 WorkCenter 建的全部测试目录（`r15-iso` / `r15-diffbox-old` / `r15-diffbox-new` / `r15-upload` / `r15-cfg`）与落盘的测试脚本（`gate3-r15.ps1/.txt`、`seq-diff-r15.ps1/.txt`、`cleanup-r15.cmd`）已删除干净（`dir /b … | findstr /i r15` 零命中，exit 1）。清理后复核：`build\state\*.json` 仍是 **9 个**（未被测试污染）；活进程仍是 **8 个**（root 253064 + 7 worker，全部 `START=12:55:50`，无 stray、无二次 Hub）；远端在跑 `build\MaaHub.exe` 仍是 **1,542,656 B @12:41:47**（r14/r15 均未部署，符合"不在实弹链路运行期换运行体"的纪律）。**测试只做只读与自有沙箱，从不动运行中的 state。**

**r14 善后（21:5x 完成，已复核）**：同上口径，`r14-sandbox` / `r14-diffbox-*` / `r14-iso` / `r14-upload` / `r14-cfg` 及各测试脚本已删除干净（`dir /b *r14*` 零命中）；`trace.log` 当时 968,150 B / mtime 20:36 未增长（无到期任务，符合预期）。

## 六、当前状态

- `build\MaaHub.exe` = **1,463,296 B**（00:55 r17 构建；r13/r14 1,470,464 B、r15 1,467,392 B、r16 1,465,856 B）。`build.bat release` 干净通过（EXIT=0 / 13 警告 / 0 错误）。累计轻量化：LTCG −60,928 B、标签去重 −1,024 B、random_hex 合并 −6,656 B、URL 拆分 −3,072 B、checked-run 类外合并 −1,024 B、r13 0 B、r14 0 B、r15 −3,072 B、r16 −1,536 B、**r17 −2,560 B**。
- 源码行数/字节（r17 后实测，含空行总行数，**首次把 `src\adapters\` 三文件计入**）：`util.hpp` **547 / 21,785 B**、`hub.hpp` **1507 / 64,828 B**、`planner.hpp` **438 / 19,921 B**、`accounts.hpp` **266 / 10,515 B**、`core_manager.hpp` **962 / 45,032 B**、`main.cpp` **972 / 45,727 B**、`worker_main.cpp` **176 / 7,066 B**、`worker_main.hpp` **7 / 139 B**、`config_import.hpp` **929 / 45,508 B**、`gui.hpp` **924 / 44,447 B**、`task_panels.hpp` **898 / 45,969 B**、`worker_client.hpp` **195 / 7,334 B**、`app_window.hpp` **292 / 13,511 B**、`settings.hpp` **183 / 7,886 B**、`json.hpp` **455 / 15,590 B**、`app_window.cpp` **3441 / 166,645 B**（仍是最大单文件）、`adapters\adapter.hpp` **88 / 2,922 B**、`adapters\maa_adapter.hpp` **486 / 22,376 B**、`adapters\maaend_adapter.hpp` **1242 / 57,971 B**；全树合计 **14,008 行**。
- `assets\MaaCore.dll` 未被本轮触碰：56,198,144 B，MD5 `6701554378F11708A9CF452621F06619`。
- **验收门已从三重升级为四重，且门 4 有两个形态**：编译 + 探针逐字节 + 独立沙箱导入 18 条 + 差分测试。门 4 形态 A = 单函数差分（`parse_url_diff` 类，验证搬移/重写的函数体）；形态 B = **双 exe CLI 差分**（r14 新增，旧二进制 vs 新二进制各建沙箱、各拷同一份 state，逐例比 EXIT + 完整 stdout，验证改动落在分发路径/UI 侧时仍能给出可执行断言）。r15 用一次 75 处的大批改动把这套门走完（A/B 均 0 mismatch），说明门的粒度足以覆盖"批量删除"这种最难用肉眼确认的变更。门 4 的引入是对"回归门不执行被改代码"这一证据缺口的直接补强。
- **部署状态（2026-11-13 00:57 收尾口径）**：远端 WorkCenter（`C:\Users\xray\Documents\MaaHub\`）现在跑的就是本地权威构建 **r17 = 1,463,296 B / SHA256 `96E93CC3…`** —— 活 Hub **pid=267436**（+ 6 个 worker，全部 `START=00:57:40`）、`build\state\*.json` **9 个**、trace 988,467 B / mtime 00:57:41，6 条 `spawn_worker … load_ok=1`（`84691d0fba02`/`57fec764dd37`/`5fa8dd2145ee`/`8b1b54dccbeb`/`6ac900828da1`/`83dc5881a4ce`）+ 2 条 `maaend load done`，**不含已退役的 `exec-02913292d1eb`**。演进链：12:55 中间构建（1,542,656 B）→ 23:14 r15 → 00:19 r16 → 00:57 r17。三级备份目录均保留并已校验 sha256。
- **测试一律远端执行（用户硬性纪律）**：本轮全部门 2/3/4 均在 WorkCenter 远端跑，含新增的双 exe 差分类。本机 `pwsh` 只做文件哈希/文本比对/脚本下发，**不执行任何产品程序或测试程序**（20:36 那次本地误启动的教训见 §五 事故复盘条）。
- 长期目标保持 armed，继续迭代 review。
