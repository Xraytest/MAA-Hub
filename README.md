# MaaHub C++

原生 C++17 / Win32 GUI 编写的 MAA 与 MaaEnd 多开统一管理器，零第三方依赖。

## 功能

* **核心管理**：内置多源更新与下载，所有核心统一存放于 `cores/` 目录。支持多实例共享同一份核心文件，避免重复占用空间。


* **多实例并发**：每个执行器运行在独立的 worker 子进程中，互不干扰，支持多账号同时挂机。


* **统一交互**：复刻 MAA 原版界面，支持任务拖拽排序、右键单次生效及专属配置面板，MaaEnd 实例同样适配此界面。


* **账号与排程**：内置多账号切换与日常追踪；支持接入 OpenAI 兼容接口（默认 DeepSeek）生成调度计划，无 API Key 时自动回退本地规则。



## 构建与使用

需要 MSVC (VS2022 及以上) 环境：

```powershell
build.bat release

```

常用启动参数：

```powershell
build\MaaHub.exe                                     # 启动界面
build\MaaHub.exe --import-core maa "D:\Path\To\MAA"  # 导入本地核心
build\MaaHub.exe --check                             # 自检核心与执行器

```

## 免责声明

本工具仅通过公开 C API 调用 [MAA](https://github.com/MaaAssistantArknights/MaaAssistantArknights) 与 [MaaEnd](https://github.com/MaaEnd/MaaEnd)，不修改或分发其文件。使用请遵守游戏服务条款，风险自负。