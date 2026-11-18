#!/usr/bin/env bash
# Run inside a disposable Ubuntu container.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends clang lld llvm libc++-dev libc++abi-dev libclang-rt-dev cmake ninja-build git ca-certificates python3-venv make device-tree-compiler libsdl3-0 file
# Remove the distro's compiler and binutils executables if pulled in as dependencies.
for pkg in $(dpkg-query -W -f='${Package}\n'); do
    case "$pkg" in binutils|binutils-*|gcc|gcc-[0-9]*|g++|g++-[0-9]*|cpp|cpp-[0-9]*)
        while read -r path; do
            case "$path" in /usr/bin/*|/usr/lib/gcc/*|/usr/libexec/gcc/*|/usr/*-linux-gnu/bin/*)
                if [[ -f "$path" && -x "$path" ]]; then rm -f "$path"; fi ;;
            esac
        done < <(dpkg -L "$pkg");;
    esac
done
for tool in gcc g++ ld.bfd as; do ! command -v "$tool" || exit 1; done
export LLVM_TOOLCHAIN_PATH=$(llvm-config --prefix)
export PATH="$LLVM_TOOLCHAIN_PATH/bin:$PATH"
ln -sf "$(command -v ld.lld)" /usr/local/bin/ld
printf '%s\n' '--rtlib=compiler-rt' > "$LLVM_TOOLCHAIN_PATH/bin/clang.cfg"
printf '%s\n' '--rtlib=compiler-rt' '-stdlib=libc++' > "$LLVM_TOOLCHAIN_PATH/bin/clang++.cfg"
export CC=clang CXX=clang++
python3 -m venv /tmp/llvm-venv
source /tmp/llvm-venv/bin/activate
export ZEPHYR_BASE=/tmp/llvm-west/zephyr
git clone --depth 1 -b v4.4.1 https://github.com/zephyrproject-rtos/zephyr "$ZEPHYR_BASE"
pip install west -r "$ZEPHYR_BASE/scripts/requirements-base.txt"
(cd /tmp/llvm-west && west init -l zephyr)
git -C q_sdk apply "$PWD/.github/llvm-evaluation/sdk.patch"
cmake -S . -B build-llvm -G Ninja -DQSIM_COSMOCC=OFF -DQSIM_LLVM_EVALUATION=ON -DENABLE_LTO=ON -DCMAKE_AR="$(command -v llvm-ar)" -DCMAKE_RANLIB="$(command -v llvm-ranlib)" -DCMAKE_OBJCOPY="$(command -v llvm-objcopy)"
cmake --build build-llvm --parallel 4
file build-llvm/CMakeFiles/core.dir/src/settings.cpp.o | grep 'LLVM IR bitcode'
llvm-readelf -h build-llvm/qsim-core.o | grep REL
RUN_SLOW_TESTS=1 ctest --test-dir build-llvm -j4 --output-on-failure
