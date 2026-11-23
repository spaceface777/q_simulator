#!/bin/sh
# make-resv-loader.sh APE_M1_C OUT: the scanning loader (apescan.h) with a
# 4.25 GiB zero-fill segment that claims the program range at exec time. The
# whole loader is linked contiguously from 0x7f0000000 (macOS 15 kills images
# with a detached segment), so the reservation covers [~0x7f0030000+slide,
# ~0x900030000+slide), i.e. 0x800000000 plus 4 GiB for any slide below 0xf000000.
set -e
src=$1 out=$2 here=$(cd "$(dirname "$0")" && pwd)
"$here/make-scan-loader.sh" "$src" "$out.tmp" > /dev/null
printf '%s\n' '__asm__(".zerofill __APERESV,__resv,_ape_resv,0x110000000,14");' >> "$out.tmp.c"
"$here/contig-link.sh" "$out" 0x7f0000000 -w -O "$out.tmp.c"
