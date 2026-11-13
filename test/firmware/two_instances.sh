#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
image=${1:-"$root/external/q_applets/out/demo_app.o"}
tools=${COSMOCC_TOOLCHAIN_PATH:?Set COSMOCC_TOOLCHAIN_PATH to the extracted cosmocc toolchain}/bin
case "$(uname -sm)" in
    Darwin\ arm64|Linux\ aarch64) arch=aarch64 ;;
    *) arch=x86_64 ;;
esac
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
out=$scratch/two_instances.exe
"$tools/$arch-unknown-cosmo-cc" -D_GNU_SOURCE -I"$root/external/q_sdk/include" \
    "$root/test/firmware/two_instances.c" "$image" "$image" -o "$out"
if [ "$(uname -sm)" = "Darwin arm64" ]; then
    "$tools/apelink" -l "$tools/ape-aarch64.elf" -M "$tools/ape-m1.c" -o "$out" "$out.dbg"
    sh "$out"
else
    "$out"
fi
