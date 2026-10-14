# Quercus simulator

The simulator runs a private Zephyr native_sim firmware image for each active module.
The host runs the world, network, platform, and LTLf property monitor.
Build applications with `qbuild sim`, then select their `.dll` files in the runtime configuration.
Changing firmware or the layout does not require rebuilding the simulator.

## Build

The simulator requires CMake, Ninja, Python, a C++23 compiler, and the recursive submodules.
The GUI also requires SDL3 and SDL3_image. CMake downloads GoogleTest for the test build.

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DTESTING_CONSOLE_BUILD=ON
cmake --build build -j
```

Set `TESTING_CONSOLE_BUILD=OFF` to include the GUI. Set `BUILD_TESTING=OFF` to omit tests.
The simulator build uses SDK headers. It does not build firmware or require a Zephyr checkout.

Firmware builds require an external Zephyr workspace. The SDK records its tested revision in
[`ZEPHYR_REVISION`](external/q_sdk/ZEPHYR_REVISION) and warns if the checkout differs.
The tested branch is `native-sim-context-backends-pr118530` at `eefcaecadec`.
Use the Python environment that contains the Zephyr dependencies.

```sh
export ZEPHYR_BASE=/path/to/workspace/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=host
(cd external/q_applets && ./q_sdk/qbuild sim)
```

`qbuild sim` builds 64-bit native_sim images on macOS and Linux.
It writes `out/<application>.dll` in the application repository.
The extension is `.dll` on both systems. An image must match the simulator's host architecture.

## Select firmware and run

A settings file supplies the default firmware:

```toml
firmware = "external/q_applets/out/module_movement.dll"
```

A module can override that default in the layout:

```toml
[module.255]
active = true
type = "tower"
firmware = "../external/q_applets/out/atc_fixed.dll"
```

A scenario can override the settings default with `[scenario].firmware` and select `[scenario].layout`.
Each firmware or layout path is relative to the file that declares it.
The parser checks configuration syntax. Startup validates firmware bindings and reports missing images with their module IDs and paths.
The removed `binary` and `application_path` keys produce configuration errors.

Run a bundled headless scenario:

```sh
./build/simulation-scenario --settings settings_example.toml scenarios/module_movement.toml
```

The headless runner returns 0 when every property holds.
It returns 1 for a failed or incomplete run, and 2 for a configuration error. Use `--trace run.ndjson` to record the run.
Use `--help` for seed, duration, layout, and property-evaluation options.
For the GUI, copy `settings_example.toml` to `settings.toml` and `layout_example.toml` to `layout.toml`,
then run `./build/simulation`. An explicit scenario can select its own layout and default firmware.

The simulator copies each selected image to a private temporary file and loads that copy.
Each module has private globals, kernel state, coroutine stacks, and deterministic entropy.
A new simulation loads fresh copies. Firmware boots when its module receives its first grant.
Platform supervision stops a module that has not activated by tick 10,000; startup then completes normally.

## Firmware environment

Firmware receives only the explicit scenario environment and its own `QUERCUS_MODULE_ID`.
It does not inherit host variables such as `PATH`.

```toml
[scenario.environment]
QSIM_STUB_REFUSE = "10:30000"
```

The runtime copies this map at construction. Each image copies its environment when bound.
Repeated `getenv` calls return stable values without allocating. Traces record the environment map.
The router smoke scenario uses this input with `layouts/physical_layout.toml`:
all conveyor modules run the router, and module 255 runs `atc_fixed.dll`.

## Tests

Portable tests use a scripted shared image to exercise the real loader and runtime ABI.
Real-firmware tests additionally exercise Zephyr, hardware behavior, scenarios, and deterministic replay.
Build the applets and repository probes before running the strict suite:

```sh
(cd external/q_applets && ./q_sdk/qbuild sim)
external/q_sdk/qbuild sim
python3 tools/check_sdk.py
cmake -S . -B build -G Ninja -DTESTING_CONSOLE_BUILD=ON -DQSIM_TEST_EMBEDDED_TRACE=ON
cmake --build build -j
QSIM_REQUIRE_FIRMWARE=1 ctest --test-dir build --output-on-failure
```

Both SDK checkouts must use the same clean revision. CI checks this before building firmware.
By default, firmware tests skip if images are absent. `QSIM_REQUIRE_FIRMWARE=1` makes absence an error.
`QSIM_TEST_FIRMWARE_DIR` can select another applet output directory.
The trace regression checks the complete NDJSON digest across repeated runs and both supported hosts.

Set `RUN_SLOW_TESTS=1` to enable wall-clock pacing tests.
These compare traces at 0.3×, 1×, 10×, and maximum speed, and check measured speed at 1× and 10×.
CI runs the full suite, ten shuffled repetitions, and a separate slow run under wall-clock timeouts.
`cmake --build build --target format-check` checks C++ formatting; the build prefers `clang-format-22`.

## Accepted differences and limitations

Simulator images use the host C library. Pico builds use the toolchain Picolibc.
The image defines simulated clock functions, private `getenv`, private `random`/`srandom`,
and process-exit functions that return a fatal firmware outcome to the host.

- Host `rand`/`srand` state is shared. Execution order makes runs repeatable on one host,
  but one module's calls change another module's sequence.
- Host allocation has no Pico-sized heap limit. Allocations that firmware never frees can survive image unloading.
- Host `errno` belongs to the executing host thread and is shared by images.
  Firmware logs can contain different errno numbers on different hosts.
  RPC statuses always use Zephyr/Picolibc numbering, including ENOSYS 88 and ETIMEDOUT 116.
- Math and floating-point formatting can differ between operating systems.
  Float-heavy traces can differ across hosts while remaining repeatable on one host.
- Firmware `printf` output shares stdout and follows module execution order.
- Firmware is trusted code. A busy loop that never yields hangs the simulator; grants cannot be preempted.
- Module reboot, Windows hosts, 32-bit images, and Rosetta images are unsupported.

[Architecture](docs/ARCHITECTURE.md) and [protocol](docs/PROTOCOL.md) describe the execution boundary.
