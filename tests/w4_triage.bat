@echo off
REM One-shot /W4 triage compile (diagnostics only; product build stays /W3 in build.bat)
setlocal
set ROOT=%~dp0..
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul
cd /d "%ROOT%"
if not exist build\w4obj mkdir build\w4obj
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /showIncludes /Od /DNDEBUG /c /Fobuild\w4obj\ src\main.cpp src\worker_main.cpp src\app_window.cpp > D:\WorkSpace\_tmp\maahub-day1\w4-triage.log 2>&1
echo EXIT=%ERRORLEVEL%
