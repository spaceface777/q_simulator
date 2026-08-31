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
├── events_example.toml      # Example event schedule
├── layout_example.toml      # Example module layout
└── settings_example.toml    # Example run settings
```

## Requirements

**Installation**

- Ubuntu 24 or later, or Debian 13 or later
- CMake 3.12 or later
- Ninja
- A C++23 compiler
- SDL3 development files for the GUI
- West, Zephyr, and the native simulator toolchain for the applications
- Internet access for the first CMake configuration

**Usage**

- Ubuntu 24 or later, or Debian 13 or later
- The `native_sim` application files used by the selected layout

## Installation

This setup is already present on the course VM.

1. Clone this repository with its submodules.

```sh
git clone --recurse-submodules <repository-url>
cd simulation
```

2. If the repository already exists, initialize the pinned submodules.

```sh
git submodule update --init --recursive
```

3. Install the build packages.

```sh
sudo apt-get install lcov build-essential git make pkg-config cmake ninja-build gnome-desktop-testing libasound2-dev libpulse-dev libaudio-dev libfribidi-dev libjack-dev libsndio-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libdrm-dev libgbm-dev libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev libudev-dev libthai-dev libusb-1.0-0-dev
```

4. Build the pinned applications for Zephyr `native_sim`.

```sh
cd external/q_applets
./q_sdk/qbuild sim
cd ../..
```

5. Copy the example input files.

```sh
cp events_example.toml events.toml
cp settings_example.toml settings.toml
cp layout_example.toml layout.toml
```

6. Set `application_path` in `settings.toml`. You can also set `binary` for one module in `layout.toml`.

7. Configure the build.

```sh
cmake -B build -S . -G Ninja
```

## Usage

#### Running

Build and start the interactive simulator with a self-contained scenario.

```sh
cmake --build build -j
./build/simulation scenarios/laser_alignment.toml
```

The scenario's `[scenario]` table may set `layout` and `binary`; relative paths start from the scenario file's folder. It starts one application process for each active module. The GUI shows the world state and lets you pause the run, set its speed, and change the scale. Close the GUI to stop the run and all child processes.

Without a scenario argument, the simulator retains the original behavior and reads `settings.toml`, `layout.toml`, and `events.toml`. In `simulation-scenario`, `--layout` and `--binary` still work and override values from the scenario.

The simulator pins itself and all application processes to one CPU to avoid cross-core wakeup and cache-coherence overhead in the short lockstep tick. Use `taskset -c N` when starting it to choose that CPU explicitly; otherwise it uses the CPU on which it starts.

Use `--no-gui` for a run without a window. Use `--timeout <sec>` to stop after a simulated-time limit. Use `--tick-batch <n>` to grant up to `n` child ticks per barrier. The default batch is 1.

A larger tick batch trades timing accuracy for speed. With a batch of N, the simulator cannot observe or answer an application action until the batch boundary, so N ms becomes the smallest effective response step. A batch of 10 runs about seven times faster than a batch of 1. The result varies with the CPU, layout, scenario, and application.

Run a finite scenario and its LTLf properties with the headless runner.

```sh
./build/simulation-scenario scenarios/hardware_test.toml
```

A complete scenario header looks like this:

```toml
[scenario]
layout = "../layouts/one_module.toml"
binary = "../external/q_applets/out/laser_alignment.exe"
duration = "60s"
properties = "laser_alignment.spec"
```

The `layout`, `binary`, and `properties` paths are relative to the scenario file. `layout` and `binary` are optional for compatibility with existing scenarios; when absent, the commands use `layout.toml` and `application_path` from `settings.toml`.

Use `./build/simulation --help` and `./build/simulation-scenario --help` for all options. The scenario runner returns 0 when all properties hold, 1 when a property fails or the run cannot finish, and 2 when its input is invalid.

The installable VS Code extension in `editors/vscode-ltlf` provides syntax highlighting and live parser and type-check diagnostics for `.spec` files. See its README for installation and rebuild instructions.

#### Testing

Build and run the normal test suite.

```sh
cmake --build build -j && ./build/tests
```

Build the pinned applets first, and set `RUN_SLOW_TESTS=1`, to run the full real-application scenario and speed-invariance tests.

```sh
RUN_SLOW_TESTS=1 ./build/tests
```

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
