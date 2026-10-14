#!/bin/bash
# Exhaustive independent verification: every state of every casebook, labelling rebuilt by CaDiCaL.
#
# Supersedes the sampled check in tools/cross-verify.sh. Sampling was the weak form of this evidence, and running it
# exhaustively is what revealed that one of the three implications being tested was mathematically false -- see the
# comment block in tools/cadical_exhaustive.cpp.
#
# Depends on a CaDiCaL build in a sibling project, so it skips cleanly rather than failing when absent.
set -uo pipefail
cd "$(dirname "$0")/.."

# CaDiCaL, pinned and built by tools/get-cadical.sh.
CADICAL_DIR=${CADICAL_DIR:-$HOME/cadical}
CADICAL_SRC=${CADICAL_SRC:-$CADICAL_DIR/src}
CADICAL_LIB=${CADICAL_LIB:-$CADICAL_DIR/build/libcadical.a}
GEN=build/casebook/compiler/gen

if [ ! -f "$CADICAL_LIB" ] || [ ! -f "$CADICAL_SRC/cadical.hpp" ]; then
    echo "SKIP: CaDiCaL not found (wanted $CADICAL_LIB). Run tools/get-cadical.sh to build it."
    echo "      This is the only check here that is not quorum's code checking quorum's other code,"
    echo "      so a skip is a real loss of evidence rather than a cosmetic one."
    exit 0
fi
[ -f "$GEN/pump_generated.hpp" ] || { echo "run the host build first"; exit 1; }

BIN=$(mktemp -u)
trap 'rm -f "$BIN"' EXIT
if ! g++ -std=c++17 -O2 -I"$CADICAL_SRC" -Isolver/include -Iarbiter/include -Ibinding/include -I"$GEN" \
        tools/cadical_exhaustive.cpp "$CADICAL_LIB" -o "$BIN" 2>/tmp/qcv_err; then
    echo "FAIL: could not build the checker"
    head -10 /tmp/qcv_err
    exit 1
fi

FAIL=0
TOTAL_STATES=0
TOTAL_CALLS=0

printf '%-10s %10s %12s %12s\n' casebook states "SAT calls" mismatches
for cb in permit vote pump tank; do
    OUT=$("$BIN" "$cb" 2>&1) || FAIL=1
    ST=$(echo "$OUT" | awk '/^states/{print $NF}')
    CALLS=$(echo "$OUT" | awk '/^CaDiCaL solve calls/{print $NF}')
    MM=$(echo "$OUT" | awk '/^mismatches/{print $NF}')
    printf '%-10s %10s %12s %12s\n' "$cb" "$ST" "$CALLS" "$MM"
    if [ "$MM" != 0 ]; then
        echo "$OUT" | grep MISMATCH | head -5 | sed 's/^/    /'
        FAIL=1
    fi
    TOTAL_STATES=$((TOTAL_STATES + ST))
    TOTAL_CALLS=$((TOTAL_CALLS + CALLS))

    # A comparison that cannot fail is not a comparison. Corrupt one label and require the run to fail.
    if "$BIN" "$cb" --mutate 3 >/dev/null 2>&1; then
        echo "FAIL: $cb passed with a deliberately corrupted label -- the check is vacuous"
        FAIL=1
    fi
done

echo
echo "$TOTAL_STATES states, $TOTAL_CALLS CaDiCaL calls, self-test confirms the check can fail"
[ "$FAIL" -ne 0 ] && exit 1
echo "every label of every argument in every state agrees with a solver sharing no code with quorum"
exit 0
