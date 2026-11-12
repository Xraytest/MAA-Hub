@echo off
setlocal
set ROOT=%~dp0..
set VCVARS="C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call %VCVARS% >nul
cd /d "%ROOT%"
cl /nologo /EHsc /std:c++17 /W3 /utf-8 /Fo:%ROOT%\build\ /Fe:%ROOT%\build\parse_url_diff.exe %ROOT%\tests\parse_url_diff.cpp /link gdi32.lib user32.lib shell32.lib winhttp.lib advapi32.lib msimg32.lib
exit /b %ERRORLEVEL%