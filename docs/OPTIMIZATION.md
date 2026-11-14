# Build and measure optimized worlds

Release builds use LTO for `qsim.o`, the Zephyr application, and the native runner.
Each prelink emits native code. World assembly links these objects and writes the executable and debug file.

## Train and use a profile

Use the toolchain and Zephyr workspace from the [build instructions](../README.md#build-a-world).
Start with fresh build directories to collect a new profile.
This example trains both the simulator and the module movement applet.

```sh
cmake -S . -B build-pgo -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/cosmocc.cmake -DPGO=TRAIN
cmake --build build-pgo --target qsim-world
(cd external/q_applets && ./q_sdk/qbuild sim module_movement --pgo train)
python3 build-pgo/qsim build layouts/one_module.toml -o training.exe
sh ./training.exe scenarios/module_movement.toml
sh ./training.exe scenarios/module_movement_idle.toml

cmake -S . -B build-pgo -DPGO=USE
cmake --build build-pgo --target qsim-world
(cd external/q_applets && ./q_sdk/qbuild sim module_movement --pgo use)
python3 build-pgo/qsim build layouts/one_module.toml -o optimized.exe
```

On Windows, launch the executables directly. Repeat the training runs with the layouts, seeds, and scenarios that represent your workload.
The simulator writes profiles to `build-pgo/pgo`. Each applet writes profiles to `build_sim/<app>/pgo` in its repository.
Use the same build directories, compiler, firmware sources, and backend for both stages.
The applet build replaces the files in `out/`. Each assembled world retains the objects from its link.
If you change the source or build options, start a new training run with fresh profiles.

## Compare builds

Save a world with PGO off before training. If you need a build without LTO, set `-DENABLE_LTO=OFF` and pass `--no-lto` to `qbuild sim`.
Use the same layout, settings, and scenarios for every build.

```sh
python3 tools/benchmark_worlds.py --world baseline baseline.exe --world pgo optimized.exe \
  --output results.csv scenarios/module_movement.toml scenarios/module_movement_idle.toml
```

The benchmark records elapsed time per scenario and checks that the exit status and failed or undecided properties agree across runs.
It alternates the execution order and reports median suite time.
The reported time includes process startup. World assembly takes place before the benchmark.

## Router benchmark

The benchmark used the [P5 router suite](COSMOPOLITAN_P5.md) and one 18-module world per build mode.
The Mac and the MiniPC each used their own profiles.
The simulator, Zephyr firmware, and native runners all used the selected build mode.
The MiniPC process could run on CPUs 2 and 3.

| Host | Cases | LTO off | LTO on | LTO and PGO |
| --- | --- | ---: | ---: | ---: |
| Apple Silicon Mac | Full suite | 15.954 s | 15.507 s | 12.709 s |
| Intel N100 MiniPC | Full suite | 34.075 s | 32.751 s | 26.559 s |
| Apple Silicon Mac | Light cases (`battery__*`) | 0.386 s | 0.384 s | 0.363 s |
| Intel N100 MiniPC | Light cases (`battery__*`) | 0.397 s | 0.388 s | 0.348 s |
| Apple Silicon Mac | Heavy cases (`big__*`) | 1.809 s | 1.770 s | 1.448 s |
| Intel N100 MiniPC | Heavy cases (`big__*`) | 3.814 s | 3.671 s | 3.148 s |

Each result is the median of three runs on the same cases used for training.
The benchmark ran 67 cases and skipped four marked cases.
All build modes had the same verdicts, with 60 passes and the same seven failures.
GCC applied partial training to translation units without profile counters.
