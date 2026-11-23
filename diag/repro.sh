#!/bin/bash
# repro.sh N PROGRAM [LOADER]: run PROGRAM N times (through LOADER if
# given), sample the first hung fork child, and count the hangs.
PATH=$PATH:/usr/bin:/bin:/usr/sbin:/sbin
n=$1 prog=$2 loader=$3 hung=0 sampled=0 start=$SECONDS
for ((i = 0; i < n; i++)); do
  $loader "$prog" > /dev/null 2> repro.err &
  r=$!
  for ((t = 0; t < 40 && $(kill -0 $r 2>/dev/null && echo 1 || echo 0); t++)); do
    /bin/sleep 0.05
  done
  if (( !sampled )) && kill -0 $r 2>/dev/null; then
    c=$(pgrep -P $r | head -1)
    if [ -n "$c" ]; then
      sampled=1
      echo "launch $i: child $c of $r: $(ps -o stat=,utime=,stime= -p $c)"
      /usr/bin/sample $c 1 2>/dev/null | grep -E "^ +[0-9]+ " | tail -6 | cut -c1-110
    fi
  fi
  wait $r || { hung=$((hung + 1)); cat repro.err; }
done
echo "$prog${loader:+ via $loader}: $hung of $n launches hung ($((SECONDS - start)) s)"
