@echo off
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul || (echo Install CMake 3.22+ and tick "Add CMake to the PATH". & exit /b 1)
where git >nul 2>nul || (echo Install Git for Windows. & exit /b 1)
if not exist build\max-sdk-base\.git git clone https://github.com/Cycling74/max-sdk-base.git build\max-sdk-base || exit /b 1
git -C build\max-sdk-base checkout --detach c03a2922a2a8ff149165a1e2ea134e9321d6e202 || exit /b 1
cmake -S . -B build\windows -G "Visual Studio 17 2022" -A x64 -T ClangCL -DMAX_SDK_BASE="%CD%\build\max-sdk-base" || exit /b 1
cmake --build build\windows --config Release --target lemus808_tilde --parallel || exit /b 1
echo.
echo External built in build\windows\externals (ticco808cal~.mxe64)
dir /s /b build\windows\externals\*.mxe64
