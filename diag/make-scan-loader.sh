#!/bin/sh
# make-scan-loader.sh APE_M1_C OUT: build an APE loader that logs what its
# MAP_FIXED program mappings would replace (see apescan.h)
set -e
src=$1 out=$2 here=$(cd "$(dirname "$0")" && pwd)
grep -q '^#include <unistd.h>$' "$src"
grep -q '^    /\* configure mapping \*/$' "$src"
grep -q '^int main(int argc, char \*\*argv, char \*\*envp) {$' "$src"
sed -e "s|^#include <unistd.h>\$|#include <unistd.h>\\
#include \"$here/apescan.h\"|" \
    -e 's|^    /\* configure mapping \*/$|    ApeScan(dynbase + (p[i].p_vaddr \& -pagesz), dynbase + ((p[i].p_vaddr + p[i].p_memsz + pagesz - 1) \& -pagesz), exe);\
    /* configure mapping */|' \
    -e 's|^int main(int argc, char \*\*argv, char \*\*envp) {$|&\
  ApeScanArgs(argc, argv, envp);|' "$src" > "$out.c"
grep -c ApeScan "$out.c"
/usr/bin/cc -w -O -o "$out" "$out.c"
