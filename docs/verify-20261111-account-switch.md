# MaaHub 账号切换（AccountSwitch）端到端验证

- 测试时间：2026-11-11 18:25–19:12（+08），WorkCenter `exec-84691d0fba02`（maa1 执行器）
- 证据重建：2026-11-12（本文档为补记；原始操作脚本 `acc_switch152.ps1`/`acc_restore.ps1`，日志证据 `evidence\acc-evidence.txt`，截图 `evidence\acc-switch-152b.png`、`evidence\chain-1912.png`）

## 1. 机制

`hub.hpp::run_on_executor`（:463-470）对 `StartUp/CloseDown/AccountSwitch` 三类任务调用
`AccountStore::apply_to_task`（`accounts.hpp:194-233`）：执行器绑定了启用账号且任务参数里
`account_name` 为空时，注入账号 `identifier`（登录名片段）。MaaCore 的 `AccountSwitchTask`
在 StartUp 链内执行登出→切号 pipeline，按 identifier OCR 匹配选人。

## 2. 实测证据（asst.log 逐条）

| 时刻 | 事件 | 结论 |
|---|---|---|
| 18:25:54 | `task disabled, pass {AccountSwitchTask}` | 未绑定账号/开关关闭时子任务被干净跳过 ✅ |
| 18:26:30–18:26:49 | `AccountSwitchTask::_run` 完整 pipeline：`SwitchAccount@StartUpBegin` → `AccountManagerOfficial` → 返回按钮 → `Settings` → `AccountLogout` → `AccountLogoutConfirm`（8 次重试等加载）→ `AccountManager` → `LoginOther` | 登出流程全通 ✅ |
| 18:52:41 | `append_task StartUp {"account_name":"152****7662","client_type":"Official","start_game_enabled":true}` → 18:53:19 `TaskChainCompleted` | 注入生效、切号成功 ✅ |
| 18:53 | 截图 `_sw152b.png`：主界面 **Molis / ID 337177326**（152 账号） | 视觉实锤 ✅ |
| 18:57:45 | `append_task StartUp {"account_name":""}` → 不切号，当前账号直接进游戏 → 整串 6 任务 19:12 done | 空 identifier 回退正常 ✅ |

## 3. 账号数据生命周期（注意）

- 测试期临时账号 `acc_5e15276620000001`（152）由 `acc_switch152.ps1` 直写 accounts.json 创建；
  `acc_restore.ps1` 用 `bak-acc-20261111-182548` 回滚后**该 id 已不存在**。
- 2026-11-12 忠实部署改用 CLI `add-account` 重建：ak152=`acc_91426dde282d1488`、
  ak151=`acc_9f0b4d19e7571ce0`（identifier 不变，id 不同），并按账号库权威绑定
  maa1→180 / maa3→151 / maa5→152。
- 18:00 maa1 忠实链首跑即该注入路径在新部署下的首次实弹验证（19:15 自动核验提醒已设）。

## 4. 已知边界

- identifier 用掩码手机号片段（`152****7662`），依赖 MaaCore 对 `*` 的 OCR 模糊匹配；
  若登录名不含掩码片段（如纯昵称），需改用 LastFourDigits 匹配模式（MaaEnd 路径同款逻辑）。
- `account_name_set` 面板开关只控制 UI 显隐，真实门控是"执行器是否绑定启用账号"。
