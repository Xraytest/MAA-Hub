# MaaHub 忠实部署记录（TaskQueue 导入上线 + 原 MAA 客户端退役）

- 时间：2026-11-12 12:55:18–12:55:58（+08），WorkCenter `10.10.226.8`
- 部署脚本：`D:\WorkSpace\_tmp\maahub-day1\deploy_faithful.ps1`（输出 `evidence\deploy-faithful-20261112.txt`）
- 回滚脚本：`deploy_rollback.ps1`（已上传远端）；状态备份 `build\state\bak-deploy-20261112-125518\`（8 文件含旧 exe）
- 新 exe：1,542,656 B（含 TaskQueue 导入 + hub.hpp 六项修复，本机 `build.bat` 编译）
- 时序说明：原计划 18:00 后部署，实际提前到 12:55——08:20 之后到 18:00 之间无任何触发点，空窗安全；且让 **18:00 maa1 成为忠实配置的首个实弹验证点**（已设 19:15 核验提醒）。

## 1. 部署动作与结果（10 步全绿）

| 步骤 | 结果 |
|---|---|
| 备份 state（8 文件 + 旧 exe） | ✅ `bak-deploy-20261112-125518` |
| 停看门狗 → 停 Hub（7 进程） | ✅ 3 秒内全停 |
| 换 exe | ✅ 1,542,656 B 落地 |
| CLI 导入 ×3（Hub 停止态） | ✅ maa1 复用 `exec-84691d0fba02`、maa5 复用 `exec-5fa8dd2145ee`、maa3 新建 `exec-57fec764dd37`；各 **36 参数**，exit=0 |
| 账号 | ✅ 新增 ak152 `152****7662`（acc_91426dde282d1488）、ak151 `151****5785`（acc_9f0b4d19e7571ce0）；ak180 复用 |
| link | ✅ maa1→180 / maa3→151 / maa5→152 |
| set-connect ×3 | ✅ 统一 MuMu adb 模板（`127.0.0.1:16384`，幂等） |
| 启 Hub（WMI Create） | ✅ 主 pid=253064 + 7 worker，`load_ok=1` 全数回执 |
| 恢复看门狗 | ✅ enabled |
| 核验 | ✅ 见下 |

## 2. 忠实性核验（部署后只读采集）

- **task-params**：maa1/3/5 各 6 任务全部落盘。逐项对照权威快照：maa1 `drones_idx=5`(PureGold)/`times=300`/`stage=TL-4`/`medicine=0`/confirm T,T,T,F；maa3 `drones_idx=1`/`times=40`/`stage=1-7`/`medicine=5`/confirm F,T,T,F；maa5 同 maa3 但 `stage=AP-5`。
- **UTF-8 字节级**：`blacklist_str`/`buy_first_str` 在文件中为正确 UTF-8（`e58aa0…`/`e68b9b…` 命中、无双编码迹象）；控制台乱码仅为 GBK 渲染假象。
- **schedules**：25 条 / 9 启用。maa1 补齐 0/12/18 点（浸泡期只有 06:00）；maa3 首次入场 3:00/7:00；今日剩余触发点仅 **18:00 maa1**（lrd 空，会触发）。全天触发点 0/2/3/6/7/8/12/18 互不重叠，同模拟器串行安全。
- **原 MAA 客户端退役**：部署后 `maa-client-count=0`；Run 键/计划任务/启动文件夹均无 MAA.exe 自启（H1/H2/H4 侦察确认）——替代目标达成。Maa6 按计划**未导入**。
- 清理：3 个 MaaHubProbe* 计划任务已删除；Watchdog 保留。

## 3. 待观察与遗留

1. **18:00 maa1 忠实链首跑**（19:15 自动核验）：重点看 AccountSwitch 切 180、链序 StartUp→Recruit→Infrast→Fight→Mall→Award、Recruit 300 次实执行、PureGold 无人机、8 设施换班、Fight TL-4。
2. 探针执行器 `exec-02913292d1eb` 及其 `probe:scheduled` 条目仍在（周四 00:37，今日 lrd 已消耗）：**11-19 前需删**（无 CLI 删除命令，需 GUI 删除或停 Hub 编辑 schedules.json）。
3. `schedules.json.bak-probe7-20261112.json` 等历史备份可在稳定一周后清理。
4. 已知简化：Fight 只取 StagePlan[0]（原 MAA 会轮换三关）；UserDataUpdate/DepotMaintain（maa1 独有）不导入——如需再评估建模。
5. 深度评审 workflow v2/v3 均因卡死被弃；结论以直接实证为准（编译、导入实测、normalize/order 探针、字节校验、实机部署核验）。
