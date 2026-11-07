@echo off
rem Put this toolchain first on PATH and select its i686 compilers.
rem Every path comes from this file's location, so the folder may be moved.
for %%I in ("%~dp0.") do set "QUERCUS_TOOLCHAIN=%%~fI"
set "PATH=%QUERCUS_TOOLCHAIN%\bin;%PATH%"
set "CC=i686-w64-mingw32-clang"
set "CXX=i686-w64-mingw32-clang++"
set "CMAKE_C_COMPILER_LAUNCHER=ccache"
set "CMAKE_CXX_COMPILER_LAUNCHER=ccache"
set "CMAKE_GENERATOR=Ninja"
