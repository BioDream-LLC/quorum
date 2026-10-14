#!/bin/bash
# An FMEA-derived draft must be REFUSED, not accepted.
#
# The whole point of the importer is that it produces an incomplete artefact. If cbc ever accepts one, the safety net has
# gone: a casebook with no defeaters decides on the absence of objections rather than the presence of evidence, and it
# validates to 100% Decided while doing so.
set -u
FMEA2CB="$1"; CBC="$2"; CSV="$3"
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT

"$FMEA2CB" "$CSV" imported-motor > "$D/draft.cb" 2>/dev/null || { echo "FAIL: importer failed"; exit 1; }

OUT=$("$CBC" "$D/draft.cb" 2>&1); STATUS=$?
if [ "$STATUS" -eq 0 ]; then
    echo "FAIL: cbc ACCEPTED a defeater-free draft. A casebook nothing can argue against is a"
    echo "      decision tree, and accepting one means the argumentation engine is decoration."
    echo "$OUT" | tail -12
    exit 1
fi
echo "$OUT" | grep -q "NOTHING IS DEFEASIBLE" || {
    echo "FAIL: refused, but without saying why. An author needs the reason, not just an exit code."
    echo "$OUT" | tail -10
    exit 1
}
echo "OK: the draft is refused, and the report says nothing in it is defeasible"


# ── a detectability SCORE is not a detection method ──────────────────────────
#
# Added after a real published FMEA (Sci Rep 2025, doi 10.1038/s41598-025-09518-6, 22 mobile X-ray
# machine failure modes) was imported silently and WRONGLY. Its Detection column held AIAG 1-10
# ratings, and the importer read them as method names, producing observations called `3_abnormal`
# described as "3 reports abnormal". It reported success. cbc then accepted those identifiers,
# validated 64 states at 100% Decided, and PASSED the claim-reach check -- emit.cpp prepends an
# underscore to digit-leading names, so the generated C++ was even valid. The only gate that refused
# the casebook was defeasibility, which had itself been added late.
#
# This pins the refusal, because silent plausible output is the defect class this project exists to
# catch, and it was sitting in this project's own importer.
SCORE_CSV="$(dirname "$0")/detection_as_score.csv"
echo
echo "a numeric detection column must be refused, not read as method names"
if "$FMEA2CB" "$SCORE_CSV" imported-score >/dev/null 2>"$D/score.err"; then
    echo "  FAIL  imported an FMEA whose Detection column is a 1-10 score"
    echo "        those are ratings, not instruments, so the observations would be meaningless"
    exit 1
fi
if ! grep -q "holds SCORES, not detection methods" "$D/score.err"; then
    echo "  FAIL  refused, but not for the right reason:"
    sed 's/^/        /' "$D/score.err"
    exit 1
fi
echo "  OK    refused: detection is a score, not a method"

# ── a HAZOP is not an FMEA, and saying so usefully matters ───────────────────
#
# A generic "no Failure Mode column" is useless advice to someone holding a perfectly good HAZOP, and
# a HAZOP is a BETTER input to a casebook than an FMEA: its Deviation column is already an observation,
# and its Safeguards column -- which no FMEA has -- is where a team records that the only protection
# against a deviation is the instrument whose failure causes it. That is the CAPECO accident, written
# down in the document beforehand. quorum's own tank casebook came from a HAZOP.
HAZOP_CSV="$(dirname "$0")/hazop_worksheet.csv"
echo
echo "a HAZOP worksheet must be recognised, not rejected generically"
if "$FMEA2CB" "$HAZOP_CSV" imported-hazop >/dev/null 2>"$D/hazop.err"; then
    echo "  FAIL  imported a HAZOP as though it were an FMEA"
    exit 1
fi
if ! grep -q "looks like a HAZOP worksheet" "$D/hazop.err"; then
    echo "  FAIL  refused, but did not recognise the document type:"
    sed 's/^/        /' "$D/hazop.err"
    exit 1
fi
if ! grep -q "Safeguards" "$D/hazop.err"; then
    echo "  FAIL  recognised the HAZOP but said nothing about the Safeguards column,"
    echo "        which is the column that matters"
    exit 1
fi
echo "  OK    recognised as a HAZOP, with the mapping and the Safeguards warning"

exit 0
