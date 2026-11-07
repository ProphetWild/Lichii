@echo off
rem Build the offline shader sweep tool (tools/shader-sweep) in build\zen5-prof, which this turns
rem ENABLE_DEBUG_TOOLS on for. See tools/nx2/shader-sweep.md.
setlocal
set ROOT=%~dp0..
set "VCVARS=D:\tools\VS2026BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || exit /b 1
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\VulkanSDK\1.4.363.0\Bin;%PATH%
if not exist "%ROOT%\build\zen5-prof\build.ninja" (
    cmake -C "%ROOT%\tuned\local-zen5-prof-cache.cmake" -S "%ROOT%" -B "%ROOT%\build\zen5-prof" -G Ninja || exit /b 1
)
cmake -S "%ROOT%" -B "%ROOT%\build\zen5-prof" -DENABLE_DEBUG_TOOLS=ON >nul || exit /b 1
cmake --build "%ROOT%\build\zen5-prof" --target shader-sweep %* || exit /b 1
