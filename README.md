# Quercus Simulation

This repository contains the Quercus Airport Simulation. Read this file before you build or run the simulator.

![A screenshot of the simulator GUI](docs/images/gui.png)

## Repository Structure

```
simulation/
├── docs/                    # Architecture, protocol, and diagrams
├── editors/                 # LTLf editor support
├── external/                # Pinned application and SDK submodules
├── layouts/                 # Example module layouts
├── scenarios/               # Finite scenarios and LTLf properties
├── src/                     # Simulator source code
├── test/                    # Unit, integration, and scenario tests
├── .clang-format            # C++ format rules
├── .gitlab-ci.yml           # GitLab CI job
├── .gitmodules              # Submodule locations
├── CMakeLists.txt           # Main build file
├── layout_example.toml      # Example module layout
└── settings_example.toml    # Example run settings
```

## Build a world

Initialize the SDK and applet submodules.

```sh
git submodule update --init --recursive
```

Use the toolchain and Zephyr checkout in the [P2 prerequisites](docs/COSMOPOLITAN_P2.md#prerequisites).
Build the firmware objects.

```sh
(cd external/q_applets && ./q_sdk/qbuild sim)
```

Configure and build the simulator with Cosmopolitan.

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/cosmocc.cmake
cmake --build build
```

Build a world from a layout.

```sh
python3 build/qsim build layouts/hardware_test.toml -o world.exe
```

A layout can set a default `firmware` path and override it for each module.
Firmware paths are relative to the layout.
The world build embeds the layout and one copy of the selected firmware for each active module.
It writes the executable and a separate `world.exe.dbg` file.
The world embeds the SHA-256 digest of each firmware object for the attestation RPCs.
All simulated modules use the public test key with bytes `0x00` through `0x1f`.
Rebuild firmware objects from P2 through P4 before you use them in a P5 world.

## Run a scenario

Pass the scenario to the world executable.

```sh
./world.exe scenarios/hardware_test_blocked_beam.toml
```

On macOS, use `sh ./world.exe` if the system does not start APE files directly.
A world can run compatible scenarios in separate invocations.
Use `--settings settings_example.toml` to supply runtime settings.
The world uses built-in defaults for each setting that the file omits.
If you omit `--settings`, the world uses built-in defaults for the entire run.
If the layout or firmware changes, build the world again.
The current build runs headlessly. The `--gui` option reports that GUI support is unavailable until P8.

The scenario runner returns 0 when all properties hold and 1 when a property fails or remains undecided.
It also returns 1 when firmware crashes or the simulation cannot finish, and 2 when its input is invalid.
After a firmware fault or watchdog timeout, the world stops that module and continues the scenario.
Keep the matching `world.exe.dbg` beside the executable for source lines in crash reports.
Set `QSIM_WATCHDOG_MS` to change the five-second grant watchdog limit. Set it to zero to disable the watchdog.

Each scenario supplies `[scenario].duration`.
If the run needs assertions, set `properties` to an LTLf file relative to the scenario.
Put timed events in the scenario file. See [the tub arrivals example](scenarios/tub_arrivals.toml).
Set firmware variables in `[scenario.environment]`.
Firmware reads those variables and its own `QUERCUS_MODULE_ID` through `getenv`.
The simulator keeps the shell environment separate.

| Layout under `layouts/` | Compatible scenarios under `scenarios/` |
| --- | --- |
| `hardware_test.toml` | `hardware_test.toml`, `hardware_test_blocked_beam.toml`, `hardware_test_tag_at_startup.toml` |
| `laser_alignment.toml` | `laser_alignment.toml` |
| `one_module.toml` | `module_movement.toml`, `module_movement_idle.toml` |
| `default.toml` | `per_module_variation.toml`, `tub_arrivals.toml` |

## Optimization

See [Build and measure optimized worlds](docs/OPTIMIZATION.md) for profile training and benchmarks.

## Tests

Run the simulator unit tests.

```sh
ctest --test-dir build --output-on-failure
```

On Linux, configure a separate build directory without the toolchain file to use the host compiler.
After the firmware build, run the linked-world tests.

```sh
python3 test/sim/test_worlds.py build/qsim
```

The tests build one world per layout and compare traces across repeated runs with default and explicit settings.
The two RFID diagnostics retain one undecided property each with the historical applet revision.
The tests require those exact verdicts.

#### Test coverage

Generate an HTML coverage report with a Debug build.

```sh
BROWSER=firefox

cmake -B build -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug -S . -G Ninja
cmake --build build --target clean -j
cmake --build build --target tests -j
./build/tests
cd build
lcov --capture --directory . --output-file coverage.info --ignore-errors gcov,gcov --ignore-errors inconsistent --rc geninfo_unexecuted_blocks=1
lcov --remove coverage.info '/usr/*' '*/_deps/*' '*/test/*' --output-file coverage_filtered.info
genhtml coverage_filtered.info --output-directory coverage_html
cd ..
cmake -B build -S . -G Ninja
$(echo $BROWSER) build/coverage_html/index.html
```

Change `firefox` on the first line to the browser that you use.

## Support

Create a GitLab issue if you find a problem. Use the provided issue format.

## Contributing

Contributions are welcome. A merge request must pass the format check and the applicable tests.

All C++ code must follow `.clang-format`. Add unit or integration tests for changes outside the visualization and `main.cpp`. Read `docs/ARCHITECTURE.md` and `docs/PROTOCOL.md` before you change the engine or its application interface.

## License

(c) Eindhoven University of Technology, Rens Verstappen, and contributors (2025-2026).

## Credits
Repository icons courtesy of [Magnific](https://www.magnific.com/) and [fancykeith](https://www.flaticon.com/authors/fancykeith).
