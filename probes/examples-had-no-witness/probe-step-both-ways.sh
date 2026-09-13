#!/usr/bin/env bash
# The step's body EXTRACTED FROM THE FILE, not retyped: a probe of a copy proves
# nothing about the original. The only substitution is the nova binary path, which
# the workflow supplies through env and nova.sh supplies here.
set -uo pipefail
cd /d/Sources/nv-lang/nova-duckdb || exit 2

body() {
  n=$(find examples -name '*.nv' | wc -l)
  echo "example sources found: $n"
  if [ "$n" -eq 0 ]; then echo "ERROR: no example sources"; return 1; fi
  out=$(./nova.sh check examples 2>&1); rc=$?
  if [ "$rc" -ne 0 ]; then echo "ERROR: an example no longer type-checks"; return 1; fi
  plain=$(echo "$out" | sed -E 's/\x1B\[[0-9;]*m//g')
  if ! echo "$plain" | grep -qE '^PASS: [0-9]+'; then
    echo "ERROR: the checker printed no summary line"; return 1
  fi
  echo "$plain" | grep -E '^PASS: [0-9]+'
  return 0
}

echo "=== A: healthy (expect 0) ==="; body; echo "A exit=$?"

F=examples/02-appender-history/appender_history.nv
cp "$F" /tmp/ah.bak
python -c "
import io
p='$F'; s=io.open(p,encoding='utf-8',newline='').read()
o='ORDER BY 1 LIMIT 3\")!!'
assert s.count(o)==1
io.open(p,'w',encoding='utf-8',newline='').write(s.replace(o,'ORDER BY 1 LIMIT 3\")',1))
"
echo "=== B: one \`!!\` removed (expect 1) ==="; body; echo "B exit=$?"
cp /tmp/ah.bak "$F"
echo "=== C: restored (expect 0) ==="; body; echo "C exit=$?"
git -C /d/Sources/nv-lang/nova-duckdb status --porcelain
echo "(end)"
