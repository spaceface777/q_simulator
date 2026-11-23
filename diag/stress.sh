#!/bin/bash
PATH=$PATH:/usr/bin:/bin:/usr/sbin:/sbin
# stress.sh OUTDIR N P [CMD...]: P parallel loops of N CMake-style test links
# (clang -fuse-ld=lld x.o -o x); a link that runs longer than 60 s counts as
# hung and is killed after the watchdog has had time to capture it.
out=$1 n=$2 par=$3; shift 3
mkdir -p "$out"
printf 'int main(void){return 0;}\n' > "$out/src.c"
(cd "$out" && clang -c src.c -o src.c.obj) || exit 1
loop() {
  local k=$1 i ok=0 hung=0 fail=0 d="$out/l$1"
  mkdir -p "$d"; cp "$out/src.c.obj" "$d/"
  for ((i = 0; i < n; i++)); do
    (cd "$d" && exec clang -fuse-ld=lld src.c.obj -o cmTC_$i) > "$d/last.log" 2>&1 &
    local c=$! s=$SECONDS
    while kill -0 $c 2>/dev/null && (( SECONDS - s < 60 )); do /bin/sleep 0.05; done
    if kill -0 $c 2>/dev/null; then
      hung=$((hung + 1)); echo "loop $k link $i HUNG ($(date +%T))"
      /bin/sleep 150; /usr/bin/pkill -9 -P $c; kill -9 $c
    fi
    wait $c && ok=$((ok + 1)) || fail=$((fail + 1))
    /bin/rm -f "$d/cmTC_$i"
  done
  echo "loop $k: ok=$ok hung=$hung fail=$fail"
}
start=$SECONDS
for ((k = 0; k < par; k++)); do loop $k & done
wait
echo "stress: n=$n par=$par in $((SECONDS - start)) s"
