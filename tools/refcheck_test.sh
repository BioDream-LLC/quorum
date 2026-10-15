#!/bin/bash
# tools/refcheck.py, the second implementation of the language, against cbc: every rulebook in the repository, then
# random rulebooks; then each of six deliberate mistakes in refcheck must be noticed, which shows the comparison has
# teeth rather than two programs agreeing about nothing.
#   tools/refcheck_test.sh <cbc>
set -u
cbc=$1
cd "$(dirname "$0")/.."
fail=0
out=$(python3 tools/refcheck.py "$cbc" $(git ls-files 'casebook/*.cb' 2>/dev/null || find casebook -name '*.cb')) || fail=1
echo "$out" | grep -E "^(DIFF|SAME)" | sed 's/^/  /'
echo "$out" | grep -q "^DIFF" && { echo "FAIL: cbc and refcheck disagree on a repository rulebook"; fail=1; }
python3 tools/refcheck.py "$cbc" --fuzz 300 1 | tail -1 || fail=1
for b in no-severity undercut-cancels any-is-all doubts-read-usable no-prefer vote-off-by-one; do
  n=$(REFCHECK_BREAK=$b REFCHECK_SHOW=0 python3 tools/refcheck.py "$cbc" --fuzz 1500 9 | tail -1 | sed -E 's/.* ([0-9]+) differed$/\1/')
  if [ "${n:-0}" -gt 0 ]; then echo "  break $b: noticed ($n random rulebooks differ)"; else echo "FAIL: break $b went unnoticed"; fail=1; fi
done
exit $fail
