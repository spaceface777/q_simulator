# Cosmopolitan P2 builds

These build and benchmark instructions describe the `cosmopolitan-p2` branch.

## Prerequisites

Use the project cosmocc release `2026.11.12-20` and a west workspace with Zephyr v4.5.0.
Apply the SDK patch `q_sdk/patches/zephyr-v4.5.0-cosmocc.patch` to Zephyr.
Set `ZEPHYR_BASE` to the patched checkout.
Set `COSMOCC_TOOLCHAIN_PATH` to the extracted toolchain.

On Apple Silicon, the toolchain ships the native APE loader as C source.
On first use without a cached or installed loader, the APE launcher uses Apple `cc` to build it.

## Firmware artifacts

From the simulator root, run these commands to build both backend variants of the `messaging` test applet.
Copy each pair before the next build replaces the files in `out/`.

```sh
cd test/applets
mkdir -p out/coroutine out/pthread
q_sdk/qbuild sim messaging --zephyr-backend coroutine
cp out/messaging.exe out/messaging.o out/coroutine/
q_sdk/qbuild sim messaging --zephyr-backend pthread
cp out/messaging.exe out/messaging.o out/pthread/
cd ../..
```

The coroutine backend is the default.
For a process measurement, pass the selected `.exe` to the existing scenario runner.
For an embedded measurement, link the selected `.o` into a test executable.
P3 adds world assembly from a layout.

## Two-copy test

The `greetings` scenario of the `messaging` applet links two copies of one object into a world.
Run it with each firmware object.

```sh
python3 test/run_scenario.py build-cosmo/qsim test/applets/out/coroutine test/applets/messaging/scenarios/greetings
python3 test/run_scenario.py build-cosmo/qsim test/applets/out/pthread test/applets/messaging/scenarios/greetings
```

Each copy receives a different module ID over RPC and reports its own ID.

## Simulator build

The build uses cosmocc by default. Configure a headless Cosmopolitan build.

```sh
cmake -S . -B build-cosmo -G Ninja -DTESTING_CONSOLE_BUILD=ON
cmake --build build-cosmo
```

For a Linux host-toolchain build, add `-DQSIM_COSMOCC=OFF` and use a separate build directory.

## GitLab CI

The current GitLab job calls `qbuild sim`. That step requires the pinned cosmocc archive and patched Zephyr checkout.
P9 will provision these dependencies in GitLab CI.
