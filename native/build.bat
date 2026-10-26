@echo off
rem Builds drongcast.exe with MSVC (Visual Studio 2026, x64).
setlocal
cd /d "%~dp0"
rem AMD AMF SDK headers (MIT) - fetched on first build, only amf/public/include
if not exist third_party\amf-sdk\amf\public\include\core\Factory.h (
    git clone --depth 1 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git third_party\amf-sdk || exit /b 1
    git -C third_party\amf-sdk sparse-checkout set amf/public/include || exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /DUNICODE /D_UNICODE ^
   /I third_party\amf-sdk\amf\public\include ^
   drongcast.cpp /Fe:drongcast.exe ^
   /link d3d11.lib dxgi.lib d3dcompiler.lib winusb.lib cfgmgr32.lib user32.lib winmm.lib ole32.lib
