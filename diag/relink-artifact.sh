#!/bin/bash
# relink-artifact.sh DIR: GitHub artifacts store symlinks as small files
# holding the target's name; turn such files back into symlinks.
find "$1" -type f -size -256c | while read -r f; do
  t=$(cat "$f"); d=$(dirname "$f")
  case "$t" in */*|*' '*|'') continue ;; esac
  [ -e "$d/$t" ] && [ "$(wc -l < "$f" | tr -d ' ')" = 0 ] && ln -sf "$t" "$f" && echo "relinked $f -> $t"
done
