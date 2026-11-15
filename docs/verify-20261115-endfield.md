# Endfield（MaaEnd）实测取证：v1.0.0 二进制在 WorkCenter 的任务终态

- 时间：2026-11-15 23:40 – 11-16 00:20（+08）
- 目标机：WorkCenter `10.10.226.8`（**只读**取证：Get-FileHash / Get-Process / Get-Content / Select-String；未停服、未替换文件、未改 `build\` 下任何内容）
- 触发背景：拟「部署新版本二进制 → Endfield 测试 → 推 gh 并发 release」。本文记录为什么**没有**部署、为什么**没有**发 release。

## 1. 「新版本二进制」并不存在

| 项 | 值 |
|---|---|
| HEAD | `6f65e83`（深度 Review 修订版，纯 docs） |
| tag `v1.0.0` | 附注 tag 对象 `5e9c826` → peel 到 commit `c083d22`（r23，tagger 11-14 20:34:36） |
| `git diff --stat v1.0.0..HEAD` | 只有 README.md 与 docs/review-20261115-deep.md |
| 源码同一性（比 stat 更强） | `src/` tree 两侧同为 `13c4786e…`；`tests/` tree 同为 `1084b4a0…`；`build.bat` blob 同为 `625a133c…` → **零源码差异**；工作树干净、stash 空、无未跟踪源码 |
| GitHub release v1.0.0 附件 | `MaaHub.exe` 1,467,904 B，`digest=sha256:ef7027c8…2846a`，downloadCount=1 |
| WorkCenter 在跑的 exe | `…\Documents\MaaHub\build\MaaHub.exe` 1,467,904 B，mtime 11-14 20:27:13，**sha256 = `EF7027C8…2846A`**（与附件逐字节相同）；7 个进程 Path 全部指向该 exe（父 `--cli serve` pid=285304 + 6 worker，maaend pid=288516 / 285628），起始全部 11-14 20:30:57 |
| 本机 HEAD 重编 exe | 1,467,904 B，sha256 `32A9EF05…C97B` |

本机重编与发布物的差异是**链接噪声**，逐字节比对可证：

```text
diff_byte_count=6   offsets=264,265,266, 1316932,1316933,1316934
peOff=256 → TimeDateStamp  发布物=1794659161(11-14 20:26:01)  本机=1794754035(11-15 22:47:15)
1316932 处是调试目录里同一时间戳的副本；sections / sizeOfImage / magic / LinkerVer 全同
→ 其余 1,467,898 字节逐字节相同
```

所以：**不是新代码，没有可部署的新二进制，也没有可发的新版本。**

- 别拿 r18 那条当理由：`review-20261112-lighten.md:359/:586` 记的是「同尺寸 + 两个不同代码」，它只能说明同尺寸**不可**推定为同一构建；本轮判据是上面的字节级比对（正法见同文档 :195：`/LTCG` 每次链接都写 PE TimeDateStamp，SHA 不能当「同一构建」的判据）。
- 口径风险两处：① 发布物 exe 的 PE 时间戳 11-14 **20:26:01** 比 tag 指向的提交 `c083d22`（20:28:18）早 2 分钟——blob 相同已排除内容差异，但「tag 源码 → 编译出此 exe」的时间链不成立，同一性只靠 blob/字节比对立住；② 远端 `Documents\MaaHub\` 是整仓工作树而非干净安装目录，同树另有 `MaaHub.exe`（1,518,592 B / 11-11）与 `build\MaaHub.exe.bak-*`，认运行体只能靠 sha。
- 发布点核查：仓库无 `.github\workflows`；`gh run list` 空、artifacts total_count=0；GitHub release 仅 `v1.0.0` 一个（非 draft/prerelease，publishedAt 11-14T12:34:55Z）；`git tag -l` 除 `v1.0.0` 只有 `remote-backup-20261103`（orphan 线 `03c78fc`，早于 v1.0.0、与 main 分叉 79/61，不构成更新产物）。**不存在任何比 v1.0.0 更新的二进制。**

## 2. Endfield 任务终态：在 v1.0.0 上是**失败**，不是「测试完成」

Hub 侧调度判定（`build\state\trace.log`，maaend 执行器 `exec-6ac900828da1`）：

| 时刻（本地） | 判定 | 所属构建 |
|---|---|---|
| 11-14 14:25:12 | `-> done` | **早于本次部署**（exe mtime 20:27:13 / 进程 20:30:57），属 r22 旧构建 |
| 11-14 21:34:57 | `-> failed` | v1.0.0 |
| 11-15 07:38:21 | `-> failed` | v1.0.0 |

v1.0.0 自带的 21:30 / 07:30 两个 maaend 定时点**连续两次失败**。14:25 那次成功属旧构建，既不能给 v1.0.0 背书，也不足以断言 v1.0.0 造成回归——能站住的只有「v1.0.0 上这两次实跑失败」。

任务终态**不在 hub 的日志里**（`trace.log` 中 Succeeded / result / poll 关键字命中 0，且 `state.json` 不存任务终态：last_task / task_state / running 命中均为 0）——这是观测盲区，**不能读作「无失败」**。终态唯一来源是 MaaEnd 核心自己的日志：

```text
build\cores\maaend-v3.1.0\debug\maahub-cpp\maafw.log                            42,678,732 B  mtime 11-15 07:38:20
build\cores\maaend-v3.1.0\debug\maahub-cpp\maafw.bak.2026.11.15-07.30.02.910.log  60,710,251 B （含 11-14 21:30 那次）

L142139  [2026-11-15 07:38:20.521] Tasker.cpp L358 task end:
         [cb_detail={"entry":"AutoEcoFarmTask","hash":"344b36514dd086cd","task_id":200000032}] [ret=false]
L142145  Tasker::run_task | leave, 497599ms
         事件计数：Tasker.Task.Starting x1，Tasker.Task.Failed x1

L227496  [2026-11-14 21:34:56.816] task end: [entry=AutoEcoFarmTask, task_id=200000001] [ret=false]
```

同一链路上其它任务的台账（`debug\maahub-cpp\maafw*.log` 汇总）：

```text
AutoCollectSchedule -> Succeeded  x4   （含 11-14 20:32:00.643 [ret=true]）
AutoEcoFarmTask     -> Failed     x2
LaunchGamePC        -> Failed     x2
```

结论强度：**「连接成功 + 任务已投递 + 部分任务可用」成立；「Endfield 测试完成／执行成功」不成立。**

## 3. 根因：v1.0.0 自己宣称修好的失效模式**复发了**

v1.0.0 的 release 说明第 2 条正是「客户区尺寸校正：按 `connect.fit_window` 校正，否则模板匹配静默失败」。实测：

- 校正**确实跑了并返回成功**：`maaend fit client=1280x720`（`maaend_adapter.hpp:943-944`）→ `connect waited st=3000`（=MAA_STATUS_SUCCEEDED）。
- 但抓图帧尺寸**恒为 1200×720**：`roi is out of range [image.size()=[1200,720]] [res=[900,334,350,318]]` 在 07:30:03.153→07:38:00.469 全程出现 **444 次**（07:30 那次 250 次；11-14 部署前的两份日志 0 次）。
- 之后：`Task timeout [pretask.name=SceneAnyEnterWorld] 20452ms` → `node not completed [__AutoEcoFarmRanchExecute]` → `Task timeout [AutoEcoFarmRecoverToMap] 20023ms` → `Tasker.Task.Failed`（整轮 497599ms ≈ 8.3 分钟）。
- 代码层缺口：`util::fit_client_size`（`src/util.hpp:150-160`）只做 `GetClientRect` 比较 + `SetWindowPos(want+frame)`，**不回读渲染缓冲真实尺寸**；`connect()` 只要 `SetWindowPos` 返回成功就继续（`src/adapters/maaend_adapter.hpp:938-947`，校正后仅 `Sleep(500)`）。全链路没有任何一环核对「抓图尺寸 == 模板基准」。
- 另一症状同源：`maaend connected res=0x0`。该值来自 `fn_.MaaControllerGetResolution(ctrl,&w,&hgt)`（`:982-983`）——**返回值被丢弃、从不检查**，`0x0` 是调用未写回时的初值残留。连接本身成功（`st=3000`），但分辨率探测没出值，属 hub 侧观测缺陷：应判失败或换校验通路。

## 4. 因此本轮做的事与没做的事

- **已推送**：`git push origin main` —— `4983cc2..6f65e83`（上一轮的 Review 修订文档）。本文档提交后另行推送。
- **未部署**任何二进制（无可部署的新产物；WorkCenter 仍是 v1.0.0，服务正常，7 进程 + watchdog 心跳正常）。
- **未新建 tag / release，未改动 v1.0.0**。在 Endfield 实跑为 `ret=false` 的情况下发新版并把「Endfield 测试完成」写进说明，等于把一个已知复发的缺陷打包对外背书。
- 附带发现：远端在跑的 maaend 核心是 `maaend-v3.1.0`，**本机 `build\cores\` 没有这一版**（本机为 maaend-v5.15.0-beta.1 / v5.16.0-beta.2），这条失败链无法本地复现；且该核心版本标识三处矛盾（目录名 v3.1.0 / `MaaEnd.exe` FileVersion 2.10.0 / `maafw.log` 自报 v5.16.1）——引用版本别拿目录名当依据。

## 5. 待决策

1. **先修 fit 缺陷**（推荐）：校正后回读真实抓图尺寸（或以首帧 `image.size` 为准），尺寸不等于基准时判失败或自动调分辨率，而不是 `SetWindowPos` 成功即继续；同时把 `MaaControllerGetResolution` 返回值纳入检查。改完在 WorkCenter 重跑 21:30 / 07:30 两个定时点，验收判据取 `Tasker.Task.Succeeded` + `roi out of range = 0`，之后才有「Endfield 测试完成」可写。
2. 或接受现状：v1.0.0 即最新发布物，Endfield 自动化在这台机器上不可用，对外发布继续停在 v1.0.0。
3. 若另有比 `c083d22` 更新的产物（本机仓库、远端工作树、GitHub release/artifact 三处均已排除），给出来源与 sha256，我按同一门重走。
