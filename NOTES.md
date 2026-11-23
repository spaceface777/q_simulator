# Toolchain port log

Discoveries, tests, hypotheses, outcomes and planned experiments for the move from the GCC-based
cosmocc bundle (release 2026.11.18-64) to the clang/lld one (2026.11.21-122 and later). Newest
entries at the end of each section.

## Starting point (2026-11-22)

- Simulator branch `experiment/linux-llvm` (bffecdd), q_sdk `llvm-toolchain` (ecacaad). Work goes
  on `cosmocc-clang` in both.
- Bundle 122 (`~/.local/share/cosmocc`): one multicall `bin/llvm` (clang 22.1.8, lld, llvm
  binutils, llvm-profdata). Tool names: `<arch>-unknown-cosmo-{cc,c++,ar,ranlib,objcopy,ld,nm,...}`
  are all links to it. Gone: every `<arch>-linux-cosmo-*` name (gcc, g++, gcc-ar, objcopy, ld.bfd),
  gcov, libgcov. Present: `ld.lld`, `llvm-*`, compiler-rt's profile runtime for both cosmo targets.
- clang's Cosmopolitan toolchain (cosmocc-build `patches/llvm/0007`) applies the ABI from the
  triple `<arch>-unknown-linux-cosmo` (x18/x28 reserved, TLS helpers, no red zone, -fportcosmo),
  also inside LTO code generation. So nothing outside the compiler needs the ABI flags any more.
- The driver links with lld and runs fixupobj; for `.com`/`.exe` outputs it makes an APE, on
  both architectures (to verify for aarch64: the old GCC driver left ELF there, hence apelink calls
  in CMakeLists.txt and assemble.py).
- The bundle's Python is `sys.platform == "cosmo"`, starts APE files directly, has no ctypes.
- cosmocc-build has two "consumer" proposals (`patches/consumers/*.patch`); this tree already
  has the qsim.py half (no `/bin/sh`), the rest is in test/ and tools/.
- quercus-train-suite (`../../cosmopolitan-upgrade/quercus-train-suite`, branch weighted-parts)
  carries a working clang port of this tree, the SDK and the Zephyr patch (commits adfedce,
  fbbedcc, afcabaf), used to PGO-train the bundle's LLVM. Its 46 tests passed with the seed
  toolchain. That port is the starting point here; it still has leftovers (GCC branches next to
  clang ones, a linker-script rewrite in assemble.py, an explicit apelink step).

## Test hosts

- MacBook Air M1 (macOS 27, arm64): cosmo builds; Docker (arm64 Linux containers).
- MiniPC (N100, Ubuntu, x86_64): cosmo and native builds.
- GitHub Actions mirror (spaceface777/q_simulator) for Windows and the full matrix.

## Baseline with 122 (2026-11-22)

- The train-suite port, applied as is, built and passed 47/47 on macOS arm64 after one fix: the
  122 driver includes `libc/integral/normalize.inc` in `-E` too, so preprocessing the runner's
  linker script as C (`-x c -E -P`) emitted typedefs and lld failed ("unknown directive:
  typedef"). `-nostdinc` keeps it out (`-nostdlibinc` too; `-ffreestanding` and `-undef` don't).
  TOOLCHAIN: arguably `-E` of a non-C input shouldn't get it; any project that preprocesses
  linker scripts with the cosmo driver hits this.
- Local-only: macOS `/tmp` is a symlink to `/private/tmp`. With ZEPHYR_BASE under `/tmp`, Zephyr
  computes `..`-relative library names and CMake rejects them. Use `/private/tmp`.

## What 122 does and doesn't do (probes in /tmp/qsx)

- `x86_64-unknown-cosmo-c++ -o x.exe` makes an APE; `aarch64-unknown-cosmo-c++ -o x.exe` makes a
  stripped ELF (+ `x.exe.dbg`), as the GCC driver did. The project still runs apelink itself for
  aarch64 (CMakeLists.txt's link rule, qsim.py). TOOLCHAIN: if the aarch64 driver ran apelink
  for `.exe`/`.com` like the x86-64 one, both calls go away.
- UBSan works on cosmo: cosmo libc has its own `__ubsan_handle_*` runtime, which stops with
  status 99. ASan: no runtime (`__asan_init` undefined). -> ENABLE_SANITIZERS now means UBSan on
  cosmo builds, ASan+UBSan on host builds.
- Coverage: `--coverage` (gcov) is broken in 122: the driver doesn't link
  `libclang_rt.profile.a` (it has the `llvm_gcda_*` functions), and with it linked by hand no
  `.gcda` appears. Source-based coverage (`-fprofile-instr-generate -fcoverage-mapping`) works and
  writes `.profraw`. There is no `llvm-cov` in the bundle to report it, and no gcov any more.
  TOOLCHAIN: add `llvm-cov` to the multicall (LLVM_TOOL_LLVM_COV_BUILD). -> ENABLE_COVERAGE is
  source-based on both builds now.
- The bundle's CMake reports `CMAKE_HOST_UNIX` true and `CMAKE_HOST_APPLE` false on macOS (it's
  a Cosmopolitan program), so Zephyr's "POSIX arch only on Linux" check passes on every host.
  It reports the Apple silicon CPU as `arm64`, which Zephyr's posix arch maps to 32-bit arm:
  qbuild passes `NATIVE_TARGET_HOST=aarch64` there. TOOLCHAIN: report `aarch64` (as Linux does)
  and that line goes.
- CMake finds llvm-ar, llvm-ranlib and llvm-objcopy by itself from the compiler's name: the
  toolchain file only names the two compilers (and the bundle's Python, which macOS's
  FindPython would otherwise pass over for a framework Python).

## Decisions (user, 2026-11-22)

- clang + lld everywhere, host builds too. No GCC paths anywhere.
- Only bundle 122 matters: no version checks, no compatibility with older bundles.
- Remove code. Where something works only with significant new code, flag it instead.
- CI installs the bundle with its own install.sh / install.ps1.
- Out-of-process firmware (`out/<app>.exe`, `lockstep_transport.c`) deleted: the simulator only
  runs firmware in-process. That took assemble.py's lld linker-script rewrite (`INSERT AFTER
  .data` -> `.rodata`, for the APE loader's segment overlap check) and its apelink step with it.
- Endgame: no Zephyr patch, the bundle behaves like a host clang.

## Unpatched Zephyr (2026-11-22)

- Stock Zephyr v4.5.0 with its own `host/llvm` variant, `CONFIG_LLVM_USE_LLD=y`, and the
  bundle's clang first on PATH builds the applets; assemble.py links them; 47/47 pass on macOS
  arm64. The bundle's clang defaults to `<arch>-unknown-linux-cosmo`, which gives Zephyr's code
  the cosmo ABI and headers with no toolchain definition at all.
- So the whole q_sdk Zephyr patch (517 lines) is gone. Hunk by hunk:
  - `cmake/toolchain/cosmocc/*`, FindHostTools, twister: not needed (host/llvm).
  - arch/posix "Linux only": not needed (bundle CMake, above).
  - nsi_errno.c non-const map: not needed; clang's -fportcosmo copes.
  - GCC LTO (`-flinker-output=nolto-rel`, `externally_visible`), lld `partial_linking`: gone with GCC.
  - check_init_priorities `.dbg` suffix, the NSI Makefile's drive-letter/space rules: only for
    `zephyr.exe`, which nothing builds (`CONFIG_BUILD_OUTPUT_EXE=n`).
  - clang `compiler_set_linker_properties` for native libraries: not needed with host/llvm.
  - `natsim_linker_script.ld` INPUT_SECTION_FLAGS: not needed with lld (to watch).
  - timer_model CLOCK_MONOTONIC_RAW fallback: real-time pacing is off in the SDK config; to
    check on Intel macOS in CI.
  - nsi_compile_options/nsi_sources one per line: assemble.py falls back to the make variables,
    which split at blanks; fine without spaces.
- What unpatched Zephyr costs, both upstream Zephyr limits, both good upstream contributions:
  1. Paths with spaces: Zephyr's own command lines don't quote `-imacros autoconf.h`, `-Map`,
     `-L`, `undef_system_defines.h`, `-T`. A Zephyr or application path with a space fails.
  2. LTO of a native library: Kconfig `LTO depends on !NATIVE_LIBRARY`. GCC's `ld -r` keeps
     bitcode, lld's generates code, so LTO could be allowed with lld. Until then: no firmware
     LTO (QUERCUS_LTO, `--no-lto`, no_lto.conf removed). The runner had LTO only with it too.
     OPTIMIZATION.md measured whole-world LTO at about 3% on the router suite (sim + firmware);
     the simulator keeps its own LTO. To measure: firmware-only share.

## Linux (Docker arm64, 2026-11-22)

- Native builds need LLVM >= 21. lld 18, 19 and 20 (Ubuntu 24.04's, and apt.llvm.org's) crash
  with SIGSEGV in `LinkerScript::addOrphanSections()` on `ld.lld -r zephyr.elf`, i.e. relinking
  Zephyr's own relocatable output (lld's too), with or without a script, debug info or
  `.llvm_addrsig`. lld 21.1.8 (Ubuntu 26.04) and the bundle's 22.1.8 are fine. Not bisected
  further: the fix is in a released LLVM. Ubuntu 22.04/24.04 take LLVM 21 from apt.llvm.org.
- Those images' libstdc++ is too old for the C++23 that the simulator uses (CI used GCC 14 from
  the toolchain PPA for it); apt.llvm.org's libc++ avoids GCC there: CXXFLAGS=-stdlib=libc++.
  That showed that qsim.py's world links didn't get CMAKE_CXX_FLAGS / CMAKE_EXE_LINKER_FLAGS:
  they do now, as CMake's own links do.
- clang caught a real bug: `abs(float)` in visualization.cpp resolved to the integer abs with
  glibc's headers (-Wabsolute-value). Now `std::abs`.
- ASan's LeakSanitizer flagged libbacktrace (DWARF 5 file name strings that no line entry
  keeps). The build had chosen libbacktrace's malloc allocator (alloc.c, read.c); upstream's
  POSIX default (mmap.c, mmapio.c) has no such report and is the plainer config.
- Ubuntu packages clang's sanitizer runtimes apart: `libclang-rt-dev`.
- Stock nsi_config writes `NSI_CC := <launcher> <compiler>` unquoted, so a bundle in a directory
  with a space (`/opt/cosmo cc`, and `%LOCALAPPDATA%` under a user name with one) split.
  assemble.py takes CMAKE_C_COMPILER and CMAKE_OBJCOPY from the application's CMakeCache.txt.
- Results, unpatched Zephyr v4.5.0: 26.04 clang 21 native 47/47; 26.04 ASan+UBSan 47/47;
  24.04 apt.llvm.org 21 + libc++ 47/47; 24.04 cosmo (only the bundle, installed with
  install.sh --archive into `/opt/cosmo cc`, PATH replaced) 47/47.

## x86_64 and the other configurations (2026-11-22)

- MiniPC (N100, Ubuntu 26.04 host; no Docker without sudo): cosmo with only the bundle
  (install.sh --archive into `~/qs-port/w/cosmo cc`, PATH replaced) 47/47; native clang 21
  47/47. The host has clang 21 but no lld: `apt download lld-21 liblld-21` + `dpkg -x` into the
  scratch directory, no root.
- macOS arm64, cosmo, -DENABLE_SANITIZERS=ON (UBSan through cosmo libc's runtime): 47/47.

## CI (2026-11-22)

- The cosmocc jobs install with the release's own installers, piped from the GitHub mirror:
  `curl $COSMOCC_URL/$COSMOCC_VERSION/install.sh | sh -s -- --version ... --dir ...`, and
  `irm .../install.ps1 | iex` with COSMOCC_URL/VERSION/DIR in the environment. The installers
  fetch `$COSMOCC_URL/<version>/cosmocc.tar.xz` (and `bsdtar` where the system's tar can't unpack
  xz 5.4 archives), so the mirror's release tag must be the bare version (`2026.11.21-122`), and
  it holds cosmocc.tar.xz, bsdtar, install.sh and install.ps1. Earlier mirrors were tagged
  `cosmocc-<version>`. No checksum step: the installers don't take one.
- No Zephyr patch: clone upstream v4.5.0 with the bundle's git and `west init -l`. The
  actions/checkout fallback for a failed clone is gone. Zephyr and the source checkout are in
  directories without spaces (Zephyr's limit); the bundle and the build directory keep theirs.
- `runner.temp` isn't available in job-level `env`: a first pwsh step writes COSMOCC_DIR, BUILD
  and ZEPHYR_BASE with Join-Path, so each OS gets its own separators.
- format-check runs in the cosmocc bash jobs with the bundle's clang-format 22 (the native jobs
  no longer pip-install clang-format).
- native: 26.04 takes the distro's clang 21, lld, libc++ and compiler-rt; 22.04 and 24.04 take
  LLVM 21 from apt.llvm.org (llvm.sh) for lld. CXXFLAGS=-stdlib=libc++ everywhere native, so no
  GCC 14 PPA. Zephyr v4.5.0 for native too (was v4.4.1): one version.
- .gitlab-ci.yml: CC=clang CXX=clang++. NOT VERIFIED: the course image (q_vm_builder/ci) must
  have clang and lld 21 or later.

## Coverage and PGO on cosmo (macOS arm64, 2026-11-22)

- ENABLE_COVERAGE (source-based): 47/47 pass; 70 profiles in `build/coverage/` (tests.exe and
  every world, `%m-%p` names). Reported with the bundle's llvm-profdata (Homebrew LLVM 23's
  can't read 22's raw profile format 10, but reads the indexed merge) and Homebrew's llvm-cov:
  80.7% of regions in `src/` over the unit, world and applet tests.
- PGO, exactly as docs/OPTIMIZATION.md now says: train, run the route scenario, copy the world's
  profile to `build_sim/conveyor/pgo/`, USE on both sides (both compile with `-fprofile-use`),
  the optimized world passes the scenario. The profile has firmware functions (main,
  z_impl_k_sleep_ticks, nsi_hws_*) without `CONFIG_STATIC_INIT_GNU` (pgo.conf, which GCC needed
  for its counter constructors): removed. Firmware TRAIN creates `pgo/` so that the copy works.
- Not run locally: world.gui with SDL3 assets (CI downloads them), Windows, Intel macOS.

## Open items

TOOLCHAIN (cosmocc-build), each removes code here:
- aarch64 driver: make `.exe`/`.com` outputs APEs (apelink -l ape-aarch64.elf -M ape-m1.c) as
  the x86-64 driver does. Removes CMakeLists.txt's extra link command and qsim.py's apelink call.
- Report `aarch64`, not `arm64`, as the machine on Apple silicon (CMake's host processor, and
  uname). Removes qbuild's NATIVE_TARGET_HOST line.
- `--coverage`: link libclang_rt.profile.a, and find out why no .gcda appears; or decide that
  source-based coverage is the only kind. Ship `llvm-cov` in the multicall for reports.
- `-E` of a linker script gets normalize.inc (assemble.py's `-nostdinc`).
- `-pthread` is "unused during compilation" at the cosmo driver's links (warning only).

UPSTREAM ZEPHYR (each a small, self-contained PR):
- Quote paths in command lines (imacros autoconf.h, zephyr_stdint.h, undef_system_defines.h,
  -Map, -L, natsim's -T, nsi_config's NSI_CC/NSI_AR/NSI_OBJCOPY). Then CI can put Zephyr and the
  checkout back in directories with spaces.
- Allow CONFIG_LTO with NATIVE_LIBRARY when the linker generates code in a relocatable link
  (lld). Then firmware LTO comes back as one Kconfig line.

PLANNED / NOT DONE:
- Publish the GitHub mirror release `2026.11.21-122` (cosmocc.tar.xz, bsdtar, install.sh,
  install.ps1) that ci.yml's installers read. Needs the user's OK. Then run the whole matrix.
- Measure what firmware LTO was worth on the router suite, now that it's gone.
- pico2 with upstream Zephyr's `llvm` variant and the bundle (the train suite did it with a
  Zephyr SDK stand-in and the hal_rpi_pico ACLE patch): not part of this repo's CI.
- Bisect lld <= 20's crash only if someone needs LLVM 18-20 hosts.
- coro.c's `CORO_X18`/`CORO_X28`: clang warns about reserved registers in a clobber list at
  every LTO link under cosmo; a host compiler must be told. Kept (from the train suite).

## Toolchain gaps fixed in cosmocc-build (2026-11-22, round 2)

Fixed in cosmocc-build (uncommitted there; its NOTES.md "What the simulator's port to 122
needed"), so that this tree drops its exceptions:
- 0007 (clang's Cosmo driver): arm64 `.exe`/`.com` outputs are APEs (apelink, as the fat
  cosmocc does) -> CMakeLists.txt's extra apelink link command and qsim.py's apelink step deleted.
- 0007: `--coverage` links the profile runtime; `-pthread` claimed at links.
- 0016: llvm-cov in the multicall, `bin/llvm-cov` -> README's coverage report uses the bundle's.
- cosmopolitan 0047: uname() says `aarch64` on Apple silicon -> qbuild's NATIVE_TARGET_HOST
  line deleted.
- Kept, with reason: assemble.py's `-nostdinc` when it preprocesses the runner's linker script.
  `-E` must keep normalize.inc for C; a linker script must not see the C library's headers.
- Needs a release built from those changes; the CI's COSMOCC_VERSION follows it.

## Build and link timings (2026-11-22)

Cosmo, clean, ccache off, dependencies pre-downloaded (/private/tmp/qs-scripts/bench.sh). Mac:
M1, 8 jobs, a scratch copy with the aarch64 apelink steps restored (122 lacks the driver fix).
MiniPC: N100, 4 jobs, the tree as is (x86-64).

| step | M1 | N100 |
|---|---:|---:|
| configure | 1.8 s | 0.8 s |
| qsim.o from clean, ThinLTO | 24.4 s | 33.0 s |
| ...of which the qsim-core.o prelink | 5.6 s | 7.8 s |
| qsim.o from clean, no LTO | 21.8 s | 32.2 s |
| tests.exe on top | 25.3 s | 36.4 s |
| 4 test applets, clean (qbuild sim) | 63.2 s | 49.9 s |
| one applet after a one-file edit | 1.5 s | (12.6 s before the fix below) |
| world link, 1 / 5 / 10 / 20 / 24 modules | 0.36 / 0.38 / 0.44 / 0.51 / 0.58 s | 0.24 / 0.23 / 0.27 / 0.29 / 0.32 s |
| world size, 1 / 24 modules | 2.0 / 4.9 MiB | 1.9 / 4.4 MiB |
| PGO TRAIN qsim.o, clean | 20.3 s | 34.3 s |
| training run (all 16 applet scenarios) | 33.5 s | 30.8 s |
| PGO USE qsim.o rebuild | 14.0 s | 32.5 s |

- World links are medians of 5. Per module about 10 ms (M1, which also runs apelink) and 3.5 ms
  (N100); the rest is Python, the objcopy calls and the driver.
- The training run is bound by the scenarios' own wall-clock limits (watchdog and timeout
  scenarios), not by CPU.
- Two-stage clean build (applets, TRAIN, training, USE) against today's (applets, qsim.o):
  ~131 s vs ~88 s on the M1, ~147 s vs ~83 s on the N100.
- REGRESSION FOUND AND FIXED: qbuild compared the cached ZEPHYR_TOOLCHAIN_VARIANT with
  "host/llvm", but Zephyr caches "host" (it splits off the compiler). Every incremental qbuild
  reconfigured Zephyr, about 10 s (12.5-15.7 s for a one-file edit); now 1.5-1.7 s.

## Module count scaling (2026-11-22)

- The module limit is only validation: MAX_MODULES = 25 (src/defs.h; routing IDs 1-24, tower 255)
  in settings, events, the LTL checker, module types and qsim.py. Every per-module table is
  already 256 entries (MODULE_TABLE_SIZE, which the in-process instances use too, the
  runtime's tick array) and IDs are uint8_t, so 254 routing modules
  fit without layout changes. Second limit: `pos` must be 0..10 on both axes (settings.cpp),
  121 cells.
- Measured in a scratch copy with MAX_MODULES = 255 and pos <= 15 (/private/tmp/qs-scripts/
  scale.sh), M1 (busy with a toolchain build), cosmo, idle applet with heartbeats, no
  neighbors, 5 s of simulated time:

| modules | world link | world size | run wall | max RSS |
|---:|---:|---:|---:|---:|
| 1 | 0.34 s | 2.0 MiB | 0.19 s | 4 MiB |
| 24 | 0.55 s | 4.9 MiB | 0.22 s | 9 MiB |
| 50 | 0.74 s | 8.2 MiB | 0.29 s | 15 MiB |
| 100 | 1.27 s | 14.4 MiB | 0.44 s | 27 MiB |
| 150 | 1.67 s | 20.7 MiB | 0.58 s | 38 MiB |
| 200 | 2.43 s | 26.9 MiB | 0.68 s | 50 MiB |
| 254 | 3.23 s | 33.7 MiB | 0.97 s | 62 MiB |

- Linear: about 11 ms of link, 128 KiB of world and 0.24 MiB of memory per module (one copy of
  the firmware image each). Not measured: real routing workloads (neighbors, tubs), the GUI
  with that many modules on screen.
- Done in the tree (2026-11-22): MAX_MODULES = 255 (routing IDs 1-254, tower 255), qsim.py
  likewise; `pos` checks the raw value against 0..255 instead of 0..10. Both `pos` and neighbor
  IDs had been cast to uint8_t before the check, so 256 or -1 wrapped into range (neighbor 257
  became module 1): fixed, with tests. The limit is the simulator's own (src/defs.h); the SDK has
  no module-count constant, so firmware and metal builds are unchanged.
- Tests that encoded 24: ModuleId bound, tub module 25, LTL module(25), now 255/256. The
  calibration statistics are about a fleet, not the ID space: that file uses its own
  FLEET_END = 25 (24 modules), which also keeps it at its old run time.
- VS Code LTLf extension: default module IDs 1-254 + 255 (extension.js; package.json no longer
  repeats the list), validator.wasm rebuilt (emscripten 7.0.0; accepts module(254), rejects 256).
- Verified: MiniPC cosmo x86_64 47/47 + format-check; Docker arm64 native clang 21 with
  ASan+UBSan 47/47. (Docker Desktop's disk was nearly full with other sessions' data: mount a
  clean rsync of the sources, /private/tmp/qs-clean, not the worktree.)

## Process transport remnants (2026-11-22)

- Missed in the first pass: q_sdk's `lockstep_shm.h` (199 lines) was still there. The in-process
  path only used its mailbox (`quercus_mailbox_t`, the slot/capacity constants, the helpers).
  The rest served the deleted process transport: the control block and barrier words, segments,
  runahead/selective grants and certificates, futex banks, `QUERCUS_SHM_ABI`, the
  `QSIM_CTRL_FD`/`QSIM_MBOX_FD`/`QSIM_MODULE_ID` environment names. The mailbox moved into
  `sim_instance.h`, the image ABI that uses it, byte for byte (4 slots, 4088 bytes); the header is
  gone. `quercus_mailbox_used` was only used by append: inlined.
- `MT_TICK`/`MT_ACK` ("lockstep-only") in rpc_wire.h: nothing sent them; gone, with frame.cpp's
  `MT_ACK` status case. The decoder never range-checked types.
- inprocess_backend.h sized its instance table with `QUERCUS_MODULE_TABLE_SIZE`; it uses the
  simulator's own `MODULE_TABLE_SIZE` (256, src/defs.h) now.
- Docs: PROTOCOL.md described the barrier, runahead and certificates; it describes the
  per-grant mailboxes now. Both PlantUML diagrams showed the process architecture
  (`childsync::*`, "lockstep adapter", "starts as process"): redrawn for in-process worlds.
  rpc.c's boot log said "bound to shared sync transport": "bound to the simulator".
- Tested: MiniPC x86_64, cosmo 122 47/47 and native clang 21 47/47. macOS arm64 is waiting for
  the toolchain round 2 release (above): with 122, its arm64 builds fail by design since the
  NATIVE_TARGET_HOST and apelink workarounds were removed. Docker's VM disk is full (a 45 GB
  volume in use and a stopped 6.3 GB debian:trixie-slim container, neither from this work), so
  no arm64 Linux run this time.

## GUI auto-fit (2026-11-22)

- Before: a fixed 800x800 window and a fixed module pitch (126 px + 14 px at `scale = 1`), from
  position (0, 0): about 5x5 modules fitted, the old 11x11 grid only at `scale` <= 0.4, and
  offset layouts drew off screen.
- Now (visualization.cpp, fit_layout(), every frame): the active modules' bounding box,
  shifted to its own top left corner, scaled to fit the window above the 70 px toolbar and
  centred; a module is at most 250 px wide (a 1-module world). `scale` (setting, zoom buttons,
  Ctrl -/+) zooms from the fitted size: 1.0 = fit. Window resizes refit. Text stays fixed-size:
  at 16x16 modules (~40 px) the 3-digit IDs crowd their module but stay legible.
- Seen via a scratch-only frame dump (QSIM_SHOT=file.bmp saves frame 30 and exits; the dummy
  SDL driver renders in software; /private/tmp/qs-scripts/shotpatch.py, gui-shots2.sh):
  1 module, router18, router18 shifted by (+100, +60) (identical image), 10x5, 16x16 (254),
  router18 in 1400x700. This process can't capture real windows (no Screen Recording grant).
- Docker 26.04 native with libsdl3-0: world.gui ran (not skipped), 47/47.
- Tested (macOS arm64, 2026-11-22) on a copy of 122 with cosmocc-build's native LLVM build of
  patches 0001-0017, the libc with 0047/0048, and cmake + coreutils rebuilt on it: 47/47, with
  CMakeLists.txt's apelink command, qsim.py's apelink step and qbuild's NATIVE_TARGET_HOST line
  removed. tests.exe and every world are APEs straight from the driver; Zephyr's CMake sees
  `aarch64`; no "-pthread unused" warnings; the bundle's llvm-cov reports source-based coverage.
- Two more toolchain bugs surfaced and were fixed there: --coverage programs crashed at start
  (gcov's constructor ran before malloc's: LLVM 0017), and apelink refused -mtiny arm64 programs
  (empty .zip section: cosmopolitan 0048), which the simulator's -mtiny world links would hit.
- OPEN (toolchain, also in 122): on macOS, an arm64 ELF exactly as lld wrote it is SIGKILLed when
  `bin/ape` starts it in place; any copy runs. The simulator never runs such files.
- Labels (module IDs, tub IDs, module types, debug values) scale with the modules:
  label_scale_ = clamp(round(unit_ * 4), 1, 5), offsets scaled from their size-2 values. Sizes:
  1 module 5, router18 3 (was 2), 10x5 2, 16x16 1 (no longer crowded). Native 26.04 with SDL3:
  47/47, world.gui run. (`debug` can't be set from a settings file: always starts off.)

## Pico 2 with upstream Zephyr (2026-11-22)

- The user's experiment: Zephyr SDK 1.0.1's cmake/ files copied to /tmp/zephyr-sdk-cosmo-proxy,
  `llvm/bin` a link to the bundle's bin, ZEPHYR_TOOLCHAIN_VARIANT=zephyr/llvm. It works.
- Not needed: Zephyr 4.5's own `host/llvm` variant sets the bare-metal triples for Arm
  (armv8m.main-none-eabi for the RP2350's M33) too. With LLVM_TOOLCHAIN_PATH = the bundle,
  CONFIG_LLVM_USE_LLD and CONFIG_COMPILER_RT_RTLIB (the bundle has no libgcc), `qbuild pico
  messaging` gave a zephyr.bin byte-identical to the proxy build. Same target, CPU and float ABI
  flags; the .config differs only in toolchain-name, rtlib and linker symbols. 15 s clean on M1.
- Zephyr needs LLVM_TOOLCHAIN_PATH: generic.cmake globs it for picolibc.h (TOOLCHAIN_HAS_PICOLIBC),
  and compiler/clang/target.cmake globs `${LLVM_TOOLCHAIN_PATH}/picolibc.cfg` whenever
  CONFIG_PICOLIBC is on. Passing -DTOOLCHAIN_HAS_PICOLIBC=ON instead left the path empty, so
  that glob walked `/`, the whole disk (configure "hung" at 60% CPU). UPSTREAM ZEPHYR: skip those
  globs when LLVM_TOOLCHAIN_PATH is empty. The bundle has no picolibc.cfg: clang selects the
  picolibc variant through lib/clang-runtimes/multilib.yaml.
- qbuild: both targets use host/llvm with lld and LLVM_TOOLCHAIN_PATH=$COSMOCC_TOOLCHAIN_PATH
  (which also pins the toolchain to the bundle rather than the first clang on PATH). Pico adds
  CONFIG_COMPILER_RT_RTLIB. `--toolchain host` now applies to Pico too: no variant at all, so
  Zephyr finds the installed Zephyr SDK (GNU arm-zephyr-eabi): 26 s clean, out/host/, nothing
  from cosmocc. Without an activated bundle and without `--toolchain host`, qbuild stops with
  "activate the cosmocc bundle, or build with --toolchain host", also for a configured tree
  (before, the host's CMake tried to start the bundle's ninja: "exec format error").
- qbuild accepts application paths (`messaging/`), as tab completion writes them.
- Board: `rpi_pico2/rp2350a/m33` (Zephyr 4.5 no longer expands `//m33` early enough), and the
  overlay deletes the board's `partitions` node (both from the user's working tree, as in the
  train suite's bcfcdfa).
- TOOLCHAIN idea: if activation also exported LLVM_TOOLCHAIN_PATH, Zephyr would read it itself
  (zephyr_get) and qbuild's line would go.

## Release 124, amd64 package (2026-11-22)

- Taken from pipeline #200994's build cache (`pkg-amd64.tar.xz`, wrapping cosmocc-amd64.tar.xz),
  installed with cosmocc-build's dist/install.sh --archive on the MiniPC, PATH replaced.
  `# cosmocc 2026.11.22-124`. Full suite on the current tree (with every change that needs 124:
  no apelink calls, no NATIVE_TARGET_HOST, the bundle's llvm-cov): 47/47. Clean simulator build
  76 s on the N100.
- README's coverage steps verbatim: 70 profiles, HTML report with the bundle's llvm-cov,
  80.0% of regions in src/. llvm-cov warns "16 functions have mismatched data": probably header
  functions compiled differently in tests.exe and in the world entry point (which defines
  _COSMO_SOURCE, the tests don't). TO CHECK.

## Release 124, published (2026-11-22)

- `2026.11.22-124` released by pipeline #200993/#200994. Installed locally with its own
  install.sh (replacing 122): clean simulator build 36 s on the M1, format-check, 47/47, and
  `qbuild pico messaging` in 26 s with configure.
- Mirrored to GitHub as release `2026.11.22-124` on spaceface777/q_simulator (pre-release):
  cosmocc.tar.xz, bsdtar, install.sh, install.ps1, SHA256SUMS, checked against the GitLab
  release's SHA256SUMS. The installers fetch only cosmocc.tar.xz and bsdtar from it.
- Pico compile step (hyperfine, 10 runs, after configure, ccache off, M1): bundle clang 22
  4.68 s +- 0.29, Zephyr SDK zephyr/llvm (clang 19) 4.69 s +- 0.32; peak memory 120 vs 139 MiB.
  messaging.bin: 58,668 bytes (bundle), 68,876 (SDK LLVM, its picolibc with unrolled mem* and
  unwind tables), 58,296 (SDK GCC 14.3). Clean qbuild with ccache emptied: 22 s bundle, 25 s
  SDK GCC.

## First CI run on 124 (run 45080642948, 2026-11-22)

- Passed (47/47 each, world.gui really run with SDL3 where it ran): cosmocc on all six Ubuntu
  images, macos-15, macos-26 and xcode-27 (Apple silicon); native on all six; both sanitizer jobs.
- Failed, all in the bundle's own tools, which passed on release 64 (run 44375419708):
  - macos-15-intel, macos-26-intel: `west init -l zephyr` (the bundle's Python) segfaults
    (exit 139), right after the bundle's git cloned Zephyr.
  - windows-2022, windows-2025, windows-11-arm, windows-11-vs2026-arm (cmd and pwsh): the step
    exits 1 with no message right after the bundle's git finishes checking out Zephyr: either
    git clone's status or west init. TOOLCHAIN: to reproduce on Windows (or Wine) with 124.
- The q_sdk submodule fetch needs the branch on the course GitLab
  (2irr70-capstone-quercus-airport/system/q_sdk): the worktree's q_sdk `origin` is a local path.

## Coverage: "16 functions have mismatched data"

- 3 in tests.exe's mapping, 13 in qsim.o's; all one-line accessors defined in headers
  (Queue::size, Runtime::world, Observation::revision, ...). tests.exe's and the worlds'
  profiles agree on every hash but `main` (two different mains, expected). Building the tests at
  -O0 like the core changes nothing. Not explained yet; those accessors lose their counts.
- ROOT CAUSE of the Intel macOS and Windows failures (2026-11-23): one bug, the bundle's Python
  crashing at start. CPython's mimalloc reads its thread id with `__builtin_thread_pointer()`,
  which clang lowered to a raw `%fs:0` for the cosmo target (patch 0006 only covered TLS
  variables). Confirmed on the runners with a throwaway diagnostic workflow (Windows: exit
  0xC0000005; Intel macOS: segfault in mi_heap_main_init) and under Rosetta. Fixed in
  cosmocc-build dbddddc (0006 lowers the intrinsic through __get_tls_<reg> too), tested with a
  local LLVM build. Needs a new release; nothing to change here.

## CI tracks the latest release (user, 2026-11-23)

- No GitHub mirror and no pinned version: the cosmocc jobs run the latest release's install.sh /
  install.ps1 from GitLab's permalink (`.../-/releases/permalink/latest/downloads/`), which then
  download that release. Users install the latest release too, so a toolchain change that breaks
  the simulator shows up in its next CI run. The older mirror releases on GitHub stay, unused.

## Run 45089940928 on release 126: clang crashes on Intel macOS and Windows

- Python works now. clang itself segfaulted on `<chrono>` on Intel macOS and on all four
  Windows runners; Linux and Apple silicon passed. Cause: cosmo's `getauxval(AT_PAGESZ)` returned
  0 where the system passes no auxiliary vector (XNU x86-64, Windows), so LLVM's MemoryBuffer
  mapped page-sized headers and read one byte past them. Fixed in cosmocc-build aaadfba
  (patches/cosmopolitan/0049). Nothing to change here.
- Rehearsed the macos-15-intel job locally under Rosetta (`arch -x86_64 /bin/bash`; uname -m
  says x86_64, so install.sh installs the amd64 package of 126). To emulate the fix, the
  pad script (../tmp/rosetta-ci-scripts) appended a newline to every text file of 16 KiB or more
  whose size is a multiple of 4096: one in the bundle (`__chrono/formatter.h`), one in Zephyr. The
  CI's bash steps then pass: west init, configure, build (simulator, applets, worlds),
  format-check, and ctest 47/47 (world.gui skipped, no SDL3, as on the runner). Two differences
  from the runner: Zephyr was cloned with the Mac's git, as cosmo programs can't start threads
  under Rosetta (cosmocc-build patch 0030); and the global git config was off, as on a runner.
  So nothing past the page size is expected to break on Intel macOS. Windows is not rehearsed.
