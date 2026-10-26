@echo off
rem Fetches the AMD AMF SDK public headers (MIT) into third_party\amf-sdk, once.
setlocal
set "ROOT=%~dp0.."
if exist "%ROOT%\third_party\amf-sdk\amf\public\include\core\Factory.h" exit /b 0
git clone --depth 1 --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git "%ROOT%\third_party\amf-sdk" || exit /b 1
git -C "%ROOT%\third_party\amf-sdk" sparse-checkout set amf/public/include || exit /b 1
