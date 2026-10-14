#!/bin/bash
# Assert what lopa2cb finds, and just as importantly what it does not.
#
# A LOPA states its own arithmetic, which makes it the strongest document this project can read: a finding derived
# from the document's own numbers cannot be wrong because a phrase was misread. That is the whole reason this
# importer exists, so the first assertion below is not that the tool finds something -- it is that the tool
# CONFIRMS a correct published document and stays silent on it.
set -uo pipefail
LOPA2CB="$1"
CBC="$2"
D="$(mktemp -d)"
F="$(dirname "$0")"
trap 'rm -rf "$D"' EXIT

run() { "$LOPA2CB" "$F/$1.csv" "$1" > "$D/$1.cb" 2> "$D/$1.txt"; }
want() {  # file, pattern, why
    if ! grep -qE "$2" "$D/$1.txt"; then
        echo "FAIL [$1]: $3"
        echo "       expected to match: $2"
        sed 's/^/       /' "$D/$1.txt"
        exit 1
    fi
}
deny() {
    if grep -qE "$2" "$D/$1.txt"; then
        echo "FAIL [$1]: $3"
        echo "       must NOT match: $2"
        sed 's/^/       /' "$D/$1.txt"
        exit 1
    fi
}

# ── 1. THE REAL PUBLISHED STUDY, which is CORRECT ─────────────────────────────
#
# Harsono, Waters, Tearle & Harkess, Sci Rep 15, 4428 (2025), CC BY 4.0. Its arithmetic is right in every row, so
# the tool must independently reproduce the paper's own conclusions and report no arithmetic defect. Accusing a
# correct document of an error is the worst thing this tool can do: it spends the credibility every true finding
# depends on. The first transcription of that fixture DID produce a false accusation, and the paper's own totals
# are what settled it.
run lopa_biocontainment
want lopa_biocontainment "0 arithmetic" "the paper's arithmetic is correct in every row and must not be questioned"
want lopa_biocontainment "recomputed: 1\.4e-05" "the recomputed total must equal the paper's stated 1.40E-05"
want lopa_biocontainment "PFD of 0\.0714" "the required PFD must equal the paper's stated 7.14E-02"
want lopa_biocontainment "SIL 1" "the paper concludes a SIL 1 function is required; the tool must agree"
want lopa_biocontainment "TARGET NOT MET" "the paper states its target is not met, so this must be reported"
# D is declared a conditional modifier in that fixture, so it must NOT be reported as a mis-marked protection.
want lopa_biocontainment "0 not a protection" "a correctly declared conditional modifier is not a finding"
# The two independence candidates: human error protected by a human procedure, and a control-system failure
# protected by something that annunciates through the control room.
want lopa_biocontainment "shared: 'a person'" "human error credited against a procedure carried out by people"
want lopa_biocontainment "shared: 'the control system'" \
    "a control system failure credited against a layer that notifies the control room"
want lopa_biocontainment "2 not independent" "exactly two independence candidates, not four"
# Physical lines, verified against the file: the operator row is at 51 and the control-system row at 52.
L_OP="$(grep -n 'Operator closes valve' "$F/lopa_biocontainment.csv" | head -1 | cut -d: -f1)"
L_CS="$(grep -n 'Control system failures' "$F/lopa_biocontainment.csv" | head -1 | cut -d: -f1)"
want lopa_biocontainment "NOT INDEPENDENT line $L_OP" "the operator-error row must be cited at its line in the file"
want lopa_biocontainment "NOT INDEPENDENT line $L_CS" "the control-system row must be cited at its line in the file"
# The false positives that an earlier version produced, twice, on this document. A level switch inside a drain is
# not defeated by that drain blocking -- detecting it is the switch's job.
deny lopa_biocontainment "shared: 'the drainage network'" \
    "a layer that MEASURES a system is not dependent on it; only support systems it needs to FUNCTION count"

# ── 2. THE CONTROL: silent on everything ──────────────────────────────────────
run lopa_clean
want lopa_clean "0 arithmetic, 0 not a protection, 0 not independent, 0 shared layer" \
    "a clean study must produce no findings at all"
want lopa_clean "meets the stated target" "the clean study's total is below its target"
deny lopa_clean "CREDIT LIMIT" "the SIL 2 layer claims 0.001 legitimately BECAUSE it declares a SIL"
deny lopa_clean "UNIFORM PFD" "three distinct PFD values is not a blanket default"
deny lopa_clean "NO TARGET" "the clean study states a target"

# ── 3. ONE DIGIT WRONG ────────────────────────────────────────────────────────
run lopa_arithmetic_error
# The PHYSICAL line in the file, which is line 10 -- not the third data row. An earlier version of this assertion
# expected "line 3" and passed, because the tool counted data rows rather than lines and the fixture's comment header
# was invisible to both. The report told a reader to look at line 3 of a file whose row was at line 10, and the test
# certified it. Line numbers are the one thing in a review a customer checks first.
L_OVERFILL="$(grep -n 'Overfilling during batch' "$F/lopa_arithmetic_error.csv" | head -1 | cut -d: -f1)"
want lopa_arithmetic_error "ARITHMETIC +line $L_OVERFILL" \
    "a wrong exponent must be reported at its line IN THE FILE"
want lopa_arithmetic_error "factor of 10" "the discrepancy is exactly one order of magnitude"
want lopa_arithmetic_error "1 arithmetic" "exactly one row is wrong; the other two must not be flagged"

# ── 4. MORE CREDIT THAN THE STANDARDS ALLOW ───────────────────────────────────
run lopa_over_credit
want lopa_over_credit "CREDIT LIMIT +'BPCS control loop" "IEC 61511 caps an uncertified BPCS at one credit, PFD 0.1"
want lopa_over_credit "CREDIT LIMIT +'Operator responds" "0.001 for a human response during an upset"
want lopa_over_credit "IEC 61511" "the reason must cite the standard, not assert a preference"

# ── 5. A CONDITIONAL MODIFIER COUNTED AS PROTECTION ───────────────────────────
run lopa_cm_as_ipl
want lopa_cm_as_ipl "NOT A PROTECTION" "a probability-of-infection column marked IPL inflates the layer count"
want lopa_cm_as_ipl "1 not a protection" "exactly the one mis-marked column"
# The arithmetic is identical whether D is called an IPL or a CM, which is why nobody notices. The tool must not
# pretend otherwise by reporting an arithmetic difference that does not exist.
want lopa_cm_as_ipl "0 arithmetic" "marking a CM as an IPL changes no number, only what the document claims"

# ── 6. THE DRAFT MUST BE REFUSED BY cbc, BY DESIGN ────────────────────────────
#
# A LOPA records what protects against a hazard and how reliable each layer is. It does not record why a conclusion
# might not follow, which is what a casebook reasons about. So the draft has no defeaters and cbc must reject it --
# the same contract the HAZOP and FMEA drafts are held to.
if "$CBC" "$D/lopa_biocontainment.cb" > "$D/cbc.txt" 2>&1; then
    echo "FAIL: cbc accepted a LOPA draft. It has no defeaters and must be refused."
    sed 's/^/       /' "$D/cbc.txt"
    exit 1
fi
if ! grep -qiE "argued against|defeasible|needs at least one action" "$D/cbc.txt"; then
    echo "FAIL: cbc refused the draft but not for a reason only an engineer can fix"
    sed 's/^/       /' "$D/cbc.txt"
    exit 1
fi
# The draft is incomplete in two ways -- no runtime action, and no defeaters -- and cbc reports the missing action
# first because it cannot validate anything without one. Either message is the right kind of refusal: both name a
# thing a LOPA does not contain and no tool can infer. Asserting only on the defeasibility message would have made
# this test depend on which check cbc happens to run first, which is not a property worth pinning.

# A PFD must never reach the casebook. It is a long-run reliability figure; a casebook reasons about the state the
# plant is in right now, which is whether a layer is AVAILABLE. Carrying 0.1 in as if it meant something at runtime
# would be a category error, and severity is the only numeric field cbc has -- so a stray PFD would land there.
if grep -qE "severity +0?\.[0-9]" "$D/lopa_biocontainment.cb"; then
    echo "FAIL: a PFD leaked into the casebook as a severity. A reliability figure is not a severity."
    grep -nE "severity" "$D/lopa_biocontainment.cb" | sed 's/^/       /'
    exit 1
fi

echo "OK: lopa2cb confirms a correct published study (total 1.4e-05, PFD 7.14e-02, SIL 1) and reports"
echo "    2 independence candidates on it; catches a wrong exponent, two over-credited layers and a"
echo "    conditional modifier counted as protection; stays silent on a clean study; draft refused by cbc"
