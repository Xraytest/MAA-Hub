# 验证报告 — Maa/MaaEnd 核心自动更新（2026-11-10 夜）

对象：`MaaHub.exe`（`build.bat release` 构建，提交 `652b873`）
结论：**自动更新链路端到端通过**（本机真实下载 + 校验 + 安装 + 二次检查）；ENG01 受国际带宽限制无法在可接受时间内完成 315 MB 下载，属环境问题，非代码缺陷。

## 一、对齐 MAA 官方 Update 组

设置持久化（`state\settings.json` → `startup_update.*`）：

| 键 | 取值 | 含义 |
|---|---|---|
| `update_check_on_start` | bool | 启动时检查一次 |
| `update_check_on_schedule` | bool | 运行期间每 6 小时检查 |
| `update_auto_download` | bool | 检查到更新自动下载 |
| `update_auto_install` | bool | 下载后安装并设为默认核心 |
| `update_version_type` | stable / beta / nightly | 渠道：`/releases/latest` 或 `/releases?per_page=20` 过滤 tag |
| `update_source` | auto / github / mirrorchyan | 更新源顺序 |
| `github_token` | string | 提高 API 限额（Bearer） |

旧键 `update_force_github` 已废弃，读入时静默忽略（老配置不报错）。

## 二、更新链（真实运行）

```
检查(GitHub API) → 选资产 → 下载 → sha256 校验 → tar 解包 → 探测包内版本
→ 同版本/同 tag 复用并回填 release_tag，否则登记为新核心 → 设为默认核心 → 清理 .tmp
```

### 2.1 MaaEnd（复用 + 回填路径）

| 阶段 | 输出 |
|---|---|
| 更新前检查 | `CURRENT version=v5.15.0-beta.1 tag=`（空）→ `has_update=1 tag_mismatch=1` |
| 选中资产 | `MaaEnd-win-x86_64-v3.0.0.zip` 315,946,882 B `sha256:36cd998f…e4d` |
| 独立复核 | PowerShell `Get-FileHash` 与上游 digest 逐字符一致 |
| 安装 | `UPDATED id=core-8717ea7f169ebf10 version=v5.16.0-beta.2 tag=v3.0.0 is_default=1` |
| 更新后检查 | `CURRENT version=v5.16.0-beta.2 tag=v3.0.0` → **`has_update=0 tag_mismatch=0`** |

注：上游 release tag 是 `v3.0.0`，包内 `interface.json` 报 `v5.16.0-beta.2` —— 两套口径。修复后以 **tag-vs-tag** 判定，第二次检查不再误报「有更新」。

### 2.2 MAA（新装 + 切默认路径）

| 阶段 | 输出 |
|---|---|
| 更新前检查 | `CURRENT version=v6.13.0` → `has_update=1` |
| 选中资产 | `MAA-v6.22.0-win-x64.zip` 294,969,951 B `sha256:197e3b4c…56c` |
| 安装 | `UPDATED id=core-a984a12bd96e404a version=v6.22.0 tag=v6.22.0 is_default=1`（耗时 24 s） |
| 更新后检查 | **`has_update=0 tag_mismatch=0`**，`.tmp` 已清理 |

## 三、本轮修复的缺陷

| # | 缺陷 | 根因 | 修复 |
|---|---|---|---|
| 1 | 资产误选 aarch64 | 过滤条件用 `find("x64")`，而 `x86_64` 不含子串 `x64` → 首轮无匹配，回退分支拿到 arm 包 | 显式枚举 `x86_64/x64/amd64` 且排除 `aarch64/arm64`；回退分支同样排除 arm |
| 2 | 永远提示有更新 | MaaEnd tag(`v3.0.0`) 与包内版本(`5.16.0-beta.2`) 不同源，直接比较必不等 | 记录 `release_tag`，优先 tag-vs-tag；无 tag 才回退探测值并置 `tag_mismatch` |
| 3 | 复用分支不写 tag | 同版本复用时直接 return，不落 `release_tag` → 下次仍误报 | 复用时回填 `release_tag` 并 `save()` |
| 4 | 下载永久挂死 | `http_download` 无超时（ENG01 连资产 CDN 时后台线程阻塞 10 min+） | 会话超时收到 10/20/30/30 s；`sha256_file` 打开失败重试 8×250 ms（杀软占用） |
| 5 | `哈希计算失败` 偶发 | 刚下载的大包被杀软/索引器短暂锁住，`CreateFileW` 直接失败 | 同上重试；另加 `MAAHUB_DEBUG_SHA=1` 诊断输出 |
| 6 | `.tmp` 残留 | 安装完成后不清理 zip + 解包副本（>1 GB） | 成功/复用两条路径都 `remove_tree(.tmp)` |
| 7 | 操作列溢出 | 8 列固定偏移超出面板宽度，默认窗口下按钮不可达 | 列宽按面板宽度自适应，右侧三个按钮组右对齐 |
| 8 | 更新面板文字重叠 | `tag` 行与「已是最新版本」行距重叠 | 面板 150 px 重排行距（值 / tag / 动作 / 警告 / 进度） |

## 四、ENG01（10.10.226.8）实测限制

| 项 | 结果 |
|---|---|
| GitHub API | `200`，1.34 s |
| 资产 CDN | 1.35 MB / 60 s ≈ **22 KB/s** → 315 MB 约 4 小时 |
| `core-check maaend` | 正常：`has_update=1 tag_mismatch=1`（该机核心无 `release_tag`） |
| `core-update maaend` | 能在进度中前进（16.7 MB），但等待不可接受，已中止并清理 `.tmp` |

要在这台机器上用自动更新，需要给 MaaHub 配代理（设置页「更新与代理」→ HTTP 代理），或改用 `--import-core` 从本机拷贝包。

## 五、界面核对（离屏渲染）

- `settings`（1100×1500）：新增「更新设置」组渲染正常 —— 4 个开关、渠道 `正式版/测试版/Nightly`、源 `自动/GitHub/Mirror酱`、`GitHub Token` 输入框、链路说明。
- `cores`（1100×760 + 假更新注入）：更新面板显示 当前/上游最新/来源/大小、`⬇ 下载并安装`、资产名 + 「sha256 校验」、`tag_mismatch` 警告行，无重叠。
- `cores`（1280×760）：表格 7 列 + `检查/设默认/删除` 全部在面板内，无截断。
- 隐藏未完成页面：现状保留（本次未改动导航）。

## 六、复现命令

```bat
cd /d <MaaHub>\build
MaaHub.exe --cli core-check maaend
MaaHub.exe --cli core-update maaend
MaaHub.exe --cli core-check maaend     :: 期望 has_update=0
MaaHub.exe --cli core-check maa
MaaHub.exe --cli core-update maa
```

离屏渲染：`MaaHub.exe --dump-render out.png cores [--dump-size 1280x760] [--dump-update-demo]`
