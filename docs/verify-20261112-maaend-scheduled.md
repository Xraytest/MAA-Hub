# MaaEnd 自动化定时流验证（2026-11-12）

## 目标
证明调度器驱动的 MaaEnd 流程端到端工作：驻留 Hub 的 10s `scheduler_tick` → 真实执行任务列表。

## 注入的探针条目
- `schedules.json` 注入条目：`exec-02913292d1eb`，kind=maaend，label=DailyFull-probe，
  weekdays=[4]（周四），times=["00:37"]，source="probe:scheduled"，enabled=true。
- 备份：`schedules.json.bak-probe7-20261112.json`（16 条原始）。
- task-lists.json：`exec-02913292d1eb`「全套日常」= 16 项 maaend 列表（AutoEcoFarm … VisitFriends），
  task-params.json 15 组参数与 2026-11-10 成功 DailyFull 相同。

## 时间线（2026-11-12）
| 时刻 | 事件 | 证据 |
|---|---|---|
| 00:26:32 | probe13 杀主 Hub（pid 248140）+ 启动游戏 Endfield（pid 253584，计划任务 MaaHubProbeGame，K:\Arknights Endfield\Endfield.exe） | watchdog.log `DEAD (no main process)` |
| 00:27 | 截图确认游戏停在启动器页 | probe_shot.png |
| 00:32:46-00:33:01 | click2.ps1 经计划任务 MaaHubProbeAct 点击「开始游戏」按钮 3 次（窗口 Endfield rect=312,136,1608,943，点击 1438,893） | click2.log |
| 00:29:03 | watchdog 重启主 Hub pid=241840 + 6 worker（3 maa + 3 maaend 预 spawn），加载 17 条 schedules | watchdog.log `create rc=0 pid=241840`；trace `scheduler_loop start`=1794414543 |
| 00:37:04.1 | **scheduler fire**：懒生成 maaend worker（pid=250500）→ `maaend connect ctrl=… hwnd=8653708`（游戏窗口句柄）→ `res=0x0` → `post AutoEcoFarm entry=AutoEcoFarmTask override={RanchJob/SisPaintingFarmJob/WulingCityFarmJob enabled=true}` | trace.log |
| 00:37:04 | schedules.json 重写，probe 条目 `last_run_date: "2026-11-12"`（fire 前置写入防重复） | schedules.json mtime 00:37:04 |
| 00:37:05→00:44 | AutoEcoFarm 场景识别全程 miss：首帧 00:37:05.336 `ScreenshotStableRecognition` ret:false（正常值），图像流正常（1200x720，~3.84s/帧，uuid 各异） | maafw.log |
| 00:41:49 | 首农场 `__AutoEcoFarmWulingCityFarmExecute` 404647ms 超时 leave | maafw.log |
| 00:41:49→00:45:29 | FailureCollector 3 轮（子任务 20s + RecoverToMap 20s）全败，action ret:false | go-service.log action.go:60/64 |
| 00:45:30.6 | `[sched] exec-02913292d1eb -> failed`（触发+506s，其余 15 任务未发布——首败即整序列 failed，符合 run_task_sequence 设计） | trace.log 1794415530.6 |
| 02:00:08 | **Maa5（kind=maa）Timer1 准点触发**：`exec-5fa8dd2145ee` MuMu adb 127.0.0.1:16384 连接 1874ms → 1280x720 → StartUp（游戏已在主界面）→ Award → … | asst.log Px253132 |
| 02:23:29 | **Maa5 `-> done`**（运行 23m21s，Recruit 02:23:28 AllTasksCompleted）；Timer1 lrd 推进 2026-11-12 | trace.log 1794421409.2；asst.log |

## MaaEnd 失败 RCA（根因：游戏画面内容非预期，非截图/调度回归）

### 证据链
1. **截图通道正常**：`Transceiver::handle_image` 自 00:37:05 起 ~3.84s/帧持续供帧，`cols=1200,rows=720,size=2592000`，uuid 各不相同（未冻结）。maafw.log 00:37-00:47 **零** WRN/ERR。
2. **`ScreenshotStableRecognition` ret:false = 正常值**：11-05 成功基线（maafw.bak，474 次请求）同样全部 ret:false——该自定义识别仅在检测到截图异常时 ret:true。判定「截图稳定性门槛命中」的早期推断**被推翻**。
3. **真实失败机制**：进图模板识别（`SceneEnterMapWulingSisPainting` → `__ScenePrivateMapAnyEnterMapWulingSisPainting`，nodes.json:89328 起）404s 全程 miss → 首农场超时 → FailureCollector 3 轮恢复全败（go-service.log `FailureCollector subtask failed` ×3 + `recovery subtask failed` ×3）→ action ret:false → 序列 failed。
4. **11-10 手动会话对照（同一核心，bak 日志）**：三个农场**同样全部失败**（CityFarm 28s、SisPainting 320s、Ranch 403s，RecoverToMap 均 20s 超时）——农场失败模式在手动会话早已存在，仅差异是当时未触发 FailureCollector 中止、任务列表继续跑完。**非 Hub 调度/适配器回归。**
5. **游戏画面内容**：游戏由计划任务自启 + 程序点击「开始游戏」进入，当晚停留在启动器/登录类界面（自启后无人干预过开场流程）；进图模板自然 miss。11-05 基线成功的前提是游戏已由人工推进到主界面。

### 佐证（环境提示，非本次命中）
资源 locale `task.SceneManager.focus.screenshot_stable_abnormal`（locales\interface\{zh_cn,en_us,ja_jp,ko_kr,zh_tw}.json 各 :2578）：
「请使用 D3D11 启动游戏，或切换到窗口化且游戏分辨率小于桌面分辨率」——官方对 GDI/ScreenDC 捕获独占全屏失效的修复指引。

### trace epoch 对照
| epoch | 本地时刻 |
|---|---|
| 1794412800 | 2026-11-12 00:00:00+08 |
| 1794414543 | 00:29:03（scheduler_loop start） |
| 1794415024 | 00:37:04（maaend connect） |
| 1794415530 | 00:45:30（maaend -> failed） |
| 1794421409 | 02:23:29（Maa5 -> done） |

## kind=maa 定时链路（Maa5 02:00）✅ 端到端成功
- `exec-5fa8dd2145ee`（MAA Default @MaaAssistantArknights_5）Timer1 hour=2/min=0：02:00:08 fire → adb 连 MuMu（127.0.0.1:16384）1874ms → StartUp 02:00:12 完成（游戏已在主界面，AccountSwitchTask "task disabled, pass"）→ Award → … → Recruit AllTasksCompleted 02:23:28 → trace `-> done` 02:23:29（23m21s）。
- lrd 推进链：Timer1 `2026-11-11`→`2026-11-12` ✓；Timer2 hour=8 lrd=`2026-11-11` 待今日 08:00 触发。
- Maa1（exec-84691d0fba02）Timer2 hour=6 lrd=""（导入条目首触发）今日 06:00 将真实触发。

## 关键结论
1. **调度器→执行器全链路工作**（maa 与 maaend 双 kind 同夜验证）：10s tick → hh:mm 精确匹配 → lrd 前置写 → spawn/connect/post → 终态回写 trace。
2. **watchdog 重启后调度照常**：主进程被杀 → ≤5 分钟自动重启 → 新主进程加载 schedules → 准点 fire。
3. **MaaEnd failed 是游戏画面状态问题，不是 Hub 缺陷**：截图/识别/控制通道全部健康；农场失败模式与 11-10 手动会话一致。
4. **首败即整序列 failed 是设计行为**（hub.hpp run_task_sequence :728）：15 个后续任务未发布属预期。

## 建议（使 MaaEnd 定时流真正跑通）
1. 触发前保证游戏停在主界面：游戏内关闭「退出后回到启动器」类设置，或 Hub 触发前加 LaunchGame/进主界面前置任务（资源含 tasks/pretasks/LaunchGamePC.json）。
2. 显示环境按官方指引：D3D11 启动、窗口化且分辨率 < 桌面分辨率（防 GDI/ScreenDC 捕获异常，本夜未命中但为既有风险）。
3. 备选：maaend_adapter screencap_method=16（PrintWindow）替代默认 32（ScreenDC）。

## 已知问题（非本次缺陷）
- AutoEcoFarm 3 农场子任务 FAIL 为既有问题（11-10 手动会话同现）。
- GDI/CopyFromScreen 截屏 GPU 渲染游戏窗口得白屏；MaaFW 自身截屏通道正常。

## 复核命令（§9）
```powershell
# watchdog 状态
Get-Content C:\Users\xray\Documents\MaaHub\build\state\watchdog.log -Tail 10
# trace 调度行（epoch 减 1794412800 = 本地时刻）
Select-String -Path C:\Users\xray\Documents\MaaHub\build\state\trace.log -Pattern "\[sched\]|maaend post"
# 进程
Get-CimInstance Win32_Process -Filter "Name='MaaHub.exe' or Name='Endfield.exe'"
# schedules lrd
Select-String -Path C:\Users\xray\Documents\MaaHub\build\state\schedules.json -Pattern "2026-11-12"
# FailureCollector（maaend 失败链）
Select-String -Path C:\Users\xray\Documents\MaaHub\build\cores\maaend-v5.15.0-beta.1\debug\go-service.log -Pattern "FailureCollector"
```

## 待办
- [x] 定时触发证据（trace fire 行 + lrd 前置写）
- [x] 失败 RCA（本文档「MaaEnd 失败 RCA」节，probe29/31/32 三轮证据）
- [x] kind=maa 对照成功（Maa5 02:00）
- [ ] Maa1 06:00（lrd=""）触发观察——**观察前勿重启 Hub**
- [ ] Maa5 08:00 Timer2 触发观察（lrd 推进 2026-11-11→2026-11-12）
- [ ] 清理：MaaHubProbeGame/MaaHubProbeShot/MaaHubProbeAct 计划任务；删 probe 条目 exec-02913292d1eb（lrd 已=2026-11-12，下次触发 11-19，清理前删掉即可）；决定是否从 .bak-probe7-20261112.json 恢复 schedules.json
- [ ] 导入 Maa1/3/5 → 忠实还原定时器 → 关闭原 MAA 客户端
