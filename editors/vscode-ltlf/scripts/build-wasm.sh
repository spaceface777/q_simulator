#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
extension_dir=$(dirname -- "$script_dir")
repository_dir=$(dirname -- "$(dirname -- "$extension_dir")")

em++ \
  "$extension_dir/wasm/validator.cpp" \
  "$repository_dir/src/ltl/lexer.cpp" \
  "$repository_dir/src/ltl/parser.cpp" \
  "$repository_dir/src/ltl/checker.cpp" \
  -I"$repository_dir/external/quercus_sdk/include" \
  -std=c++23 -O3 -flto -fexceptions \
  -sDISABLE_EXCEPTION_CATCHING=0 \
  -sALLOW_MEMORY_GROWTH=1 \
  -sFILESYSTEM=0 \
  -sMODULARIZE=1 \
  -sEXPORT_NAME=createLtlfValidator \
  -sENVIRONMENT=node \
  -sEXPORTED_FUNCTIONS='["_ltlf_validate","_malloc","_free"]' \
  -sEXPORTED_RUNTIME_METHODS='["UTF8ToString","stringToUTF8","lengthBytesUTF8"]' \
  -o "$extension_dir/wasm/validator.js"
