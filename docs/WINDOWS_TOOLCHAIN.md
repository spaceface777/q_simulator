# Windows toolchain

`tools/windows_sdk/assemble.py` builds two portable toolchain ZIPs from pinned upstream releases:
one for Windows on Arm (arm64) and one for x64 Windows (x86_64). Both build 32-bit x86 (i686)
Windows programs with the Universal C Runtime (UCRT), the architecture of the simulator and its
images on Windows. Both have the same contents and layout. Only the host programs differ.

On Windows on Arm, the GCC toolchain from WinLibs is itself an i686 program, so every compiler run
is emulated. Every program in these ZIPs is native to its host: only the built programs run under
x86 emulation.

## Contents

| Component | Version | License | Source |
| --- | --- | --- | --- |
| LLVM-MinGW (Clang, LLD, LLVM tools, mingw-w64, libc++, libunwind, compiler-rt) | 20261104 (LLVM 23.1.5) | Apache-2.0 WITH LLVM-exception, ZPL-2.1, MIT and others: `LICENSE.TXT` | [releases](https://github.com/mstorsjo/llvm-mingw/releases/tag/20261104), UCRT `aarch64` and `x86_64` ZIPs |
| GNU Make | 4.4.1 | GPL-3.0-or-later: `share/make/COPYING.txt` | part of LLVM-MinGW, as `bin/mingw32-make.exe` |
| CMake | 4.4.5 | BSD-3-Clause: `doc/cmake/LICENSE.rst` | [releases](https://github.com/Kitware/CMake/releases/tag/v4.4.5), `windows-arm64` and `windows-x86_64` ZIPs |
| Ninja | 1.13.2 | Apache-2.0: `share/doc/ninja/COPYING` | [releases](https://github.com/ninja-build/ninja/releases/tag/v1.13.2), `ninja-winarm64` and `ninja-win` ZIPs |
| ccache | 4.14.2 | GPL-3.0-or-later: `share/doc/ccache/LICENSE.md` | [releases](https://github.com/ccache/ccache/releases/tag/v4.14.2), `windows-aarch64` and `windows-x86_64` ZIPs |

`tools/windows_sdk/components.json` records each URL with its SHA-256. The ZIP's `README.txt` and
`MANIFEST.json` list the same versions, licenses and checksums.

GNU Make comes from LLVM-MinGW, not from separate binaries. LLVM-MinGW 20261104 builds GNU Make
4.4.1 from source for each host and installs it as `bin/mingw32-make.exe`. A second GNU Make in
`bin/` would collide with it. The equation.com binaries are the same version, but equation.com
publishes no checksums and replaces its files under the same URL, so a pinned build would break.
The assembler copies `mingw32-make.exe` to `make.exe` as well.

LLVM-MinGW also contains `busybox/` and `python/` (for LLDB). They are not on `PATH`.
The toolchain contains no Python: the Zephyr build needs its own Python installation.

## Layout

All components share one root: `bin/`, `include/`, `lib/` and `share/`, plus one directory for
each target's libraries and runtime DLLs, such as `i686-w64-mingw32/`. CMake finds its modules in
`share/cmake-4.4` and Clang finds its resources in `lib/clang/23`, relative to their programs.
Nothing in the toolchain refers to its own location, so the folder can be moved or renamed.

- `env.cmd` and `env.ps1`: put `bin/` first on `PATH`; select `i686-w64-mingw32-clang` and
  `i686-w64-mingw32-clang++` as `CC` and `CXX`; run them through ccache in CMake builds
  (`CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER`); select the Ninja generator.
- `selftest/run.cmake`: checks an installation. See [Tests](#tests).
- `README.txt` and `MANIFEST.json`: versions, licenses and checksums.

The i686 runtime DLLs (`libc++.dll`, `libunwind.dll`, `libwinpthread-1.dll`) are in
`i686-w64-mingw32/bin`. A program that links them dynamically needs that directory on `PATH`,
or the DLLs next to it. `-static` links them into the program instead.

## Assemble

```sh
python3 tools/windows_sdk/assemble.py --cache ~/.cache/windows-toolchain --output dist
```

This writes `dist/quercus-windows-toolchain-<version>-arm64.zip` and `-x86_64.zip`, each with a
`.sha256` file. `--host arm64` or `--host x86_64` builds one ZIP. The script runs on Linux, macOS
and Windows with Python 3.10 or later and needs no other tools.

- Every download is checked against its pinned SHA-256. A download with another checksum stops the
  script. After a first run, `--offline` builds from the cache without network access, and
  `--download-only` only fills the cache.
- Mapping rules place each component's files in the root. A file that no rule maps stops the script,
  and so does a file that two components install with different contents. Paths are compared
  without regard to case, as Windows compares them.
- Every `.exe` and `.dll` must have the right machine type: the host's in `bin/`, the target's in a
  target directory. A host program may only import Windows system DLLs and DLLs in the toolchain.
- The ZIP is deterministic: the same inputs give the same file and the same checksum.

## Tests

`cmake -P selftest/run.cmake` checks an extracted toolchain. Only the toolchain's `bin/` and the
Windows system directories are on `PATH` during the test, so a missing file or an outside
dependency makes it fail. It needs no network access.

1. Every program in `bin/` is native to the host. CMake, Ninja, ccache, both GNU Make names, Clang
   and LLD start.
2. A C program compiles to an i686 executable that runs. A static C++ program with threads and
   exceptions links a C static library and runs. A C++ program runs with the shared libc++ and
   libunwind.
3. A C and C++ CMake project builds and runs with the Ninja generator, and with the MinGW Makefiles
   generator through `mingw32-make` and through `make`.
4. A second build in a new directory takes its compilations from ccache.

CI assembles both ZIPs in one job. Every Windows runner that uses the toolchain runs the self-test
from a folder whose name has a space, moves the folder, and runs it again before it builds the
simulator and the images. On macOS, the x86_64 ZIP passes the self-test under Wine.

## Compatibility with the GCC toolchain

Nothing built with the WinLibs GCC toolchain can be reused with this one. Rebuild every static
library, object file, image and the simulator. In particular:

- **C++ library**: GCC uses libstdc++ and Clang uses libc++. Their `std::` types have different
  layouts and mangled names. No C++ object or interface can cross between the two.
  `ENABLE_DEBUG_CONTAINERS` defines the checked mode of both libraries.
- **Compiler runtime**: GCC's code needs `libgcc` (`libgcc_s_dw2-1.dll`); Clang's needs
  compiler-rt, linked statically. LLVM-MinGW has no libgcc. Zephyr images select
  `CONFIG_COMPILER_RT_RTLIB`.
- **Unwinding**: both use DWARF unwind tables on i686, but GCC uses `libgcc_eh` and Clang uses
  libunwind (`libunwind.dll`).
- **Thread-local storage**: GCC emulates it (`__emutls_get_address`); Clang uses native Windows TLS.
- **C runtime libraries**: WinLibs has mingw-w64 14.0.0, LLVM-MinGW its own mingw-w64 build. Their
  `libmingwex.a`, `libmingw32.a` and `libwinpthread` differ.
- **Runtime DLLs**: a GCC build needs `libstdc++-6.dll`, `libgcc_s_dw2-1.dll` and
  `libwinpthread-1.dll` from WinLibs; a Clang build needs `libc++.dll`, `libunwind.dll` and
  `libwinpthread-1.dll` from `i686-w64-mingw32/bin`.

Images talk to the simulator through a C interface and both toolchains use UCRT, but a GCC image in
a Clang simulator, or the reverse, is not supported.

Windows images built with Clang link with LLD, which supports neither linker scripts nor
relocatable links. The pinned Zephyr revision replaces both with `scripts/build/coff_layout.py`.
It renames the embedded software's private symbols with the prefix `__nsi_esw.` instead of making
them local. Debug information, and so the crash report, keeps the original names.
