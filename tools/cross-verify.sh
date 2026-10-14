#!/bin/bash
# Cross-check quorum's grounded labelling against an architecturally independent solver.
#
# ── Why this is the most valuable test in the project ───────────────────────
#
# Every other check here compares my code against my other code. `test_grounded` validates the solver against a reference
# implementation I also wrote; `advisory_two_tier` checks containment using an enumerator I also wrote. Agreement proves
# CONSISTENCY, not correctness. A shared misunderstanding of Dung's definitions would be invisible to all of it.
#
# The ICCMA 2025 solver is a different artefact in every respect that matters. It is SAT-based rather than a worklist algorithm, it
# decomposes into strongly connected components, and the actual reasoning is performed by CaDiCaL -- Armin Biere's code,
# not mine. It competed in ICCMA 2025 against mu-toksia, Fudge and scallop, which means its answers were checked by the
# competition organisers against other people's solvers. That last part is what makes this transitively independent rather
# than merely differently-mine.
#
# ── The two theorems being exploited ────────────────────────────────────────
#
# The grounded extension is the LEAST complete extension. Two consequences, each checkable by a solver that has never
# heard of grounded semantics:
#
#   quorum says IN   ->  the argument is in EVERY complete extension, hence every preferred one.
#                        So skeptical acceptance under preferred (DS-PR) must be YES.
#
#   quorum says OUT  ->  the argument is attacked by the grounded extension, so it is in NO complete extension.
#                        So credulous acceptance under complete (DC-CO) must be NO.
#
# Both are theorems, not implementation details. If either fails, one of the two engines is wrong about Dung 1995, and
# finding out which is worth more than any other test result available today.
#
# UNDECIDED carries no such implication -- an argument grounded leaves undecided may or may not be credulously accepted --
# so those are counted and skipped rather than checked, which is stated in the output rather than hidden.
#
# Usage: tools/cross-verify.sh [casebook] [n-states]
set -uo pipefail
cd "$(dirname "$0")/.."

# The ICCMA 2025 solver is optional and not distributed with quorum: set ICCMA_SOLVER to its binary to include it.
SOLVER=${ICCMA_SOLVER:-}
CADICAL_DIR=${CADICAL_DIR:-$HOME/cadical}
CADICAL_SRC=${CADICAL_SRC:-$CADICAL_DIR/src}
CADICAL_LIB=${CADICAL_LIB:-$CADICAL_DIR/build/libcadical.a}
DUMP=build/casebook/compiler/dump_af
CASEBOOK=${1:-pump}
STATES=${2:-12}

HAVE_ICCMA=1
[ -x "$SOLVER" ] || HAVE_ICCMA=0

HAVE_CADICAL=0
CHECKER=""
if [ -f "$CADICAL_LIB" ] && [ -f "$CADICAL_SRC/cadical.hpp" ]; then
    CHECKER=$(mktemp -u)
    if g++ -std=c++17 -O2 -I"$CADICAL_SRC" tools/cadical_check.cpp "$CADICAL_LIB" -o "$CHECKER" 2>/dev/null; then
        HAVE_CADICAL=1
    fi
fi

if [ "$HAVE_ICCMA" = 0 ] && [ "$HAVE_CADICAL" = 0 ]; then
    echo "SKIP: no independent checker available."
    echo "      Wanted CaDiCaL at $CADICAL_LIB, or a solver at $SOLVER"
    exit 0
fi
if [ ! -x "$DUMP" ]; then
    echo "need $DUMP; run: cmake --build build -j"
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "quorum grounded labelling vs an independent SAT-based solver"
echo "============================================================"
echo "casebook            $CASEBOOK"
if [ "$HAVE_CADICAL" = 1 ]; then
    echo "PRIMARY             direct SAT encoding in tools/cadical_check.cpp, solved by CaDiCaL"
    echo "                    The encoding is mine and ~40 lines from Dung 1995. The SEARCH is Armin"
    echo "                    Biere's code, which shares nothing with quorum. Checks IN, OUT *and*"
    echo "                    UNDECIDED -- the last constrains exactly where two implementations are"
    echo "                    most likely to diverge, and no other test in this project has it."
fi
if [ "$HAVE_ICCMA" = 1 ]; then
    echo "SECONDARY           $SOLVER"
    echo "                    SAT-based ICCMA 2025 entrant. Weaker evidence than it first appeared:"
    echo "                    quorum was salvaged from that project and it contains the SAME grounded"
    echo "                    worklist, so an agreement may be one implementation agreeing with itself."
    echo "                    Kept because its DS-PR path is a genuinely different SAT encoding, but it"
    echo "                    is corroboration, not the primary evidence."
fi
echo

TOTAL_IN=0
TOTAL_OUT=0
TOTAL_UNDEC=0
MISMATCH=0
CHECKED_STATES=0

# Which states to check. A spread rather than the first N, because consecutive masks differ in one bit and would test
# almost the same framework repeatedly.
#
# AMBIGUOUS STATES ARE INCLUDED DELIBERATELY. The first run of this script found 0 undecided arguments across 12 random
# states, which is unsurprising -- only 1.37% of the pump's states are contested -- and useless: an argument left
# undecided is exactly where two implementations are most likely to disagree, because it is where the semantics stop
# being forced. A cross-check that only exercises the forced cases is a cross-check of the easy half.
if [ -n "${CROSS_STATES:-}" ]; then
    MASKS="$CROSS_STATES"
else
    MASKS="0"
    for i in $(seq 1 $((STATES - 1))); do
        MASKS="$MASKS $(printf '%x' $(( (i * 2654435761) % 16384 )))"
    done
    # Known contested states of the pump, from pass 5's exhaustive validation.
    [ "$CASEBOOK" = "pump" ] && MASKS="$MASKS 203 223 2a3 283 20b 213"
fi

for MASK in $MASKS; do

    if ! "$DUMP" "$CASEBOOK" "$WORK/f.af" "$MASK" > "$WORK/labels.txt" 2>"$WORK/err"; then
        echo "  state 0x$MASK: dump failed"; cat "$WORK/err"; MISMATCH=1; continue
    fi
    CHECKED_STATES=$((CHECKED_STATES + 1))

    STATE_BAD=0
    while read -r idx label name; do
        case "$label" in
            IN)
                TOTAL_IN=$((TOTAL_IN + 1))
                ANS=$("$SOLVER" -p DS-PR -f "$WORK/f.af" -a "$idx" 2>/dev/null | head -1)
                if [ "$ANS" != "YES" ]; then
                    echo "  MISMATCH state 0x$MASK arg $idx ($name):"
                    echo "    quorum says IN, so it must be in every preferred extension."
                    echo "    independent solver DS-PR says '$ANS'."
                    STATE_BAD=1
                fi
                ;;
            OUT)
                TOTAL_OUT=$((TOTAL_OUT + 1))
                ANS=$("$SOLVER" -p DC-CO -f "$WORK/f.af" -a "$idx" 2>/dev/null | head -1)
                if [ "$ANS" != "NO" ]; then
                    echo "  MISMATCH state 0x$MASK arg $idx ($name):"
                    echo "    quorum says OUT, so it must be in no complete extension."
                    echo "    independent solver DC-CO says '$ANS'."
                    STATE_BAD=1
                fi
                ;;
            UNDEC)
                # No implication available: grounded-undecided says nothing about credulous acceptance either way.
                TOTAL_UNDEC=$((TOTAL_UNDEC + 1))
                ;;
        esac
    done < "$WORK/labels.txt"

    [ "$STATE_BAD" != 0 ] && MISMATCH=1
    UNDEC_HERE=$(grep -c ' UNDEC ' "$WORK/labels.txt" || true)

    if [ "$HAVE_CADICAL" = 1 ]; then
        if OUT=$("$CHECKER" "$WORK/f.af" "$WORK/labels.txt" 2>&1); then
            printf '  state 0x%-6s %s\n' "$MASK" "$OUT"
            TOTAL_UNDEC=$((TOTAL_UNDEC + UNDEC_HERE))
        else
            printf '  state 0x%-6s CADICAL DISAGREES\n' "$MASK"
            echo "$OUT" | sed 's/^/    /'
            MISMATCH=1
        fi
    else
        printf '  state 0x%-6s checked   %s undecided\n' "$MASK" "$UNDEC_HERE"
    fi
done

echo
echo "states cross-checked          $CHECKED_STATES"
echo "IN  verified via DS-PR        $TOTAL_IN"
echo "OUT verified via DC-CO        $TOTAL_OUT"
if [ "$HAVE_CADICAL" = 1 ]; then
    echo "UNDECIDED verified by CaDiCaL $TOTAL_UNDEC   (must be in some complete extension and out of another)"
else
    echo "UNDECIDED (no implication)    $TOTAL_UNDEC   counted, not checked"
fi
echo "queries to the other solver   $((TOTAL_IN + TOTAL_OUT))"
echo

if [ "$MISMATCH" != 0 ]; then
    echo "DISAGREEMENT. One of the two engines is wrong about Dung 1995, and which one matters"
    echo "more than any other open question here. Do not adjust anything until it is understood."
    exit 1
fi

echo "No disagreement."
echo
echo "What this does and does not establish. The SAT encoding is mine, so a bug in it is"
echo "possible -- but it would have to be a bug that agrees with quorum, and the two reach an"
echo "answer by completely different routes: a worklist fixpoint against a clause database and"
echo "conflict-driven search. The search is Armin Biere's code and shares nothing with quorum."
echo
echo "This is the first evidence in the project that is not my code checking my other code."
exit 0
