# MaaHub 执行器 UI × MAA GUI 任务队列 UI 点对点对比（2026-11-24）

## 0. 口径与基准

| 项 | 内容 |
|---|---|
| 左（本仓库） | `maa-hub-cpp` @ `c249a08`（r32）。实现位置：`src/app_window.cpp`（`page_executor` L2101、`draw_executor_list` L963、`draw_left_rail` L880、`draw_log_column` L1880、`draw_add_task_overlay` L1357、完成后动作 L1470+、右键菜单 L1687）、`src/task_panels.hpp`（`draw_task_panel` L1168） |
| 右（基准） | MAA 官方 WPF GUI = `MaaAssistantArknights` / `MaaWpfGui`。本地快照 `_tmp/maa_xaml/`（含 `OperProgress`/`SwitchTheme`/`UserDataUpdate`/`DepotMaintain` 四个 6.19-beta 新任务类型）。任务队列页 = `Views__UI__TaskQueueView.xaml`（1091 行） |
| 比对方法 | 逐控件对：XAML 属性（Width/MinHeight/Margin/Visibility 绑定/DataTrigger）↔ MaaHub 的自绘常量与分支 |
| 范围 | MAA 执行器的「一键长草」实例页（左列任务列表 / 中列任务设置 / 右列日志 / 底部控制）。MaaEnd（终末地）66 任务、账号/核心/规划/定时/设置等全局页不在本次逐项范围 |

> 命名说明：你写的 `Maven:MaaArknightsAssistant` 按「MaaAssistantArknights 官方 WPF GUI」理解（本仓库所有 1:1 工作一直以该 XAML 为基准）。若你指的是另一个 Maven 坐标的安卓 GUI 版本，基准要换，结论需重做。

**总评**：三列骨架、210/190 卡片尺寸、24.5 行高、220 面板宽、100×50 按钮、完成后三行结构都已按官方常量复刻，属于「像素级对齐」级别。差异集中在三类：
1. **架构性差异**（多执行器模型带来的必然偏离）；
2. **运行态约束缺失**（官方整套 `Run.Idle` 锁定，MaaHub 基本没有）；
3. **版本落后**（官方 6.19 的 4 个新任务类型 + 若干新设置项没有）。

---

## 1. 窗口外壳

| 点 | 官方 GUI | MaaHub | 判定 |
|---|---|---|---|
| 尺寸 | `Width=800 Height=600 MinWidth=800 MinHeight=600` | `800+RAIL_W(190)=990 × 600`，可缩放 | 近似（多出 190px 侧栏） |
| 标题栏 | `ShowTitle=False` 自绘非客户区：彩虹跑马灯（版本/资源更新）+ 标题跑马灯 + 📌 置顶按钮 | 原生标题栏，`SetWindowTextW` 写执行器名（L~830） | **差异**：跑马灯、📌 图标无法实现 |
| 页签 | `TabControlInLine`：一键长草 / 自动战斗 / 小工具 / **设置**（4 个） | 一键长草 / 自动战斗 / 小工具（MaaEnd 执行器只剩 1 个），设置被挪到左侧栏 | **差异**：页签数随执行器类型变化，官方无此分支 |
| 页签样式 | 选中=Primary 文字 + 2px 下划线，hover 仅背景微亮 | 同（L~840 `draw_top_tabs`） | **一致** |
| 切换动画 | 外层 `TransitioningContentControl` 横向滑入 | 页面切换无动画（任务面板切换有 140ms 滑入） | 差异（轻微） |
| 右下挂件 | `hc:GifImage` 100×100，可拖拽，右键换图 | 100×100 占位块，可拖拽，右键显隐 | 部分（无 GIF 资源与换图） |
| 成就弹幕 | 右上 `AchievementGrowlPanel` | 无 | 缺失（非 UI 骨架） |
| 多实例 | 单窗口单实例（全局设置单例） | 左侧栏常驻执行器列表（L963），每执行器独立核心/账号/参数 | **架构性差异** |

---

## 2. 左列：任务列表卡片

| 点 | 官方 | MaaHub | 判定 |
|---|---|---|---|
| 卡片 | `Border Width=210 MaxWidth=210`，`RegionBrushOpacity25` + 1px `BorderBrush`，`CornerRadius=4` | `round_rect` 210×…，card_bg + border，r=4（L2126-2133） | **一致** |
| 内层 | `Grid Width=190`，两行：`*` + `40` | 190 宽 + `ADD_ROW_H=40` | **一致** |
| 列表 | `ListBox Width=190 MinHeight=220 Margin="0,10,0,0"`，无边框、透明底 | 同，含滚动与裁剪 | **一致** |
| 行高 | `MinHeight=24.5`；`Items.Count>9` 时 `MaxHeight=ActualHeight/(Count+1.5)`（下限 24.5） | `item_h=25`；>9 项按 `list_h/(n+1.5)` 压缩，下限 18 | 近似（下限更激进，25 vs 24.5） |
| 选中态 | `NoSelectedHighlightListBoxItemStyle`：选中不高亮、只文字变色 | 同思路（齿轮变 Primary 表示当前面板） | **一致** |
| 状态色 | 4 态：InProgress 蓝 / Completed 绿 / Error 红 / **Skipped 灰（Opacity 0.5）** | 3 态：running / ok / error，无 Skipped 半透明 | 缺失 1 态 |
| 勾选框 | `MaxWidth=140 Margin="4.5,0,0,0"`，`IsHitTestVisible=Run.Idle`（运行中不可改） | 命中区 164px（L2141），运行中照常可点 | **差异**：命中区偏大 + 无运行锁定 |
| 右键勾选 | 勾选置 `null`＝「不保存」，配合设置项 `MainTasksInvertNullFunction` 反转语义 | 右键＝参数「仅生效一次」(`CheckState::Once`)，作用于面板参数持久化 | **语义不同**（同为「不落盘」，作用对象不同） |
| 名称 | `TextBlock Margin="26,0,4,0"`，null 时用 `TraceLogBrush` | 26px 起排，别名可覆盖（`task_alias_`） | 一致 + 额外能力（重命名别名） |
| 齿轮 | 16×16 `RadioButton`，`IsChecked=EnableSetting`（同时是「切到该任务设置」的入口），`GroupName=TaskSettings` | 16px 图标，仅作状态指示（Primary=当前），点行才切换 | **交互差异**：官方齿轮即单选，MaaHub 齿轮不可点 |
| hover 操作键 | `StackPanel Margin="0,0,26,0"`，复制/重命名/删除，仅 `IsMouseOver && Run.Idle && !HideHoverActionButtons` | 3×22px 图标（删除/重命名/复制），hover 即显示，无运行/设置门控（L2218-2275） | 近似，顺序不同（官方 复制/重命名/删除），缺设置开关 |
| 拖拽排序 | `IsDragSource = Run.Idle or !Run.Inited`；ToolTip＝任务顺序说明 | 无状态门控，4px 阈值，插入线绘制 | 功能一致，**缺运行态禁用** |
| 右键菜单 | 执行一次 / 从此开始执行 / 复制（`CanCopy` 门控）/ 重命名 / 删除（`Run.Idle` 门控） | 单次运行 / 从此处运行 / 重命名 / 复制 / 移出列表（顺序不同，恒可用） | 近似 |
| 「复制」语义 | 克隆出**一条新的任务项**（可重复出现同类型任务） | 只给同一项加 `(副本)` 别名，**不能出现第二条**（任务清单是名字集合） | **功能缺口** |
| 任务类型唯一性 | 仅 `StartUp` 限 1 条，其余可重复 | 名字集合天然唯一，无 StartUp 特判 | 差异（复制能力缺失的根因） |

---

## 3. 左列底部 40px 行

| 点 | 官方 | MaaHub | 判定 |
|---|---|---|---|
| 布局 | `Auto / * / *`：MenuButton 30×30（`CornerRadius=4,0,0,4`）+ 全选（stretch）+ SplitButton（`0,4,4,0`） | `button_group {30,0,0}` 三段同高 + 手绘 chevron（L2514-2560） | **一致** |
| 启用条件 | 三者均 `Run.Idle or !Run.Inited` | 无门控 | **差异** |
| 添加任务 | `MenuButton` 弹出 190 宽 `Menu`，条目＝`TaskTypeList`；带**新任务红点**（`IsNew`/`introducedVersion`）；`Custom` 仅调试模式可见；`StartUp` 已有则置灰 | **全屏模态浮层**：搜索框 + 全量勾选列表 + 全选/反选（L1357-1440） | **交互差异**（菜单 vs 勾选面板；无红点/无调试门/无 StartUp 限制） |
| 全选 | 一键全勾 | 同 | **一致** |
| 反选/清空 | SplitButton：主按钮＝当前模式（反选/清空），下拉项 `ChangeInverseMode` 切模式 | 主按钮反选 + 右侧 18px 热区切换「反选/清空」 | 近似（下拉变成热区） |
| 附加 | 列表 ToolTip 说明「可拖拽排序」 | 无 ToolTip | 轻微缺失 |

---

## 4. 「完成后」+ 启动/停止（左列下两行）

| 点 | 官方 | MaaHub | 判定 |
|---|---|---|---|
| 完成后 | `StackPanel Width=210`（内 185）：`ActionTitle` + `Divider` + `ActionDescription`（160 宽居中换行）+ 右齿轮 | 同几何（185/160，L2678-2700），描述串自动拼接（L1554 `post_action_desc`） | **一致** |
| 齿轮语义 | `RadioButton GroupName=TaskSettings` → 中列整体切到「完成后」面板（`ScrollViewer RowSpan=2`，隐藏常规/高级） | 打开浮层（`show_post_action_`），不切中列 | **差异**（浮层 vs 就地面板） |
| 面板项 | 执行一次(Once) / 清空(右键「清空一次」) / 返回模拟器主屏幕 / 退出明日方舟 / 退出模拟器 / 退出 MAA（与「无其他 MAA 运行时」互斥启用）/ 无其他 MAA 运行时 / 锁屏 / 睡眠 / 休眠 / 关机 / **出错时跳过** | 8 项：返回主屏/退出游戏/退出模拟器/退出程序/锁屏/睡眠/休眠/关机 | 缺 4 项（Once、清空、IfNoOtherMaa 独立勾选、SkipOnError） |
| Link Start | 100×50 `Margin=5`，`MaaButtonDefaultStyle`（不是蓝色实底），可见 `!Run.Inited or Run.CanStart`，`IsEnabled=Run.CanStart`；`ShowDeepSleepIcon` 时换成 DeepSleep 图标 | 100×50 白底 Primary 文字（L2700-2716），busy 时整颗换成「停止」 | **一致**（缺 DeepSleep 态） |
| 停止 | 100×50，可见 `Run.Inited && (!Run.Idle || Stopping)`，禁用 `Stopping` | 同（busy→停止） | **一致** |
| 等待 & 停止 | 100×50，`RoguelikeInCombatAndShowWait && !Waiting` 时出现 | 无 | **缺失** |
| 自动重载资源 | CheckBox，`Visibility=CanShowAutoReload`（真有资源要更新才出现） | 只要已连接 + MAA 就显示（L2720+） | 近似（可见条件更宽） |
| 指南模式 | `GuideUserControl` 覆盖整页（`TaskSettingVisibilities.Guide`），首启引导 | 无 | 缺失 |

---

## 5. 中列：任务设置

| 点 | 官方 | MaaHub | 判定 |
|---|---|---|---|
| 列结构 | `Grid` 三行：`*`（MinHeight 360）/ Auto（常规·高级）/ `*`（MaxHeight 250，今日关卡） | 同构，`right_col` + `TG_H=28` + `TIP_H`（L2287-2300） | **一致** |
| 滚动容器 | `hc:ScrollViewer Margin=10` 居中，横竖 Auto | `begin_scroll` + 实测内容高 `content_h_`（L2589-2620） | 近似（无惯性/无横向滚动条） |
| 面板宽 | `SimpleStackPanel Width=220` | `PANEL_W=220` | **一致** |
| 面板外观 | 直接坐在页面背景上：无标题、无卡片、无竖线 | 同 | **一致** |
| 任务切换过渡 | `TransitioningContentControl TransitionOrientation=Vertical TransitionIndex=当前项下标` | 140ms ease-out 滑入，方向由列表位置推导（L2176-2200） | **对齐**（自实现） |
| 常规/高级 | `ButtonGroup Width=220`，高级按钮 `IsEnabled=AdvancedSettingsVisibility` | `option_strip` 28px，仅 `Fight` 有高级页（`task_has_advanced`） | 近似（官方按任务给 `AdvancedSettingsVisibility`/禁用态） |
| 今日关卡提示 | `ScrollViewer Width=200` + `StagesOfTodayInlines`（含颜色 Inlines）+ 右侧 `TooltipBlock`（库存更新时间 `LastDepotSyncTimeText`） | 自行按 `tm_wday` 拼 6~9 行文本（L2225-2255），带「?」提示块但无库存更新时间 | 近似（数据源与 tooltip 不同） |
| PostAction 占位 | 完成后面板占中列两行 | 无（浮层） | 差异 |

---

## 6. 右列：日志

| 点 | 官方 | MaaHub | 判定 |
|---|---|---|---|
| 容器 | `Grid.Column=2 Margin="10,10,0,10"`，Row0 空 + Row1 下载卡片 + 日志 | `log_col` 同位置 | **一致** |
| 顶部 | 无任何工具条 | 状态灯 + 「清空」+「编辑」（L2765-2790） | **额外新增**（官方把这些放在窗口/设置里） |
| 日志样式 | 两种可切换（设置 `UseCardLog`）：纯文本 `ItemsControl`（时间列 Auto + 内容 Wrap，`TraceLogBrush` 时间色）| 仅卡片流：时间 60px 列 + 内容逐字折行，最多 3 行加省略号（L1880+） | 部分（无纯文本模式、无换行数以外的省略策略） |
| 卡片 | `Border Margin="0,0,0,2.5" Padding="2.5,0,1,5"`，`MouseOverRegionBrushOpacity10` + 1px border + `CornerRadius=4`，`MaxWidth=420` | `round_rect` card_bg_soft + border + r=4，间距 3 | **一致**（缺 MaxWidth 约束） |
| 结构卡 | `LogCardViewModels`：段首 `LogSectionDivider` 标题、卡内多列（掉落/公招结果等结构化行）、tooltip 图标 | 全部压平成一行文本 + 颜色；未做结构化卡片 | **缺失**（掉落/公招等结果只显示汇总一行） |
| 下载卡片 | 有 `DownloadLogItemViewModel` 时显示进度卡（下载进度/速度/取消） | 无（核心下载在「核心管理」页，不在实例页） | **缺失**（但功能位置不同，非丢功能） |
| 自动滚动 | `properties:AutoScroll.AutoScroll=True` | 从底部向上堆叠，天然贴底 | **一致** |
| 内容来源 | VM 输出的本地化文案 + 权重/颜色 | `format_maa_event` 手工映射 20+ 事件，并过滤 `ConnectToRemote/TouchMode/ScreencapMethod/...` 底层事件（L1916-1990） | 近似（自建映射表，覆盖不全时回退 `what · task`） |
| 「暂无日志」 | 空态无提示（列表为空） | 显示「暂无日志」 | 轻微额外 |

---

## 7. 任务目录（最大的一处偏差）

官方 6.19 的 `TaskTypeList`（`TaskQueueViewModel.cs` L1747-1758）共 12 项：

| 官方任务 | MaaHub MAA 面板 | 说明 |
|---|---|---|
| 开始唤醒 StartUp | `panel_startup` | ✅ |
| 理智作战 Fight | `panel_fight` | ✅ |
| 基建换班 Infrast | `panel_infrast` | ✅ |
| 自动公招 Recruit | `panel_recruit` | ✅ |
| 信用收支 Mall | `panel_mall` | ✅ |
| 领取奖励 Award | `panel_award` | ✅ |
| 自动肉鸽 Roguelike | `panel_roguelike` | ✅ |
| 自定任务 Custom（仅调试） | `panel_custom` | ✅（MaaHub 恒显示） |
| **干员培养** OperProgress | 无 | ❌ 6.19 新类型 |
| **更新数据** UserDataUpdate | 无（功能在工具箱/核心页？） | ❌ 6.19 新类型 |
| **库存保持** DepotMaintain | 无（有 `Depot` 仓库识别，语义不同） | ❌ 6.19 新类型 |
| **更换主题** SwitchTheme | 无 | ❌ 6.19 新类型 |
| — | `CloseDown`、`AccountSwitch`、`Depot`、`OperBox`、`Reclamation`、`Copilot` | ⚠️ 官方队列里没有：关闭游戏在设置、抄作业在「自动战斗」页、仓库/干员识别在「工具箱」、生息演算已移出队列 |

根因：**任务目录来源不同**。官方是 GUI 里手工维护的 `TaskTypeList`（可含没有直接 core task 的条目）；MaaHub 直接消费核心 `interface.json` 全量任务（`catalog_of` ← `executor_catalog` ← worker），于是「核心有什么就列什么」。

---

## 8. 任务面板逐项抽查（MAA 12 项中已核 4 项）

### 8.1 Fight（理智作战）— 最完整的一项
- 常规页对齐：使用药剂 / 使用源石（`allow_stone_save` 未开时标签带 `*`，官方用 `CalcBinding` 拼星号）/ 指定次数 / 指定材料（可搜索下拉，首项「不选择」）/ 刷取数量·目标库存（`IsSpecifiedDrops` 才出现的第二行）/ 代理倍率 / 关卡·候选关卡 / 添加候选。行距 `RH=42`、左列 `LC=100`、数字框 `70×30` 与官方一致。
- 高级页对齐：博朗台 / 无限吃 N 天内过期药（含 `medicine_expire_days`）/ 活动结束前 48H 吃过期药 / 隐藏代理倍率 / 允许源石保存 / 隐藏当日不开放关卡 / 过期活动关卡重置为 / 使用备选关卡 / 启用周计划。
- **差异**：
  1. 官方「自定关卡代码」勾选后，把常规页的关卡 `ComboBox` **整块换成 TextBox**（同位置互斥切换）；MaaHub 是高级页一个 checkbox，常规页输入框始终可编辑。
  2. MaaHub 高级页多了「上报掉落统计到企鹅物流」——官方此项在设置里，不在 Fight 面板。
  3. 「过期活动关卡重置为 → 指定关卡」的目标输入是 MaaHub 补的（官方用对话框选关），属于实现差异而非缺失。

### 8.2 StartUp（开始唤醒）
- 官方面板确实内嵌连接设置（账号切换+账号名+手动执行 / 启动客户端 / 客户端类型 / 自动检测 / 每次重新检测 / 连接配置模板 / 地址 / ADB 路径 / MuMu 增强 / LD 增强 / 截图·鼠标·键盘方式 / 静音 / 截图测试 / 窗口恢复·最小化 / 断线重连 / ADB 重启 / 触控模式 / 强制替换 ADB）。
- MaaHub `panel_startup` 覆盖 14 项，缺：**连接配置模板**（官方 `ConnectConfigList`，如 MuMuEmulator12/通用）、静音、截图测试、窗口恢复/最小化、断线重连、ADB 重启/强制替换、MuMu/LD 子项（路径、实例号）。
- 另有 3 项官方放在**启动设置**页、MaaHub 放进任务面板：模拟器自动拉起（路径/命令/等待秒）——r31 有意为之，注释已写明「对齐 MAA 启动设置」。
- 数据归属差异：官方是**全局单例**（`ConnectSettingsUserControlModel.Instance`，设置页与任务面板共用）；MaaHub 每执行器独立 `connect` 对象。多实例下这是必要偏离，但意味着「改一次全局生效」的体验不同。

### 8.3 Recruit / Mall / Award / Infrast / Roguelike
- 控件集合基本一一对应（官方 `RecruitTaskUserControl` 的星级确认 4 行 × (勾选+两个数字框)、Tag 策略组；`MallTaskUserControl` 的访问好友/只执行一次/OF-1/购物/只买打折/信用满无视黑名单/保留额度/优先购买/黑名单；`AwardTaskUserControl` 7 个勾选项；`InfrastTaskUserControl` 模式三选一/无人机/设施 8 项 + 上下排序/跨设施组合/专精与补货；`RoguelikeTaskUserControl` 主题/模式/难度/开局干员/层数/投资/结局…）。
- 未逐控件核对到的部分（如 Roguelike 深度选项、Infrast 自定义排班文件选择器的交互）在本文标为「未核」，不要当作已对齐。

---

## 9. 连接 / 启动设置

| 点 | 官方 | MaaHub |
|---|---|---|
| 位置 | 设置 → 连接设置（24 项）+ 启动设置（开机自启/最小化/启动后运行/打开模拟器/PC 端/更新后跳过自启） | 执行器「编辑」浮层（ADB 路径/地址/配置模板/触控模式） + StartUp 面板 + 设置页（代理/CDK/界面） |
| 覆盖 | 24 项全量 | 自认「核心部分」，设置页里明确写了差集（L5411-5417） |
| 多实例 | 单例全局 | 每执行器独立（新增能力，也是偏离） |

---

## 10. 差异分级与建议

**P0（真缺口，建议补）**
1. 缺 4 个 6.19 任务类型（`OperProgress` / `UserDataUpdate` / `DepotMaintain` / `SwitchTheme`）——用户升级核心后在 MaaHub 里完全看不到这些能力。
2. 「复制任务」不生成第二条任务项（只是别名），与官方行为不符；根因是任务清单用 `set<name>`，若要支持需把清单改成有序 `vector<task_id>`。
3. 运行态无全局锁定：官方 `Run.Idle` 门控任务勾选、拖拽、添加菜单、右键菜单、操作键；MaaHub 运行中都能改，可能导致「正在跑的序列」与界面显示不一致。

**P1（对齐差距，影响还原度）**
4. 齿轮不可点（官方齿轮＝单选＝切面板），hover 操作键顺序/门控不同，缺 `HideHoverActionButtons` 设置。
5. 完成后面板缺 Once / 清空 / IfNoOtherMaa / SkipOnError 四项，且用浮层替代了「占中列两行」的面板。
6. 缺「等待 & 停止」（肉鸽战斗中）、DeepSleep 图标态、指南（Guide）模式。
7. 右列缺结构化卡片日志（掉落/公招结果卡）与 `UseCardLog` 开关、下载进度卡。
8. 日志页顶部多出状态灯/清空/编辑三件套（官方没有），属于「设计越界」，需确认是否有意为之。

**P2（细节）**
9. 行高 25 vs 24.5；勾选命中区 164 vs 140；卡片 `MaxWidth=420` 缺失；Skipped 灰态缺失。
10. 添加任务用全屏勾选浮层代替菜单（功能更强但与官方交互不同）；无新任务红点。
11. 今日关卡提示数据源不同（本地周几推导 vs `StagesOfTodayInlines`），缺「库存更新时间」tooltip。
12. 原生标题栏 vs 自绘标题栏（跑马灯/📌 无法复刻）、GIF 挂件为占位。

**本次未核**：Roguelike/Infrast/Depot/OperBox/Reclamation/Copilot 面板内部逐控件；MaaEnd 66 任务面板；快捷键（`HotKeySettingsUserControl`）与成就系统。
