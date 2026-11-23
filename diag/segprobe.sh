#!/bin/bash
# segprobe.sh OUTDIR: which zero-fill segments (address, size) does this
# kernel accept in an executable? Prints one line per variant.
out=$1; mkdir -p "$out"; cd "$out" || exit 1
mk() { # name addr size
  printf '#include <stdio.h>\n#include <mach-o/getsect.h>\n#include <mach-o/dyld.h>\n__asm__(".zerofill __APERESV,__resv,_ape_resv,%s,14");\nint main(void){const struct segment_command_64 *s=getsegbyname("__APERESV");printf("%%#llx\\n", s ? s->vmaddr + _dyld_get_image_vmaddr_slide(0) : 0ull);return 0;}\n' "$3" > "$1.c"
  if [ "${2#base}" != "$2" ]; then /usr/bin/cc -O1 -w -o "$1" "$1.c" -Wl,-segaddr,__TEXT,"${2#base}"; otool -l "$1" | grep -A3 -E "segname (__TEXT|__APERESV|__LINKEDIT)$" | grep -E "segname|vmaddr" | paste - - | sed "s/^/  $1: /"
  elif [ "$2" = none ]; then /usr/bin/cc -O1 -w -o "$1" "$1.c"; else /usr/bin/cc -O1 -w -o "$1" "$1.c" -Wl,-segaddr,__APERESV,"$2"; fi
}
run() { # name addr size
  mk "$@" 2>&1 | head -2
  ok=0; for i in 1 2 3 4 5; do r=$("./$1" 2>/dev/null) && ok=$((ok + 1)); done
  echo "segment at $2 size $3: $ok/5 ran (last: ${r:-killed})"
}
run a1 0x200000000 0x1000000
run a2 0x400000000 0x1000000
run a3 0x600000000 0x1000000
run a4 0x7f0000000 0x1000000
run a5 0x800000000 0x1000000
run a6 0xa00000000 0x1000000
run a7 0x1000000000 0x1000000
run a8 0x2000000000 0x1000000
run a9 0x8000000000 0x1000000
run b1 none 0x110000000
run b2 none 0x1000000
run b3 0x7f0000000 0x110000000
run c1 base0x7f0000000 0x110000000
run c2 base0x7c0000000 0x150000000
run c3 base0x400000000 0x500000000
start=$(date -u +%Y-%m-%dT%H:%M:%S)
./a4; ./b3
/bin/sleep 2
log show --style compact --start "$(date -j -u -f %Y-%m-%dT%H:%M:%S "$start" +"%Y-%m-%d %H:%M:%S")" --predicate 'process == "kernel" OR sender == "AppleMobileFileIntegrity" OR process == "amfid" OR eventMessage CONTAINS[c] "a4" OR eventMessage CONTAINS[c] "shared region"' 2>/dev/null | tail -40
