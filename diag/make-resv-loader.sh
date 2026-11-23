#!/bin/sh
# make-resv-loader.sh APE_M1_C OUT: the scanning loader (apescan.h) plus a
# zero-fill segment that claims [0x7f0000000, 0x900000000) at exec time, so
# that nothing (libmalloc in particular) can be placed where programs load.
set -e
src=$1 out=$2 here=$(cd "$(dirname "$0")" && pwd)
"$here/make-scan-loader.sh" "$src" "$out.tmp" > /dev/null
printf '%s\n' '__asm__(".zerofill __APERESV,__resv,_ape_resv,0x110000000,14");' >> "$out.tmp.c"
/usr/bin/cc -w -O -o "$out" "$out.tmp.c" -Wl,-segaddr,__APERESV,0x7f0000000
