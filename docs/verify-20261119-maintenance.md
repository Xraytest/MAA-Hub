# 检修 2026-11-19：MaaEnd 执行器体检 + 三处修复

范围：MaaHub 全链路体检（MaaEnd 执行器为主），只修实证缺陷，不引入新行为。

## 体检结果（全绿）

| 项 | 结果 |
|---|---|
| state 7 个 JSON 完整性 | executors=5 / cores=2 / schedules=24(enabled=8, maaend=0) / task-params maaend SellProduct 8 键 / task-lists 3×6 / state.json OK |
| 游戏路径 | Endfield.exe ✓ Arknights.exe ✓（config.apps.*.game_exe 均真实存在）|
| core / executor 目录 | maaend core v3.0.0 (maafw v5.16.0-beta.2) 目录齐全；exec-5418b4e55d78 六子目录 + interface.json 齐；tasks 58/58、resource 2392/2392 与 core 完全同步；.core_sync 5 个执行器均指向正确 core |
| CLI 冒烟（重建后 exe） | ping=CLI OK / devices=5 exec 全部 healthy（maaend fw_log=ok tasks=56）/ sched-list=24 条 / tasks exec-5418b4e55d78=56 任务，全部 rc=0 |
| 上游版本 | **MaaEnd v3.1.0 可用（has_update=1，asset 317MB）未安装**（settings.update_auto_install=false，尊重用户策略）；MAA v6.22.0 已最新 |
| 回归 | UI_RUNNER_TEST PASS（exit 0），重建后复跑仍 PASS |
| config.json install_dir 遗留字段 | 指向不存在的 D:\Application\*，但 hub.hpp:2063 运行时一律以 exec_dir/core.path 覆盖 → 非缺陷，不动 |

## 修复（3 处）

1. **build\state\settings.json 带 UTF-8 BOM**（PowerShell 5.x Set-Content 风格写入残留）。
   hub 自身解析容忍（json.hpp:440），但严格 JSON 工具全部炸。已剥 BOM（592→589B，内容逐字节不变，回读 parse OK）。
2. **--cli help 误入 executor 解析路径**：5 执行器时 resolve("") 为空 → "executor not found: "（rc=1，误导）。
   main.cpp:169 增 help/--help/-h 显式分支 → usage 直出 rc=0；空命令保持 rc=2 兼容。build.bat release 重建 + help/ping/devices 冒烟 + 回归复跑全绿。
3. **C:\Temp 陈旧 maafw-agent-*.sock 1727 个**（2026-07-06 起堆积，0B，全部死 socket）。
   确认零 MaaHub/MaaEnd 进程后全数删除；当日冒烟的 socket 均随进程退出自清 → 新代码收尾干净。

## 遗留（不修，如实记录）

- MaaEnd 上游 v3.1.0 有更新：一条命令 `build\MaaHub.exe --cli core-update`（下载 317MB 并设为默认 core）；是否升级由用户定。
- MaaEnd 无任何定时条目（schedules 里 24 条全是 MAA）：终末地侧当前无自动触发，属预期配置而非缺陷。
- 终末地/明日方舟客户端均未运行：executor conn=unknown 是"无游戏窗口"的正常状态，启动游戏后 hub 可连接。
- 无 MaaHub 常驻进程：8 条 enabled 的 MAA 定时只在 GUI 或 --cli serve 常驻时才会到点触发。
