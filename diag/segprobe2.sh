#!/bin/bash
# segprobe2.sh OUTDIR: does this kernel's exec accept these loader layouts?
here=$(cd "$(dirname "$0")" && pwd); out=$1; mkdir -p "$out"; cd "$out" || exit 1
src() { printf '#include <stdio.h>\n#include <mach-o/getsect.h>\n#include <mach-o/dyld.h>\n__asm__(".zerofill __APERESV,__resv,_ape_resv,%s,14");\nint main(void){const struct segment_command_64 *s=getsegbyname("__APERESV");printf("%%#llx\\n", s ? s->vmaddr + _dyld_get_image_vmaddr_slide(0) : 0ull);return 0;}\n' "$1" > "$2"; }
try() { # name description build-command...
  local n=$1 d=$2; shift 2
  "$@" > "$n.build.log" 2>&1 || { echo "$n ($d): build failed: $(tail -1 $n.build.log)"; return; }
  ok=0; for i in 1 2 3 4 5; do r=$("./$n" 2>/dev/null) && ok=$((ok + 1)); done
  echo "$n ($d): $ok/5 ran (last: ${r:-killed}); $(otool -l $n | awk '/segname/{s=$2} /vmaddr/{printf "%s@%s ", s, $2}')"
}
src 0x110000000 r1.c; src 0x800000000 r2.c
try d0 "contiguous at 0x7f0000000 (round 5)" "$here/contig-link.sh" d0 0x7f0000000 -O1 -w r1.c
try d1 "pagezero to 0x7f0000000, contiguous" "$here/contig-link.sh" d1 0x7f0000000 -O1 -w r1.c -Wl,-pagezero_size,0x7f0000000
try d2 "32 GiB zero-fill straight after __DATA" /usr/bin/cc -O1 -w -o d2 r2.c
try d3 "1 MiB hole before the zero-fill" env HOLE_BEFORE=__APERESV:0x100000 "$here/contig-link.sh" d3 0x100000000 -O1 -w r1.c
try d4 "1 MiB hole after the zero page" "$here/contig-link.sh" d4 0x100100000 -O1 -w r1.c
try d5 "default layout, 4.25 GiB zero-fill (b1)" /usr/bin/cc -O1 -w -o d5 r1.c
