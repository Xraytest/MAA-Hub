@echo off
REM MaaHub C++ build script (MSVC). Usage: build.bat [debug|release]
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=release

set ROOT=%~dp0
if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%

set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist %VCVARS% (
    echo [ERROR] vcvars64.bat not found: %VCVARS%
    exit /b 1
)

call %VCVARS% >nul
if errorlevel 1 (
    echo [ERROR] vcvars64 failed
    exit /b 1
)

REM vcvars may change cwd; go back to project root
cd /d "%ROOT%"

if not exist build mkdir build

set SRCS=%ROOT%\src\main.cpp %ROOT%\src\worker_main.cpp %ROOT%\src\app_window.cpp

REM Native Win32 GUI, zero third-party deps
set CLFLAGS=/nologo /EHsc /std:c++17 /W3 /utf-8 /Fo:%ROOT%\build\ /Fe:%ROOT%\build\MaaHub.exe
set LINKFLAGS=gdi32.lib user32.lib shell32.lib winhttp.lib advapi32.lib msimg32.lib

if /i "%CONFIG%"=="debug" (
    set CLFLAGS=%CLFLAGS% /Zi /Od
    set LINKFLAGS=%LINKFLAGS% /DEBUG
) else (
    set CLFLAGS=%CLFLAGS% /O2 /DNDEBUG
)

echo [BUILD] config=%CONFIG%
cl %CLFLAGS% %SRCS% /link %LINKFLAGS% /OUT:%ROOT%\build\MaaHub.exe
if errorlevel 1 (
    echo [ERROR] build failed
    exit /b 1
)
echo [OK] %ROOT%\build\MaaHub.exe
exit /b 0
