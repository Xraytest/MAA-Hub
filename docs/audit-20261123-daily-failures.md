# 审计 2026-11-23：每日自动化未正确执行——根因定责

数据源：WorkCenter 实机全量日志包（11-23 12:38 经 chatwg 隧道 HTTP 通道回收，
`_tmp/maahub-audit-1123/`）。对象：`build\state\*`、`cores\*\debug\**\*.log`（30 个最新）、进程快照。

## 结论（一句话）

调度器本身每天都在正常触发；失败的是执行层——**① MaaEnd（终末地）的 go-service 代理
11-21 07:45:44 挂死后再未响应**，此后每轮日常 16 个任务全部以「8 秒代理超时」空转（且首任务
ret=true 造成假成功）；**② ak180 的 Fight 关卡配置 TL-4 在 11-22 活动结束后不可达**，
Fight 链连败；**③ 11-19~11-21 期间另有 screendc 前台抢占失败风暴**与**僵尸游戏客户端**
（Endfield.exe 自 11-21 07:30 挂机 2.5 天未退出）叠加，导致系统无法自愈。

## 各执行器现状（截至 11-23 12:38）

| 执行器 | 游戏/账号 | 关卡/内容 | 调度 | 近况 |
|---|---|---|---|---|
| exec-6ac900828da1 | 终末地 | 16 任务日常 | 07:30+21:30 | **≥6 天无一次 done**（failed/partial 连贯） |
| exec-84691d0fba02 | 明日方舟 ak180 | **TL-4** | 00/06/12/18 | 11-22 06:17 起连续 6 次 partial（Fight 导航失败） |
| exec-5fa8dd2145ee | ak152 | AP-5 | 02/08 | 健康 |
| exec-57fec764dd37 | ak151 | 1-7 | 03/07 | 健康（11-18 部署日偶发 failed） |

## 根因 ①（主因）：hub 从不排空 agent 子进程的 stdout 管道 → go-service 写满 4KB 后永久阻塞

> 2026-11-23 晚间补勘后改写：先前版本写「挂死原因不明、根因不在 hub」——**有误，主根因在 hub 侧**。

完整机制（源码 + 日志双证，两次独立复现同位）：

1. **hub 侧**：`start_agents()`（maaend_adapter.hpp:793）用 `util::spawn_process`（util.hpp:279）
   拉起 go-service.exe——`CreatePipe(&out_read, &out_write, &sa, /*nSize=*/0)`（**默认 4KB 缓冲**）
   作为子进程 stdout，`out_read` 存进 `ProcHandle.stdout_read` 后**全程无人读取**
   （`agent_procs_` 只用来查 `alive()`；唯一的管道消费者 `run_capture` 仅用于 CLI 探测且读完即杀）。
   stderr 被导向 NUL。
2. **MaaEnd 侧（设计如此）**：`dynamic_html_log.Print()` → `maafocus.PrintLargeContentTrimNewline`
   → **`fmt.Println(html)` 写 stdout**（v3.1.0 `agent/go-service/pkg/maafocus/maafocus.go`）。
   官方客户端由 MaaFW client 模式拉起 agent 并持续消费 stdout 落盘 `debug/go-service.log`；
   hub 改为自行 spawn 却不消费——zerolog 走自己的文件所以 `go-service.log` 里一条 HTML 都没有
   （grep=0，旁证管道确实没人接）。
3. **卡死点**：`CountDownAction`（`common/countdown/countdown.go`）= 5 秒可中断等待，
   每 500ms 轮询一次并 `dynLog.Print()` 一条倒计时 HTML，**每次调用约写 1.3KB stdout**。
   AutoEcoFarm 的日常序列（SellProduct→…→DeliveryJobs 导航→…→AutoEcoFarm）打印量确定，
   **3 次倒计时 ≈ 4KB 写满管道，第 4 次倒计时中途 `fmt.Println` 永久阻塞**：
   - 11-18 21:48:47（第 4 次调用，首条 Print 即卡，worker 只收到 1 次 tasker_stopping 轮询）
   - 11-21 07:45:45（第 4 次调用，写到第 9 条 Print 卡在 T+4.0，之后全静默）
4. **为什么一卡全卡**：MaaEnd agent 的请求派发是**单线程**（Px383544 全部请求都在 Tx18308 串行处理），
   一个 handler 阻塞 → 派发线程不再受理任何新请求 → 之后所有任务每个都等满 hub 设的
   `MaaAgentClientSetTimeout 8000ms`（maaend_adapter.hpp:855）超时空转——「每任务 ~9 秒即败」。
   进程存活、socket 未断（worker 侧全部超时签名，无一次 connection reset）、stderr 空（无 panic）
   → **僵尸而非崩溃**。
5. **为什么永不自愈**：hub `status()` 的 `agents_alive` 用 `MaaAgentClientAlive()`（只看 socket/进程存活），
   永远报 2/2；watchdog 只数进程数（OK procs=7）；hub 只有重启（worker 重建 agent）才能恢复，
   而 11-21 02:02 之后无人重启——僵尸保持 2.5 天+。
   - 假成功陷阱：SellProduct 在代理死后仍走 End 节点返回 `ret=true`（识别超时被当「未安排」）。
   - 复合：Endfield.exe 自 11-21 07:30:15 挂机不退，`prepare skip: game window already present`
     永远跳过重启客户端；11-19/20 两天的 partial 是带着 11-18 已死的代理跑的（部分任务靠
     sidecar 导航还能干真活，如 DeliveryJobs 跑 2~7 分钟），叠加前台风暴。

修复方向（**未实施**，本轮只定责）：
- 治本（hub）：`start_agents()` 给 agent 子进程的 stdout 接一个排空 reader（或直接重定向到 NUL，
  与 stderr 一致）；任选其一都能让 dynamic_html_log 永远写不满。
- 兜底（hub）：一轮内 agent `send_and_recv` 超时 ≥3 次 → 判定代理僵尸：`TerminateProcess` +
  重建 worker/agent（现有 `agents_alive` 假健康必须换掉）。
- 可选（上游）：MaaEnd `PrintLargeContent` 系列「写死 stdout 无背压」的设计可反馈给 MaaXYZ；
  上游已有 v3.2.0/v3.3.0-beta，升级前值得先核 changelog（本轮网络只读受限未能取到）。

## 根因 ②：ak180 Fight 关卡 TL-4 失效

- task-params.json：`exec-84691d0fba02.Fight.stage = "TL-4"`（活动关）。
- asst.log 11-23 12:11：`TL-4@TL-OpenOpt` / `LastBattleStageName` SubTaskError →
  `StageNavigationTask` 失败 → `TaskChainError: Fight`。
- 11-22 00:24 尚 done，11-22 06:17 起 partial ×6——**TL 活动约 11-22 凌晨结束**。
- 账号 daily_done_date 仍推进（日常链其余任务完成），仅刷理智环节失效。

## 根因 ③（历史帮凶）：screendc 前台抢占失败风暴

- 11-19 21:30 → 11-21 07:30 三轮，`ScreenDCScreencap ... Failed to ensure foreground
  window before screencap` 每轮 445–895 次：游戏窗口无法置前台（被遮挡/会话非交互），
  截屏内容错→识别大面积失败。
- 11-21 07:30 轮 fg 警告消失（部分任务真实执行），却在该轮 07:45 触发代理挂死——两代故障交接。

## 立即恢复（WorkCenter 上执行）

1. 结束僵尸客户端：`taskkill /f /im Endfield.exe`。
2. 重启 MaaHub（托盘退出→`MaaHubScheduler` 任务拉起，或直接重启 worker 链），连带重建
   go-service 代理与新 socket；顺手清 `C:\Temp\maafw-agent-*.sock` 陈旧项（11-19 曾清 1727 个）。
3. ak180 的 Fight 关卡改为常驻关（1-7 / AP-5）或当前可用活动关。
4. 恢复后观察当晚 21:30 终末地轮：go-service.log 应有新输出、任务耗时恢复分钟级。

## 工程加固（MaaHub 缺陷登记，按修复优先级）

- **P0（治本）agent 子进程 stdout 排空**：`spawn_process` 给 agent 的 stdout 管道（4KB 默认缓冲）
  全程无人读，任何子进程写满即冻结。给 `agent_procs_` 接排空 reader 落盘（对齐官方客户端
  消费到 `debug\go-service.log` 的行为），或对 agent 子进程把 stdout 一并重定向 NUL（与 stderr 一致）。
  注意 `run_capture`/CLI 路径依赖现有管道行为，只动 `start_agents()` 一处即可。
- **P0（兜底）代理健康探测**：一轮内 agent send_and_recv 超时 ≥3 次 → 判定僵尸 →
  `TerminateProcess` + 重建 worker/agent，本轮标 failed。`MaaAgentClientAlive` 的「socket 活=健康」
  假设必须换掉，否则 P0 修好后仍需要这层保险。
- **P1 ret 真实性**：agent 超时导致的「End 节点 ret=true」应在 hub 侧折算为 partial/failed
  （SellProduct 假成功必须消除）。
- **P1 客户端寿命检查**：maaend prepare 增加游戏进程启动时长检查（>24h 先退出重启），
  消灭「窗口存在但僵死」。
- **P2 screendc 前台依赖**：评估截屏方式改 dxgi/窗口位捕获，或在运行前强制置顶+校验前台
  成功；11-19~21 的 fg 风暴诱因（谁抢了前台）值得复演。

## 环境事实备注

- WorkCenter `xray` 为空密码本地账户，Windows 策略禁空密码网络登录（SSH/SMB 不可用）；
  本次经 chatwg 隧道 HTTP 通道回收日志（Mac 端 10.10.226.14:8765，防火墙放行的框架版
  Python 承载，收完即关）。
- 部署版本核对（exe 哈希）本轮因收集脚本 W() 函数 bug 缺失，下次补。
