#!/bin/bash
# Assert the HAZOP review report is a deliverable, on the strongest real document this project has.
#
# The hospital oxygen study (PLOS ONE 18(2):e0280918, CC BY) is the document that produced this project's most
# valuable finding and its most embarrassing one. It is used here because a report is only worth testing against a
# document whose right answer is known independently of the tool.
set -uo pipefail
HAZOP2CB="$1"
D="$(mktemp -d)"
F="$(dirname "$0")"
trap 'rm -rf "$D"' EXIT

OXY="$F/hazop_oxygen_psa.csv"
"$HAZOP2CB" "$OXY" oxy --report "$D/oxy.md" >/dev/null 2>&1

want() {
    if ! grep -qE "$2" "$D/$1"; then
        echo "FAIL [$1]: $3"
        echo "       expected: $2"
        exit 1
    fi
}
deny() {
    if grep -qE "$2" "$D/$1"; then
        echo "FAIL [$1]: $3"
        echo "       must not match: $2"
        exit 1
    fi
}

# ── PROVENANCE ────────────────────────────────────────────────────────────────
WANT="$(sha256sum "$OXY" | cut -d' ' -f1)"
want oxy.md "$WANT" "the digest must match sha256sum or the promise to match a report to a revision is empty"
# The transcription warning is not decoration. This exact document was silently re-headed to suit the importer once,
# producing thirteen findings that all vanished under its real column names.
want oxy.md "No column was renamed" "the report must state that nothing was retyped on the way in"

# ── THE FINDING THIS DOCUMENT EXISTS TO DEMONSTRATE ───────────────────────────
#
# Every row of this worksheet records a safeguard, and not one of them is an instrument. Row by row it looks
# protected; in aggregate it is protected by people looking at things. Both halves must appear.
want oxy.md "14 of 14 row\(s\) record some protection" \
    "credit where due: the document does record a safeguard on every row"
want oxy.md "NOT AN IPL" "no row is protected by an instrument, which is the finding"
want oxy.md "SHARED LAYER" "one safeguard carries most of the worksheet -- invisible row by row"
want oxy.md "a person looking" "the report must say in plain words what the protection actually is"

# An earlier version listed "0 of 14 rows name an instrument" under the heading that reads as reassurance. That is
# this document's single worst characteristic filed as a confirmation.
deny oxy.md "^- protection by instrument: 0 of" \
    "the absence of instruments is a finding, not something to list under what the review established"

# ── EVERY FINDING MUST BE LOCATABLE, OR SAY IT IS ABOUT THE WHOLE DOCUMENT ────
deny oxy.md "^\*\*Where:\*\* line 0 " "line 0 is not a location; say 'the document as a whole'"
want oxy.md "^\*\*Where:\*\* line [1-9]" "at least one finding must point at a real row of the customer's file"
want oxy.md "^\*\*What would settle it:\*\*" "a finding with no remedy is a complaint"

# ── SILENCE MUST BE VISIBLE ───────────────────────────────────────────────────
want oxy.md "## Checks that found nothing" "silence is only evidence if the silent checks are named"
for k in UNPROTECTED "COMMON CAUSE" "SHARED LAYER" "NOT AN IPL" "UNPROVEN LAYER" "MITIGATIVE ONLY" \
         "SHARED CAUSE" "PACKED CAUSES"; do
    want oxy.md "\| $k \|" "check '$k' ran and must be listed whether or not it fired"
done
want oxy.md "## What was NOT checked" "a report that hides its limits misleads by omission"
want oxy.md "Whether a safeguard actually WORKS" \
    "the central limit: this reads words in a cell, not the state of a plant"

# ── DETERMINISM AND A WORKING REPRODUCTION COMMAND ────────────────────────────
"$HAZOP2CB" "$OXY" oxy --report "$D/oxy2.md" >/dev/null 2>&1
if ! diff <(grep -v "$D" "$D/oxy.md") <(grep -v "$D" "$D/oxy2.md") >/dev/null; then
    echo "FAIL: the findings are not deterministic between runs"
    diff "$D/oxy.md" "$D/oxy2.md" | head -20 | sed 's/^/       /'
    exit 1
fi
CMD="$(awk '/## Reproducing this/{f=1} f&&/^hazop2cb /{print;exit}' "$D/oxy.md")"
if [ -z "$CMD" ]; then echo "FAIL: no reproduction command in the report"; exit 1; fi
if ! ( cd "$F/../.." && ${CMD/#hazop2cb/$HAZOP2CB} >/dev/null 2>&1 ); then
    echo "FAIL: the reproduction command printed in the report does not run: $CMD"
    exit 1
fi

# ── THE CAPECO CASE: the document says it, years in advance ───────────────────
#
# One row, both findings. The safeguard cell reads "None - LT-101 is the only measurement", so the deviation is
# unprotected AND the only named instrument is the one whose failure causes it. This is the tank overflow written
# into a worksheet before it happened.
"$HAZOP2CB" "$F/hazop_worksheet.csv" cap --report "$D/cap.md" >/dev/null 2>&1
want cap.md "UNPROTECTED 1" "the unprotected deviation must be reported"
want cap.md "COMMON CAUSE 1" "the safeguard naming its own cause must be reported"
want cap.md "both name: +lt-101" "the report must name the shared tag, from the document's own words"
want cap.md "6 of 7 row\(s\) record some protection" "the count must agree with the unprotected finding"
# An unprotected deviation is the document's own content, not a contradiction in it.
want cap.md "## The study's own conclusion" "a cell reading 'None' is what the document says, restated"
deny cap.md "## The document contradicts itself" "a HAZOP has no arithmetic to contradict"

# ── A CLEAN DOCUMENT MUST STILL PRODUCE A USEFUL REPORT ───────────────────────
"$HAZOP2CB" "$F/hazop_independent.csv" ind --report "$D/ind.md" >/dev/null 2>&1
want ind.md "\| findings \| 0 \|" "the control document must produce no findings"
if [ "$(grep -c '| nothing |' "$D/ind.md")" != "8" ]; then
    echo "FAIL: on a clean worksheet all eight checks must be listed as having found nothing"
    grep '|' "$D/ind.md" | sed 's/^/       /'
    exit 1
fi
want ind.md "## What was NOT checked" "a clean report must still state its limits -- that is when they matter most"
deny ind.md "^## The conclusion depends" "a clean report must not print empty finding sections"

echo "OK: HAZOP report traces to a SHA-256, credits 14 of 14 rows for recording protection while reporting"
echo "    that none is an instrument, names all 8 checks including the silent ones, states its limits, and"
echo "    reproduces the CAPECO row as both UNPROTECTED and COMMON CAUSE with the shared tag quoted"
