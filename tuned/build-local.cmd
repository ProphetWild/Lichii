@echo off
rem Configure (first run) and build the emulator with tuned\local-zen5-cache.cmake.
setlocal
set ROOT=%~dp0..
rem The bundled Qt is built with the VS 2026 (MSVC 14.5x) STL; older toolsets fail to link it.
set "VCVARS=D:\tools\VS2026BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul || exit /b 1
set PATH=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\VulkanSDK\1.4.363.0\Bin;%PATH%
if not exist "%ROOT%\build\zen5\build.ninja" (
    cmake -C "%ROOT%\tuned\local-zen5-cache.cmake" -S "%ROOT%" -B "%ROOT%\build\zen5" -G Ninja || exit /b 1
)
if "%1"=="configure" exit /b 0
cmake --build "%ROOT%\build\zen5" --target yuzu %* || exit /b 1
