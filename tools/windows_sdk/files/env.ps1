# Put this toolchain first on PATH and select its i686 compilers.
# Every path comes from this file's location, so the folder may be moved.
$env:QUERCUS_TOOLCHAIN = $PSScriptRoot
$env:Path = "$PSScriptRoot\bin;$env:Path"
$env:CC = "i686-w64-mingw32-clang"
$env:CXX = "i686-w64-mingw32-clang++"
$env:CMAKE_C_COMPILER_LAUNCHER = "ccache"
$env:CMAKE_CXX_COMPILER_LAUNCHER = "ccache"
$env:CMAKE_GENERATOR = "Ninja"
