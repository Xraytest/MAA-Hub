# MaaHub 沙盒 —— 无窗口构建脚本
# 关键：/SUBSYSTEM:WINDOWS + mainCRTStartup，让探针成为"无控制台、无窗口"程序。
#   * GUI 子系统 => 不会分配控制台窗口
#   * 探针内没有任何 CreateWindow/ShowWindow 调用 => 不会产生可见窗口
#   * 结果写文件（*_report.txt），不依赖 stdout
# 用法：pwsh -File build_sandbox.ps1
$ErrorActionPreference = "Stop"
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$src  = Join-Path $here "..\src"
$out  = Join-Path $here "out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

# 注意：用 + 拼接，避免在双引号字符串里嵌套反引号转义出问题
$common = "/nologo /EHsc /std:c++17 /W3 /utf-8 /DNOMINMAX /I" + $src
$common += " /Fo" + $out + "\"
$common += " /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
$libs   = "gdi32.lib user32.lib shell32.lib winhttp.lib advapi32.lib"

$targets = @(
    @{ name = "probe_buttons"; src = "probe_buttons.cpp" },
    @{ name = "render_probe";   src = "render_probe.cpp" },
    @{ name = "scroll_probe";   src = "scroll_probe.cpp" }
)

# 每个 cl 单独一行写进临时 bat，彻底避开 PowerShell 引号解析
$bat = Join-Path $env:TEMP ("dsh_sandbox_build_" + [Guid]::NewGuid().ToString("N") + ".bat")
$lines = @()
$lines += '@echo off'
$lines += 'call "' + $vcvars + '" >nul 2>&1'
$lines += 'if errorlevel 1 exit /b 1'
$lines += 'cd /d "' + $here + '"'
foreach ($t in $targets) {
    $lines += 'cl ' + $common + ' /Fe"' + $out + '\' + $t.name + '.exe" ' + $t.src + ' /link ' + $libs
    $lines += 'if errorlevel 1 exit /b 1'
}
[System.IO.File]::WriteAllLines($bat, $lines, [System.Text.Encoding]::ASCII)
cmd /c "`"$bat`""
$rc = $LASTEXITCODE
Remove-Item $bat -Force -ErrorAction SilentlyContinue
if ($rc -ne 0) { throw "build failed (rc=$rc)" }
Write-Output "[OK] sandbox probes built (GUI subsystem, no console, no window)"
