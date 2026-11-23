#!/bin/bash
PATH=/usr/bin:/bin:/usr/sbin:/sbin
# watchdog.sh OUTDIR [CPU_SECONDS]: until OUTDIR/stop exists, look for fork
# children that spin before exec (same argv as their parent, CPU time above
# the threshold) and capture each one once with capture.sh.
out=$1 lim=${2:-20} here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
while [ ! -e "$out/stop" ]; do
  ps -axo pid=,ppid=,time=,command= | python3 -I -c '
import sys
lim = float(sys.argv[1])
rows = {}
for l in sys.stdin:
    f = l.split(None, 3)
    if len(f) < 4: continue
    pid, ppid, t, cmd = f[0], f[1], f[2], f[3].rstrip("\n")
    p = t.split(":")
    secs = sum(float(x) * 60 ** i for i, x in enumerate(reversed(p)))
    rows[pid] = (ppid, secs, cmd)
for pid, (ppid, secs, cmd) in rows.items():
    if secs >= lim and ppid in rows and rows[ppid][2] == cmd:
        print(pid)
' "$lim" | while read -r c; do
    [ -e "$out/seen-$c" ] && continue
    touch "$out/seen-$c"
    echo "watchdog: spinning fork child $c: $(ps -o time=,command= -p "$c" | cut -c1-160)"
    "$here/capture.sh" "$c" "$out/capture-$c"
  done
  sleep 5
done
