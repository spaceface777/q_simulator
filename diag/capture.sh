#!/bin/bash
PATH=/usr/bin:/bin:/usr/sbin:/sbin
# capture.sh CHILD_PID OUTDIR: record everything about a spinning fork child
# and its parent (ps, fault rate, sample, vmmap, lldb stop reason, registers,
# disassembly of the libSystem code involved, kernel log).
c=$1 out=$2
p=$(ps -o ppid= -p "$c" | tr -d ' ')
mkdir -p "$out"
SUDO=; sudo -n true 2>/dev/null && SUDO="sudo -n"
L() { echo "== $*"; }
{
  L "sw_vers"; sw_vers; uname -a; sysctl -n machdep.cpu.brand_string hw.ncpu hw.memsize kern.osversion
  L "ps child $c parent $p"
  ps -o pid,ppid,pgid,stat,pri,nice,utime,stime,time,etime,rss,vsz,command -p "$c,$p"
  L "threads"; ps -M -p "$c,$p"
  L "top fault rate (2 samples, 1 s apart)"
  top -l 2 -s 1 -pid "$c" -stats pid,cpu,faults,cow,csw,msgsent,sysbsd,sysmach,state,time 2>&1 | tail -4
  L "vm_stat"; vm_stat; L "memory_pressure"; memory_pressure -Q 2>&1 | tail -3
  L "sysctl vm"; sysctl vm.swapusage vm.compressor_mode vm.memory_pressure 2>&1
} > "$out/ps.txt" 2>&1
/usr/bin/sample "$c" 1 -mayDie > "$out/sample-child.txt" 2>&1
/usr/bin/vmmap -w -interleaved "$c" > "$out/vmmap-child.txt" 2>&1
/usr/bin/vmmap -w -interleaved "$p" > "$out/vmmap-parent.txt" 2>&1

# the child: attaching reports the pending exception, if there is one
$SUDO lldb --batch -o "process attach --pid $c" \
  -k "thread list" -k "register read" -k "register read --set 2" \
  -k "bt all" -k "dis -p -c 12" -k "image lookup -a \$pc" \
  -k "memory region \$x0" -k "memory region \$far" \
  -k "frame select 1" -k "register read x0 x1 x2 x19 x20 x21 x22 x23 lr" -k "dis -p -c 4" \
  -k "frame select 2" -k "register read x0 x19 x20 x21 x22 x23 lr" -k "dis -s \$pc-48 -c 16" \
  -k "frame select 3" -k "register read x19 x20 x21 x22 lr" \
  -k "frame select 0" \
  -k "dis -n _platform_memset" -k "dis -n _xzm_free" -k "dis -n _objc_atfork_child" \
  -k "dis -n libSystem_atfork_child" -k "dis -n dispatch_atfork_child" \
  -k "image list" \
  -k "memory read -fx -s8 -c16 \$x0-64" \
  -k "process continue" -k "thread list" -k "register read far esr" \
  -k "expr -- (void*)pthread_getspecific(40)" -k "expr -- (void*)pthread_getspecific(41)" \
  -k "expr -- (void*)pthread_getspecific(42)" -k "expr -- (void*)pthread_self()" \
  -k "detach" \
  -o "thread list" -o "register read" -o "bt all" -o "dis -p -c 12" -o "memory region \$x0" \
  -o "dis -n _platform_memset" -o "dis -n _xzm_free" -o "dis -n _objc_atfork_child" -o "detach" \
  > "$out/lldb-child.txt" 2>&1 &
lp=$!; ( sleep 120; kill -9 $lp 2>/dev/null ) & wait $lp

addr=$(grep -m1 -oE "far = 0x[0-9a-f]+" "$out/lldb-child.txt" | cut -d' ' -f3)
x0=$(grep -m1 -oE "^ +x0 = 0x[0-9a-f]+" "$out/lldb-child.txt" | awk '{print $3}')
echo "child far=$addr x0=$x0" > "$out/addr.txt"
# the parent: the same addresses, objc's thread data, malloc's view
pc=(); for a in $addr $x0; do
  pc+=(-o "memory region $a" -o "memory read -fx -s8 -c8 $a" -o "p (unsigned long)malloc_size((void*)$a)" -o "p (void*)malloc_zone_from_ptr((void*)$a)")
done
$SUDO lldb --batch -o "process attach --pid $p" -o "thread list" -o "bt all" "${pc[@]}" \
  -o "expr -- void *\$d = (void*)pthread_getspecific(40)" -o "p \$d" -o "memory read -fx -s8 -c10 \$d" \
  -o "p (unsigned long)malloc_size(\$d)" -o "p (unsigned long)malloc_size(*(void**)((char*)\$d+8))" \
  -o "memory region *(void**)((char*)\$d+8)" \
  -o "expr -- (void*)pthread_getspecific(41)" -o "expr -- (void*)pthread_getspecific(42)" \
  -o "detach" > "$out/lldb-parent.txt" 2>&1 &
lp=$!; ( sleep 120; kill -9 $lp 2>/dev/null ) & wait $lp

# where the faulting address sits in both maps
for a in $addr $x0; do
  for w in child parent; do
    echo "== $w around $a"
    python3 -I -c 'import re,sys
a=int(sys.argv[1],16)
for l in open(sys.argv[2]):
  m=re.search(r" ([0-9a-f]+)-([0-9a-f]+) ",l)
  if m and int(m.group(2),16)>a-0x2000000 and int(m.group(1),16)<a+0x2000000: print(l.rstrip())' "$a" "$out/vmmap-$w.txt"
  done
done > "$out/around.txt" 2>&1
[ -n "$APE_SCAN_LOG" ] && grep -E "pid=($c|$p) " "$APE_SCAN_LOG" > "$out/scan.txt"
log show --last 10m --style compact --predicate 'process == "kernel" OR eventMessage CONTAINS[c] "ape" OR eventMessage CONTAINS[c] "clang"' 2>/dev/null | tail -400 > "$out/log.txt"
echo "captured $c (parent $p) into $out: far=$addr x0=$x0"
