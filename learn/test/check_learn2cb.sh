#!/usr/bin/env bash
# learn2cb end to end on AI4I 2020: it must flag the failure-mode columns as recorded after the event, and with them
# left out it must draft a casebook that cbc validates over every state, with an honest cross-validated estimate.
#   check_learn2cb.sh <learn2cb> <cbc> <ai4i2020.csv>
set -uo pipefail
learn=$1 cbc=$2 data=$3
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL  $*"; exit 1; }

"$learn" "$data" --outcome "Machine failure" --asserts failure > "$work/leaky.cb" 2> "$work/leaky.txt" || fail "learn2cb failed"
for flag in TWF HDF PWF OSF; do
    grep -q "^  $flag is 1 " "$work/leaky.txt" || fail "$flag was not flagged as recorded after the event"
done
grep -q "skipped          'UDI': every value distinct and increasing, so an ID" "$work/leaky.txt" || fail "UDI not skipped as an ID"

"$learn" "$data" --outcome "Machine failure" --asserts failure --ignore TWF,HDF,PWF,OSF,RNF --dated 2026-10-01 \
    > "$work/clean.cb" 2> "$work/clean.txt" || fail "learn2cb failed without the flag columns"
grep -q "WARNING" "$work/clean.txt" && fail "a warning remained after the flag columns were ignored"
ba=$(sed -n 's/^  learned casebook *\([0-9.]*\)$/\1/p' "$work/clean.txt")
[[ -n "$ba" ]] || fail "no cross-validated estimate reported"
awk -v b="$ba" 'BEGIN { exit !(b > 0.55 && b < 0.95) }' || fail "implausible balanced accuracy $ba"
"$cbc" "$work/clean.cb" --quiet || fail "cbc refused the learned casebook"
echo "learn2cb: flags 4 after-the-event columns; without them drafts a casebook cbc validates (CV balanced accuracy $ba)"
