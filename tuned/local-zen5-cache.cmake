# Local build for a Ryzen 7 9800X3D (Zen 5) + RTX 5090 with LLVM 23 in its default install path.
# No PGO: tuned/eglantine.profdata was trained on Zen 3 and an older tree.
#   cmake -C tuned/local-zen5-cache.cmake -S . -B build/zen5 -G Ninja
#   cmake --build build/zen5 --target yuzu
set(LLVM_ROOT "C:/Program Files/LLVM")
set(ZEN5_FLAGS "-march=znver5 -mtune=znver5 /clang:-Wno-backend-plugin")
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
set(CMAKE_C_COMPILER "${LLVM_ROOT}/bin/clang-cl.exe" CACHE FILEPATH "")
set(CMAKE_CXX_COMPILER "${LLVM_ROOT}/bin/clang-cl.exe" CACHE FILEPATH "")
set(CMAKE_LINKER "${LLVM_ROOT}/bin/lld-link.exe" CACHE FILEPATH "")
set(CMAKE_AR "${LLVM_ROOT}/bin/llvm-lib.exe" CACHE FILEPATH "")
set(CMAKE_C_FLAGS "/DWIN32 /D_WINDOWS ${ZEN5_FLAGS}" CACHE STRING "")
set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS /EHsc ${ZEN5_FLAGS}" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "/machine:x64" CACHE STRING "")
# ThinLTO costs a lot of link time; turn on for a release build.
set(ENABLE_LTO OFF CACHE BOOL "")
set(BUILD_TESTING OFF CACHE BOOL "")
set(YUZU_TESTS OFF CACHE BOOL "")
set(YUZU_CMD OFF CACHE BOOL "")
set(YUZU_ROOM OFF CACHE BOOL "")
set(YUZU_ROOM_STANDALONE OFF CACHE BOOL "")
set(YUZU_USE_BUNDLED_SDL3 ON CACHE BOOL "")
set(ENABLE_UPDATE_CHECKER OFF CACHE BOOL "")
set(USE_DISCORD_PRESENCE OFF CACHE BOOL "")
set(GLSLANGVALIDATOR "C:/VulkanSDK/1.4.363.0/Bin/glslangValidator.exe" CACHE FILEPATH "")
