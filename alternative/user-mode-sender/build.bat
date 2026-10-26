@echo off
rem Builds drongcast.exe (user-mode sender) with MSVC x64.
setlocal
cd /d "%~dp0"
call ..\..\scripts\fetch-amf.cmd || exit /b 1
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /DUNICODE /D_UNICODE ^
   /I ..\..\third_party\amf-sdk\amf\public\include ^
   /Fo:out\ drongcast.cpp /Fe:out\drongcast.exe ^
   /link d3d11.lib dxgi.lib d3dcompiler.lib winusb.lib cfgmgr32.lib user32.lib winmm.lib ole32.lib
