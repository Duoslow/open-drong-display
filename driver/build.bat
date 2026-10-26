@echo off
rem Builds DrongIdd.dll (UMDF 2.25 / IddCx 1.4 display driver) with MSVC + the Windows Driver Kit.
setlocal
cd /d "%~dp0"
set "AMF=..\native\third_party\amf-sdk\amf\public\include"
if not exist "%AMF%\core\Factory.h" (
    git clone --depth 1 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git ..\native\third_party\amf-sdk || exit /b 1
    git -C ..\native\third_party\amf-sdk sparse-checkout set amf/public/include || exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" 10.0.26100.0 >nul || exit /b 1
cd /d "%~dp0"
set "KIT=%ProgramFiles(x86)%\Windows Kits\10"
set "SDKVER=10.0.26100.0"
if not exist out mkdir out
cl /nologo /std:c++17 /EHsc /O2 /W3 /MD /LD /Zi ^
   /DUNICODE /D_UNICODE /D_WINDLL /DUMDF_VERSION_MAJOR=2 /DUMDF_VERSION_MINOR=25 ^
   /DIDDCX_VERSION_MAJOR=1 /DIDDCX_VERSION_MINOR=4 /DIDDCX_MINIMUM_VERSION_REQUIRED=4 ^
   /I "%KIT%\Include\wdf\umdf\2.25" /I "%KIT%\Include\%SDKVER%\um\iddcx\1.4" /I "%AMF%" ^
   /Fo:out\ /Fd:out\ DrongIdd.cpp /Fe:out\DrongIdd.dll ^
   /link /DEBUG /OPT:REF /OPT:ICF ^
   "%KIT%\Lib\wdf\umdf\x64\2.25\WdfDriverStubUm.lib" "%KIT%\Lib\%SDKVER%\um\x64\iddcx\1.4\iddcxstub.lib" ^
   d3d11.lib dxgi.lib avrt.lib ole32.lib ntdll.lib || exit /b 1
