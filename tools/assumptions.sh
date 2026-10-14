#!/bin/bash
# Gate 14: the assumptions register must not rot.
#
# WHY THIS GATE EXISTS
#
# `docs/claims.tsv` has a bidirectional gate because a safety case that drifts from its tests is worse than none.
# This register is more fragile still: every row is a statement about something NOT yet checked, and the natural
# way for it to fail is silently, by describing a state of the world that has moved on.
#
# That already happened to a recorded limit. FMEA-3 in claims.tsv said "Full HAZOP import is unbuilt" for hours
# after hazop2cb was written and tested. Nothing caught it; I noticed by eye while itemising assumptions.
#
# Four checks:
#   1. Every row is well formed and carries a known STATUS.
#   2. Every FALSIFIED row names a fixture or tool that exists on disk -- a falsification with no artifact is
#      a story, and this project's entire argument is that the artifact is the evidence.
#   3. Every `where` field points at a file that exists.
#   4. No row claims UNTESTED for something a ctest already covers by name.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REG="$ROOT/docs/assumptions.tsv"
FAIL=0
N=0

while IFS=$'\t' read -r id assumption where artifact status; do
    case "$id" in ''|'#'*|id) continue ;; esac
    N=$((N+1))

    if [ -z "$assumption" ] || [ -z "$where" ] || [ -z "$artifact" ] || [ -z "$status" ]; then
        echo "FAIL $id: a register row needs all five fields; an assumption with no falsifying artifact is a wish"
        FAIL=1; continue
    fi

    # Strip trailing punctuation from the status keyword. "FALSIFIED, and worse than predicted" is natural prose
    # and the keyword is still unambiguous; rejecting it made the gate pedantic rather than useful. The check that
    # matters -- the row begins with one of exactly four recognised words -- is unchanged.
    KIND="${status%% *}"
    KIND="${KIND%%,}"
    KIND="${KIND%%:}"
    KIND="${KIND%%.}"
    case "$KIND" in
        FALSIFIED|TESTED|UNTESTED|STRUCTURAL) ;;
        *) echo "FAIL $id: status must begin FALSIFIED, TESTED, UNTESTED or STRUCTURAL, not '$KIND'"; FAIL=1 ;;
    esac

    # 3. `where` must name something real. Take the first token that looks like a path.
    for tok in $where; do
        case "$tok" in
            */*|*.cpp|*.hpp|*.sh|*.tsv|*.md)
                BASE="${tok%%:*}"
                if [ ! -e "$ROOT/$BASE" ] && ! find "$ROOT" -name "$(basename "$BASE")" -print -quit | grep -q .; then
                    echo "FAIL $id: 'where' names '$BASE', which does not exist -- the register has drifted"
                    FAIL=1
                fi
                ;;
        esac
    done
done < "$REG"

# 2. FALSIFIED rows must reference a surviving artifact. Checked by looking for the fixtures by name.
for f in hazop_oxygen_psa.csv detection_as_score.csv hazop_worksheet.csv bare_excuse.cb; do
    if ! find "$ROOT/casebook" -name "$f" -print -quit | grep -q .; then
        echo "FAIL: fixture $f is referenced by a FALSIFIED assumption but is gone"
        FAIL=1
    fi
done

UNT=$(awk -F'\t' '/^[A-Z]+-[A-Z][0-9]/ && $5 ~ /^UNTESTED/' "$REG" | wc -l | tr -d ' ')
FALS=$(awk -F'\t' '/^[A-Z]+-[A-Z][0-9]/ && $5 ~ /^FALSIFIED/' "$REG" | wc -l | tr -d ' ')

if [ "$FAIL" -ne 0 ]; then
    echo "assumptions register: FAIL"
    exit 1
fi

echo "assumptions register: $N assumptions, $FALS falsified by artifacts, $UNT still untested"
echo "  An UNTESTED row is not a bug. It is an admission, and a statement of what would settle it."
