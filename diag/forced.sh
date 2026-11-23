#!/bin/bash
# forced.sh OUTDIR N: N test links whose clang driver is re-executed until the
# loader's MAP_FIXED replaces a malloc segment (APE_SCAN_POLICY=force); count
# how many then hang in the fork child. Also N clang --version under force.
PATH=$PATH:/usr/bin:/bin:/usr/sbin:/sbin
out=$1 n=$2
mkdir -p "$out"; cd "$out" || exit 1
printf 'int main(void){return 0;}\n' > src.c
clang -c src.c -o src.c.obj || exit 1
ok=0 hung=0 fail=0
for ((i = 0; i < n; i++)); do
  APE_SCAN_POLICY=force clang -fuse-ld=lld src.c.obj -o cmTC_$i > last.log 2>&1 &
  c=$! s=$SECONDS
  while kill -0 $c 2>/dev/null && (( SECONDS - s < 40 )); do /bin/sleep 0.1; done
  if kill -0 $c 2>/dev/null; then
    hung=$((hung + 1)); echo "forced link $i HUNG"
    (( hung == 1 )) && /bin/sleep 100   # first one: let the watchdog capture it
    /usr/bin/pkill -9 -P $c; kill -9 $c
  fi
  wait $c && ok=$((ok + 1)) || { fail=$((fail + 1)); echo "forced link $i exit $?: $(head -c 300 last.log)"; }
done
echo "forced links: n=$n ok=$ok hung=$hung fail=$fail"
v=0
for ((i = 0; i < n; i++)); do APE_SCAN_POLICY=force clang --version > /dev/null 2>&1 && v=$((v + 1)); done
echo "forced clang --version: $v of $n ok"
