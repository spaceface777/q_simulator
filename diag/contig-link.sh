#!/bin/bash
# contig-link.sh OUT BASE CC-ARGS...: link a Mach-O executable whose segments
# all sit contiguously from BASE (ld-prime won't move later segments after a
# -segaddr): link once at the default address, read each segment's size, and
# link again with every segment placed explicitly.
set -e
out=$1 base=$2; shift 2
/usr/bin/cc "$@" -o "$out.pre"
args=$(otool -l "$out.pre" | python3 -I -c '
import sys
import os
base, off, name = int(sys.argv[1], 0), 0, None
hole = os.environ.get("HOLE_BEFORE", "")  # SEG:BYTES leaves a hole before SEG
for l in sys.stdin:
    f = l.split()
    if not f: continue
    if f[0] == "segname": name = f[1]
    elif f[0] == "vmsize" and name and name != "__PAGEZERO":
        if hole and hole.split(":")[0] == name: off += int(hole.split(":")[1], 0)
        print("-Wl,-segaddr,%s,%#x" % (name, base + off), end=" ")
        off += int(f[1], 16); name = None
' "$base")
/usr/bin/cc "$@" -o "$out" $args
