# Architecture

## Independent builds

The SDK builds applications with `qbuild`. The native_sim profile produces a shared library named
`out/<application>.dll`. Pico builds produce hardware firmware with the toolchain Picolibc.
The simulator uses SDK headers and loads application images at runtime. Its build does not compile firmware.
Application builds use Ninja's dependencies, so firmware changes relink the image without relinking the simulator.

The SDK links each image from Zephyr's localized CPU object, complete coroutine runner library,
and generated platform layout inputs. It uses the upstream registration order and task bounds.
The runner sources are unmodified. The image exports only `qsim_image_api`.
The tested Zephyr revision is recorded in the SDK's `ZEPHYR_REVISION`; configuration warns on a mismatch.
Both simulator SDK submodules use the same clean revision, enforced by CI.

## Image ownership

Settings, layouts, and scenarios select `.dll` paths relative to their containing files.
Each active module needs one image. The runtime loads a private copy of that file with the host OS library loader.
The POSIX implementation uses `dlopen(RTLD_NOW | RTLD_LOCAL)` and Windows uses `LoadLibraryW`.
The loader shares the copy cache, ABI checks, and diagnostic prefixes across hosts.
Separate files prevent the loader from reusing one loaded image for multiple modules.

The copies stay in `qsim-cache/images/<SHA-256 of the image>/<slot>.dll` in the temporary directory.
Set `QSIM_IMAGE_CACHE` to use a different directory.
The host validates a new library file the first time it loads it. On macOS this costs about 25 ms for each copy.
Reused copies avoid this cost in later runs.
In one process, each loaded copy has its own slot. Separate processes can load the same slot.
The loader compares a cached copy with its source before each load, and replaces a copy that differs.
A copy which the host loader did not unload keeps its slot until the process ends.
The cache keeps the eight most recently used images.
The loader reports failures with the source path and OS diagnostic text. It checks ABI version 11 after loading.
The OS loader rejects incompatible architectures before the API can be read.

Each loaded copy owns its globals, Zephyr kernel, scheduler, entropy state, coroutine stacks,
and transport buffers. Retiring a module destroys and unloads its copy.
Runtime shutdown releases the remaining images.
A later simulation loads fresh copies, including when tests run several simulations in one host process.

## Execution and time

Binding stores configuration without running firmware. The module's first grant boots Zephyr through
upstream `nsi_init`, then delivers input and advances to the grant endpoint.
Boot output belongs to that grant. The image preserves the host's SIGINT and SIGTERM handlers around boot.

A grant is an absolute window in one module's native time.
The image first catches up to its start without input, then processes the supplied input through the endpoint.
Events due exactly at the endpoint are included.
If no input or native event is due, the backend skips execution and accumulates the native-time cursor.
The next executed grant catches up before exposing new input.

The host prepares all module grants before executing firmware and consumes output only after the epoch completes.
It copies output into host-owned storage before another module runs.
This keeps same-tick module execution from bypassing network and RPC handoff delays.
The seeded host task queue orders requests; request labels identify work without acting as numeric sort keys.

The world and platform use global ticks. Module clocks independently model startup offsets, drift, and skipped ticks.
Host pacing controls how quickly global time advances relative to wall time.
Platform supervision stops a module without a first heartbeat at tick 10,000.
Stopped modules are simulated outcomes, so startup completes once every module is activated or stopped.
There is no separate wall-clock startup timeout.

## Determinism and host services

The runtime owns an explicit environment map from the scenario. Images copy its `KEY=VALUE` entries at binding,
provide their own `QUERCUS_MODULE_ID`, and never inherit the host environment.
Trace metadata records the supplied map.

Each image defines eleven libc functions: `getenv`, `time`, `gettimeofday`, `clock_gettime`, `exit`, `_exit`,
`abort`, `__stack_chk_fail`, `setvbuf`, `random`, and `srandom`. On macOS it also defines the `malloc` family,
which allocates from a malloc zone of the image's own. These bind within the image.
Clocks return simulated native time. Environment lookups return stable pointers without per-call allocation.
The private PRNG supplies repeatable entropy on both supported hosts.
Fatal exits return a firmware outcome to the host, and `setvbuf` cannot change host stream buffering.

Other C library calls use the host implementation. The README lists the resulting differences and limitations.
The SDK translates every errno-valued RPC status between host numbering and Zephyr/Picolibc wire numbering.
Application text, including errno numbers printed in logs, remains application output.

## Diagnostics and traces

Loader errors name the module and image path. Firmware errors identify the module, operation, status or outcome,
and available diagnostic text. Destroy failures are reported without aborting the host.
Runtime error paths flush the trace before returning control to the caller.
NDJSON traces include configuration digests, environment metadata, clocks, RPC traffic, world observations,
property results, and lifecycle changes. Repeatability checks compare complete bytes without normalization.

## Crash containment

Every call into an image runs under a guard (`src/firmware/crash_posix.cpp`, `crash_windows.cpp`).
A CPU fault or a C library `abort` in application code arrives as a signal, or on Windows as an exception.
The handler records the registers and walks the frame-pointer chain without reading invalid memory.
It then returns control to the guard. A watchdog thread interrupts a grant that runs past its limit the same way.
An application that ends itself reports its stack through the image's fatal hook instead.
Either way, the backend returns the failure with the grant rather than throwing.
The runtime prints the report, applies Core0's safe stop, retires the module, and continues the run.

Reports are symbolized after the guard returns, never in a signal handler, with libbacktrace.
`cmake/libbacktrace.cmake` builds it from source for all three hosts.
Summaries and stop reasons contain no host addresses, so equal runs keep equal traces.
A watchdog stop happens at a wall-clock moment, but always during the same simulated grant.

Images are trusted applications. The loader is an isolation mechanism for globals, not a security boundary.
A faulted image is abandoned, not unloaded: its code may still be referenced by host state.
On Linux and Windows, unloading cannot reclaim leaked host allocations.
