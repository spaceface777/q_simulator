# Cosmopolitan P2 builds

## Prerequisites

Use the project cosmocc release `2026.11.12-20` and a west workspace with Zephyr v4.5.0.
Apply the SDK patch `external/q_sdk/patches/zephyr-v4.5.0-cosmocc.patch` to Zephyr.
Set `ZEPHYR_BASE` to the patched checkout.
Set `COSMOCC_TOOLCHAIN_PATH` to the extracted toolchain.

On Apple Silicon, the toolchain ships the native APE loader as C source.
On first use without a cached or installed loader, the APE launcher uses Apple `cc` to build it.

## Firmware artifacts

From the simulator root, run these commands to build both backend variants of `demo_app`.
Copy each pair before the next build replaces the files in `out/`.

```sh
cd external/q_applets
mkdir -p out/coroutine out/pthread
./q_sdk/qbuild sim demo_app --zephyr-backend coroutine
cp out/demo_app.exe out/demo_app.o out/coroutine/
./q_sdk/qbuild sim demo_app --zephyr-backend pthread
cp out/demo_app.exe out/demo_app.o out/pthread/
cd ../..
```

The coroutine backend is the default.
For a process measurement, pass the selected `.exe` to the existing scenario runner.
For an embedded measurement, link the selected `.o` into a test executable.
P3 adds world assembly from a layout.

## Two-copy test

Run the same test with both firmware objects.

```sh
test/firmware/two_instances.sh "$PWD/external/q_applets/out/coroutine/demo_app.o"
test/firmware/two_instances.sh "$PWD/external/q_applets/out/pthread/demo_app.o"
```

The test alternates grants between two linked copies of the same object.
Each copy receives a different module ID over RPC and reports its own ID.

## Simulator build

Use the toolchain file for a headless Cosmopolitan build.

```sh
cmake -S . -B build-cosmo -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/cosmocc.cmake -DTESTING_CONSOLE_BUILD=ON
cmake --build build-cosmo
```

For a Linux host-toolchain build, omit `-DCMAKE_TOOLCHAIN_FILE` and use a separate build directory.

## GitLab CI

The current GitLab job calls `qbuild sim`. That step requires the pinned cosmocc archive and patched Zephyr checkout.
P9 will provision these dependencies in GitLab CI.
