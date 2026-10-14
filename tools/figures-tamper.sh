#!/usr/bin/env bash
#
# Prove tools/figures.sh can FAIL.
#
# A check that cannot fail is worse than no check: it consumes CI time and buys confidence it has not earned. This repo has
# already shipped one vacuous check and one check that was testing a mathematically false theorem, and both looked healthy
# from the outside because they passed. So every gate that guards a document gets a test of its FAILING path.
#
# Four ways the documents can drift, one per tamper:
#
#   1  a number inside a generated region is edited by hand          -> STALE
#   2  a new unaccountable figure appears in the README              -> UNREGISTERED
#   3  a figure in the safety case is changed                        -> UNREGISTERED + ORPHANED
#   4  a registered figure is removed from both documents            -> ORPHANED
#
# Tamper 3 is the one worth reading: editing a single number fires BOTH directions at once, the new value being
# unaccountable and the old one no longer appearing anywhere. That is the property the safety case check lacked until it was
# made bidirectional, and it is why this one was built that way from the start.
#
# Every tamper is reverted whether the assertion passes or fails; the trap is what makes that true.
set -euo pipefail
cd "$(dirname "$0")/.."

BAK="$(mktemp -d)"
cp README.md "$BAK/README.md"
cp docs/claims.tsv "$BAK/claims.tsv"

restore() {
    cp "$BAK/README.md" README.md
    cp "$BAK/claims.tsv" docs/claims.tsv
    rm -rf "$BAK"
}
trap restore EXIT

fail=0

# Assert that figures.sh rejects the current (tampered) documents, and that it says why.
expect_caught() {
    local what="$1" pattern="$2"
    local out
    if out="$(./tools/figures.sh 2>&1)"; then
        printf '  NOT CAUGHT  %s\n' "$what"
        printf '              figures.sh passed on a document it should have rejected\n'
        fail=1
        return
    fi
    if ! printf '%s' "$out" | grep -qE "$pattern"; then
        printf '  WRONG REASON  %s\n' "$what"
        printf '              rejected, but not for %s\n' "$pattern"
        printf '%s\n' "$out" | sed 's/^/                /' | head -12
        fail=1
        return
    fi
    printf '  caught      %s\n' "$what"
}

echo "proving tools/figures.sh can fail"
echo

# Sanity first: the untampered documents must PASS, or every assertion below is meaningless.
if ! ./tools/figures.sh >/dev/null 2>&1; then
    echo "  ABORT       the untampered documents already fail figures.sh"
    echo "              fix that first; nothing below would mean anything"
    exit 1
fi
echo "  baseline    untampered documents pass"

# 1 ── a hand edit inside a generated region.
# Edit whatever the row says now. This used to name 2,620; when the footprint became 2,864 the sed matched nothing,
# the document was never tampered, and the gate looked vacuous when the tamper was.
sed -i -E 's/^(\| \+ binding layer \| )[0-9,]+ \|/\11 \|/' README.md
grep -q '^| + binding layer | 1 |' README.md || { echo "  tamper 1 did not apply: the footprint row changed shape"; exit 1; }
expect_caught "hand edit inside a generated region" "STALE +footprint"
cp "$BAK/README.md" README.md

# 2 ── a figure nobody can account for. 332 is the real worst-case stack; 7,777 is not.
sed -i 's/worst-case stack of 332 bytes/worst-case stack of 7,777 bytes/' README.md
expect_caught "unaccountable new figure in README" "UNREGISTERED +7,777"
cp "$BAK/README.md" README.md

# 3 ── a changed figure in the safety case. Both directions must fire.
sed -i 's/2,253,824/2,999,999/' docs/claims.tsv
expect_caught "changed figure in the safety case (new value)" "UNREGISTERED +2,999,999"
sed -i 's/2,253,824/2,999,999/' docs/claims.tsv 2>/dev/null || true
expect_caught "changed figure in the safety case (old value orphaned)" "ORPHANED +2,253,824"
cp "$BAK/claims.tsv" docs/claims.tsv

# 4 ── a registered figure that has silently left both documents.
sed -i 's/33,554,432/XXX/' docs/claims.tsv
expect_caught "registered figure removed from both documents" "ORPHANED +33,554,432"
cp "$BAK/claims.tsv" docs/claims.tsv

echo
if [[ $fail -ne 0 ]]; then
    echo "figures.sh does NOT reliably fail; the gate is not trustworthy"
    exit 1
fi
echo "all four drift paths are detected, so the gate is not vacuous"
