#!/bin/bash
# osinfo.sh OUTDIR: static facts about this macOS: the libSystem code on the
# hang path, whether a blocked fault spins, objc's thread data at a cosmo
# clang's fork, and the address-space layout at that point.
out=$1 here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out/w"
{ sw_vers; uname -a; sysctl -n machdep.cpu.brand_string hw.ncpu hw.memsize kern.osversion
  csrutil status; DevToolsSecurity -status; sysctl kern.hv_vmm_present 2>&1; } > "$out/os.txt" 2>&1
printf '#include <unistd.h>\nint main(){sleep(1);return 0;}\n' > "$out/w/sleeper.c"
/usr/bin/cc -g -o "$out/w/sleeper" "$out/w/sleeper.c"
/usr/bin/lldb --batch -o "target create $out/w/sleeper" -o "b main" -o run \
  -o "dis -n _platform_memset" -o "dis -n _xzm_free" -o "dis -n _objc_atfork_child" \
  -o "dis -n libSystem_atfork_child" -o "dis -n dispatch_atfork_child" -o "dis -n fork -s libsystem_c.dylib" \
  -o "image list" -o kill > "$out/disasm.txt" 2>&1
/usr/bin/cc -O1 -o "$out/w/blockedfault" "$here/blockedfault.c"
"$out/w/blockedfault" blocked 2>/dev/null & b=$!; /bin/sleep 3
{ /bin/ps -o pid,stat,utime,stime,time -p $b; /usr/bin/top -l 1 -pid $b -stats pid,cpu,faults,state | tail -2; } > "$out/blockedfault.txt" 2>&1
kill -9 $b
"$out/w/blockedfault" 2>/dev/null; echo "unblocked exit $?" >> "$out/blockedfault.txt"
printf 'int main(void){return 0;}\n' > "$out/w/src.c"
(cd "$out/w" && clang -c src.c -o src.c.obj)
T=$(dirname "$(command -v clang)")
(cd "$out/w" && /usr/bin/lldb --batch -o "command script import $here/trace_vm.py" -o "vmtrace $out/forktrace" -- "$T/ape" "$T/clang" -fuse-ld=lld src.c.obj -o cmTC > "$out/forktrace.lldb" 2>&1)
(cd "$out/w" && /usr/bin/lldb --batch -o "b -n fork -s libsystem_c.dylib" -o run \
  -o "expr -- void *\$d = (void*)pthread_getspecific(40)" -o "p \$d" -o "memory read -fx -s8 -c10 \$d" \
  -o "p (unsigned long)malloc_size(\$d)" -o "p (unsigned long)malloc_size(*(void**)((char*)\$d+8))" \
  -o "memory region *(void**)((char*)\$d+8)" -o "memory region \$d" \
  -o "expr -- (void*)pthread_getspecific(41)" -o "expr -- (void*)pthread_getspecific(42)" -o kill \
  -- "$T/ape" "$T/clang" -fuse-ld=lld src.c.obj -o cmTC > "$out/forkslot.txt" 2>&1)
echo "osinfo done"
