# Like local-zen5-cache.cmake, with CodeView debug info and a PDB for the sampling profiler
# (EDEN_NX2_SAMPLE_PROFILE + tools/nx2/sample_report.py --exe lichii.exe). Same optimization.
#   cmake -C tuned/local-zen5-prof-cache.cmake -S . -B build/zen5-prof -G Ninja
include("${CMAKE_CURRENT_LIST_DIR}/local-zen5-cache.cmake")
set(CMAKE_C_FLAGS "/DWIN32 /D_WINDOWS ${ZEN5_FLAGS} /Z7" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "/DWIN32 /D_WINDOWS /EHsc ${ZEN5_FLAGS} /Z7" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "/machine:x64 /DEBUG:FULL /OPT:REF /OPT:ICF" CACHE STRING "" FORCE)
