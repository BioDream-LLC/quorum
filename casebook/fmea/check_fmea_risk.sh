#!/bin/bash
# Assert the FMEA risk audit, and the report it produces.
#
# WHY THIS EXISTS AT ALL
#
# MISSION.md ranks the FMEA last of the three documents, and that ranking is right for building a casebook: an FMEA
# has no Safeguards column, so none of the protection-layer questions can even be asked of it. It was wrong as a
# general claim. An FMEA states S, O, D and an RPN, so like a LOPA it asserts a conclusion and shows its working,
# and a machine can redo the working and disagree.
#
# The rules checked here are arithmetic and a published lookup table, not a reading of English -- which is why
# constructed fixtures are acceptable here in a way they would not be for a HAZOP.
set -uo pipefail
FMEA2CB="$1"
D="$(mktemp -d)"
F="$(dirname "$0")"
trap 'rm -rf "$D"' EXIT

want() { if ! grep -qE "$2" "$D/$1"; then echo "FAIL [$1]: $3"; echo "       expected: $2";
         sed 's/^/       /' "$D/$1"; exit 1; fi }

# The physical line a row occupies, looked up in the fixture rather than written down here.
#
# Three separate line-number defects in this project were followed by a fourth kind: assertions that hard-coded a
# number, which then chased the fixture every time its comment header was edited. Deriving the expected line from the
# file tests the property that matters -- the report sends the reader to the row it is talking about -- instead of
# testing a magic number that happened to be true when it was written.
line_of() { grep -n "$2" "$F/$1.csv" | head -1 | cut -d: -f1; }
deny() { if grep -qE "$2" "$D/$1"; then echo "FAIL [$1]: $3"; echo "       must not match: $2"; exit 1; fi }

"$FMEA2CB" "$F/fmea_scored.csv" scored --report "$D/scored.md" > /dev/null 2> "$D/scored.txt"

# ── 1. RPN ARITHMETIC ─────────────────────────────────────────────────────────
#
# The row states 100; 8 x 3 x 5 is 120. Definitional, and the kind of defect nobody catches by eye because a wrong
# cell looks exactly like a right one.
L_PUMP="$(line_of fmea_scored 'Coolant pump')"
want scored.txt "RPN ARITHMETIC line $L_PUMP: 8 x 3 x 5 = 120, but the row states 100" \
    "a stated RPN that is not the product of its own ratings must be caught, at its line in the file"

# ── 2. RANK INVERSION: the documented reason the standard changed ─────────────
#
# Line 24 is a brake failure: S=10 O=2 D=7, RPN 140, Action Priority HIGH.
# Line 25 is a cosmetic blemish: S=3 O=8 D=8, RPN 192, Action Priority MEDIUM.
#
# RPN ranks the blemish above the brake. Any team sorting by RPN, or applying the RPN >= 100 cutoff used in
# published studies, works on the trim panel first. This is why AIAG-VDA 2019 replaced RPN with Action Priority.
L_TRIM="$(line_of fmea_scored 'Trim panel')"
L_BRAKE="$(line_of fmea_scored 'Brake actuator')"
want scored.txt "RANK INVERSION line $L_TRIM \(RPN 192, AP MEDIUM\) outranks line $L_BRAKE \(RPN 140, AP HIGH\)" \
    "a higher-RPN lower-priority row must be reported against the row it outranks"

# ── 3. NO ACTION on an Action Priority HIGH row ───────────────────────────────
want scored.txt "NO ACTION      line $L_BRAKE: Action Priority HIGH with no recommended action \(RPN 140\)" \
    "an AP HIGH row with a blank action must be reported, and its RPN shown"
# Line 27 is S=2 O=2 D=2, Action Priority LOW, action also blank. That is correct and must stay silent: the standard
# does not require an action on a low-priority item, and reporting one would train the reader to ignore this check.
L_BADGE="$(line_of fmea_scored 'Badge')"
deny scored.txt "line $L_BADGE" "a LOW priority row with no action is not a finding"

# ── 4. THE REPORT ─────────────────────────────────────────────────────────────
WANT="$(sha256sum "$F/fmea_scored.csv" | cut -d' ' -f1)"
want scored.md "$WANT" "the digest must match sha256sum"
want scored.md "AIAG & VDA FMEA Handbook \(2019\)" \
    "where the review disagrees with an RPN ranking it is the standard disagreeing, and must say so"
want scored.md "## The document contradicts itself" "a wrong RPN is the document contradicting itself"
want scored.md "of [0-9]+ stated RPN\(s\) equal S x O x D" "credit the rows whose arithmetic is right"
want scored.md "Action Priority HIGH under AIAG-VDA 2019" "state how many rows the standard calls HIGH"
want scored.md "^\*\*Where:\*\* line $L_BRAKE " "findings must cite the physical line in the file"
# An FMEA cannot be asked the protection-layer questions at all, and a reader must not have to infer that.
want scored.md "no Safeguards column" "the report must state why an FMEA is the weakest of the three documents"
for k in "RPN ARITHMETIC" "RANK INVERSION" "NO ACTION" "DETECTION AS SCORE"; do
    want scored.md "\| $k \|" "check '$k' ran and must be listed whether it fired or not"
done
# A well-formed worksheet -- both a Detection Method and a D rating -- gets an audit AND a casebook.
deny scored.txt "holds SCORES" "fmea_scored.csv names real detection methods and must not be refused"

# ── 5. THE CLEAN STUDY: silent, but still a useful report ─────────────────────
"$FMEA2CB" "$F/fmea_scored_clean.csv" clean --report "$D/clean.md" > /dev/null 2> "$D/clean.txt"
want clean.md "\| findings \| 0 \|" "the clean study must produce no findings"
deny clean.txt "RPN ARITHMETIC" "every stated RPN in the clean study equals its product"
deny clean.txt "RANK INVERSION" "RPN order and Action Priority order agree in the clean study"
deny clean.txt "NO ACTION" "the one AP HIGH row in the clean study carries an action"
want clean.md "## What was NOT checked" "a clean report must still state its limits"
if [ "$(grep -c '| nothing |' "$D/clean.md")" != "4" ]; then
    echo "FAIL: on a clean scored FMEA all four checks must be listed as having found nothing"
    grep '|' "$D/clean.md" | sed 's/^/       /'
    exit 1
fi

# ── 6. A SCORED FMEA IS AUDITED EVEN THOUGH ITS CASEBOOK IS REFUSED ───────────
#
# A Detection column holding ratings makes an FMEA useless for observations -- you cannot take a reading called
# "7" -- and simultaneously makes it auditable, because those numbers are what the arithmetic needs. Earlier
# versions refused the file outright and threw the audit away with it.
# fmea_scored.csv has BOTH a Detection Method and a D rating, which is a well-formed worksheet: it gets an audit AND
# a casebook. fmea_ratings_only.csv has only the rating, and the two outcomes must point in opposite directions.
"$FMEA2CB" "$F/fmea_ratings_only.csv" ro --report "$D/ro.md" > "$D/ro.cb" 2> "$D/ro.txt"
want ro.txt "holds SCORES, not detection methods" "a rating-only Detection column must be refused a casebook"
if [ -s "$D/ro.cb" ]; then
    echo "FAIL: no casebook may be built from ratings; an observation called '7' is not a reading anyone can take"
    sed 's/^/       /' "$D/ro.cb"; exit 1
fi
# The audit of the SAME file must still have happened. Line 19 is an airbag squib failing to fire: S=10 O=4 D=2,
# Action Priority HIGH, RPN 80. A study applying the RPN >= 100 threshold that appears in published FMEAs would have
# filtered out the highest-priority row it contains.
L_AIRBAG="$(line_of fmea_ratings_only 'Airbag squib')"
L_FUEL="$(line_of fmea_ratings_only 'Fuel line')"
want ro.txt "NO ACTION      line $L_AIRBAG: Action Priority HIGH with no recommended action \(RPN 80\)" \
    "the audit must run even though the casebook is refused -- the ratings are what the arithmetic needs"
# And the inversion in its starkest form: a fuel union weeping (RPN 162) outranks an airbag failing to fire in a
# crash (RPN 80), by RPN. Action Priority reverses it.
want ro.txt "RANK INVERSION line $L_FUEL \(RPN 162, AP MEDIUM\) outranks line $L_AIRBAG \(RPN 80, AP HIGH\)" \
    "RPN ranking a fuel weep above an airbag failure is the defect the standard was changed to fix"
want ro.md "DETECTION AS SCORE" "the refusal itself belongs in the report as a finding"
if ! "$FMEA2CB" "$F/fmea_ratings_only.csv" ro --report "$D/x.md" >/dev/null 2>&1; then
    echo "FAIL: with --report the audit succeeded and the exit status must say so"
    exit 1
fi
# Without --report there is no audit to deliver, so the refusal is the whole result and must be a failure.
if "$FMEA2CB" "$F/fmea_ratings_only.csv" ro >/dev/null 2>&1; then
    echo "FAIL: without --report, a rating-only Detection column must be a hard refusal"
    exit 1
fi

# ── 7. THE MULTI-LINE CELL, which was broken here long after it was fixed elsewhere ──
#
# A quoted cell containing a bulleted list contains NEWLINES. This importer kept its own std::getline after the
# other two were fixed, so the second cause was lost AND every later column shifted -- the Detection Method became
# empty, the observation was named "undetected_abnormal", and the real instrument was dropped.
printf 'Item,Failure Mode,Effect,Cause,Severity,Detection Method\nPump,"- Seal leak\n- Shaft wear",Loss of containment,Vibration,8,Bearing RTD\n' > "$D/ml.csv"
"$FMEA2CB" "$D/ml.csv" ml > "$D/ml.cb" 2>/dev/null
if ! grep -q "bearing_rtd_abnormal" "$D/ml.cb"; then
    echo "FAIL: a multi-line quoted cell shifted the columns; Bearing RTD was lost"
    sed 's/^/       /' "$D/ml.cb"; exit 1
fi
if ! grep -q "Seal leak; - Shaft wear" "$D/ml.cb"; then
    echo "FAIL: both causes must survive, flattened onto one line"
    grep "^claim" "$D/ml.cb" | sed 's/^/       /'; exit 1
fi
# A cell may now contain a newline and a .cb string may not, so the emitted casebook must still parse as one line
# per claim. Fixing the read must not trade a wrong casebook for an unparseable one.
if grep -cE '^claim ' "$D/ml.cb" | grep -qv '^1$'; then
    echo "FAIL: expected exactly one claim from one row"; exit 1
fi

echo "OK: RPN arithmetic, rank inversion against AIAG-VDA Action Priority, and an unactioned HIGH row are all"
echo "    reported at their physical lines; the clean study is silent on all four checks; a rating-only"
echo "    Detection column is audited but refused a casebook; multi-line cells no longer shift columns"
