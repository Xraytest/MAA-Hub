@echo off
REM MaaHub 沙盒构建（控制台子系统，但由 build_sandbox.ps1 以 CreateNoWindow + 重定向 stdio 启动，
REM                因此不会出现任何可见窗口）
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvars failed
    exit /b 1
)
cd /d "D:\WorkSpace\maa-hub-cpp\_sandbox"
if not exist out mkdir out

set COMMON=/nologo /EHsc /std:c++17 /W3 /utf-8 /ID:\WorkSpace\maa-hub-cpp\src /FoD:\WorkSpace\maa-hub-cpp\_sandbox\out\
set LIBS=gdi32.lib user32.lib shell32.lib winhttp.lib advapi32.lib

echo [BUILD] probe_buttons
cl %COMMON% /FeD:\WorkSpace\maa-hub-cpp\_sandbox\out\probe_buttons.exe probe_buttons.cpp /link %LIBS%
if errorlevel 1 exit /b 1

echo [BUILD] render_probe
cl %COMMON% /FeD:\WorkSpace\maa-hub-cpp\_sandbox\out\render_probe.exe render_probe.cpp /link %LIBS%
if errorlevel 1 exit /b 1

echo [BUILD] scroll_probe
cl %COMMON% /FeD:\WorkSpace\maa-hub-cpp\_sandbox\out\scroll_probe.exe scroll_probe.cpp /link %LIBS%
if errorlevel 1 exit /b 1

echo [OK] sandbox probes built
exit /b 0
