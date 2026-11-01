# MaaHub 沙盒 —— 无窗口执行器
# 保证不出现任何可见窗口：
#   * UseShellExecute = false + RedirectStandardOutput/Error
#     => 子进程拿到管道句柄，Windows 不会为它分配/显示控制台
#   * CreateNoWindow = true + WindowStyle Hidden
#     => 双重保险
# 用法：pwsh -File run_sandbox.ps1 <probe.exe 名> [参数...]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string[]]$Args = @()
)

$ErrorActionPreference = "Stop"
$dir = "D:\WorkSpace\maa-hub-cpp\_sandbox\out"
$exePath = Join-Path $dir $Exe
if (-not (Test-Path $exePath)) { throw "not found: $exePath" }

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName               = $exePath
$psi.WorkingDirectory       = $dir
$psi.UseShellExecute        = $false          # 关键：不用 shell，才能重定向
$psi.CreateNoWindow         = $true           # 关键：不建窗口
$psi.WindowStyle            = [System.Diagnostics.ProcessWindowStyle]::Hidden
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
foreach ($a in $Args) { [void]$psi.ArgumentList.Add($a) }

$p = [System.Diagnostics.Process]::Start($psi)
$stdout = $p.StandardOutput.ReadToEndAsync()
$stderr = $p.StandardError.ReadToEndAsync()
$p.WaitForExit()
[void]$p.StandardOutput.ReadToEnd()
[void]$p.StandardError.ReadToEnd()

Write-Output "=== exit code: $($p.ExitCode) ==="
if ($stdout.Result) { Write-Output "--- stdout ---"; Write-Output $stdout.Result }
if ($stderr.Result) { Write-Output "--- stderr ---"; Write-Output $stderr.Result }
if (-not $stdout.Result -and -not $stderr.Result) { Write-Output "(no output)" }
