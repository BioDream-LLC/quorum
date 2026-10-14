#!/bin/bash
# Assert the review report is a deliverable, not a debug dump.
#
# WHY EACH ASSERTION IS HERE
#
# A report is the first artefact of this project a customer sees without a terminal, and every claim it makes about
# itself is a claim that can be wrong. Each one below is checked because an unchecked claim in a document handed to
# a client is worse than no document: it invites them to conclude the tool is careless about the things they cannot
# verify either.
set -uo pipefail
LOPA2CB="$1"
D="$(mktemp -d)"
F="$(dirname "$0")"
trap 'rm -rf "$D"' EXIT

DOC="$F/lopa_biocontainment.csv"

# ── 1. DETERMINISM, which the report asserts about itself in its last paragraph.
# The FINDINGS must be identical run to run. The file is not byte-identical, because the reproduction command
# names wherever the output was sent -- and the first version of this report claimed the stronger property, which
# this assertion caught by taking it at its word.
"$LOPA2CB" "$DOC" bio --report "$D/a.md" >/dev/null 2>&1
"$LOPA2CB" "$DOC" bio --report "$D/b.md" >/dev/null 2>&1
if ! diff <(grep -v "$D" "$D/a.md") <(grep -v "$D" "$D/b.md") >/dev/null; then
    echo "FAIL: the report claims its findings are deterministic and they are not"
    diff "$D/a.md" "$D/b.md" | head -20 | sed 's/^/       /'
    exit 1
fi
# And the overclaim must not come back.
if grep -q "the same worksheet gives the same report" "$D/a.md"; then
    echo "FAIL: the report overclaims byte-identical output; it embeds its own output path"
    exit 1
fi

# ── 2. THE DIGEST, which the report offers as a way to match a report to a revision.
#
# If this disagrees with sha256sum the promise is empty, and a customer who checks it -- exactly the diligent
# customer worth having -- concludes the tool is wrong about the one thing they could verify unaided.
WANT="$(sha256sum "$DOC" | cut -d' ' -f1)"
if ! grep -q "$WANT" "$D/a.md"; then
    echo "FAIL: the SHA-256 in the report does not match sha256sum"
    echo "       sha256sum says: $WANT"
    grep -i "sha-256" "$D/a.md" | sed 's/^/       /'
    exit 1
fi

# Same digest must appear on stderr, so a report can be identified without opening it.
"$LOPA2CB" "$DOC" bio --report "$D/c.md" 2> "$D/c.txt" >/dev/null
if ! grep -q "${WANT:0:16}" "$D/c.txt"; then
    echo "FAIL: the digest printed to the terminal does not match the document"
    sed 's/^/       /' "$D/c.txt"
    exit 1
fi

# ── 3. THE REPRODUCTION COMMAND MUST RUN.
#
# A command a reader cannot paste is worse than none. The first version of this report omitted the <name> argument
# entirely, so following the instructions produced a usage error.
CMD="$(awk '/## Reproducing this/{f=1} f&&/^lopa2cb /{print;exit}' "$D/a.md")"
if [ -z "$CMD" ]; then
    echo "FAIL: the report contains no reproduction command"
    exit 1
fi
# shellcheck disable=SC2086
if ! ( cd "$F/../.." && ${CMD/#lopa2cb/$LOPA2CB} >/dev/null 2>&1 ); then
    echo "FAIL: the reproduction command printed in the report does not run"
    echo "       $CMD"
    exit 1
fi

# ── 4. EVERY FINDING MUST BE LOCATABLE IN THE CUSTOMER'S FILE.
#
# A finding with no line is untraceable, and an untraceable finding cannot be acted on or argued with. Findings
# about the document as a whole say so explicitly rather than defaulting to line 0.
if grep -qE "^\*\*Where:\*\* line 0 " "$D/a.md"; then
    echo "FAIL: a finding reports line 0; say 'the document as a whole' instead"
    exit 1
fi
if ! grep -qE "^\*\*Where:\*\* line [1-9]" "$D/a.md"; then
    echo "FAIL: no finding is located to a real line in the document"
    exit 1
fi
# The line must be the PHYSICAL line in the file. A report that says "line 3" for a row at line 51 sends the reader
# to the wrong place, and the reader checks that before anything else. Verified against the fixture with grep -n.
L_OP="$(grep -n 'Operator closes valve' "$DOC" | head -1 | cut -d: -f1)"
if ! grep -q "^\*\*Where:\*\* line $L_OP " "$D/a.md"; then
    echo "FAIL: the operator-error row is physically at line $L_OP; the report must say so"
    grep "^\*\*Where:" "$D/a.md" | sed 's/^/       /'
    exit 1
fi

# ── 5. SILENCE MUST BE VISIBLE.
#
# This is the section most tools omit and the one that makes the rest mean anything: a report naming six findings
# could be a thorough audit or a tool that only knows six things. Every check must be listed whether it fired or
# not, and on this document four of the seven find nothing.
if ! grep -q "## Checks that found nothing" "$D/a.md"; then
    echo "FAIL: the report does not list the checks that found nothing"
    exit 1
fi
for k in ARITHMETIC TARGET "NOT A PROTECTION" "NOT INDEPENDENT" "SHARED LAYER" "CREDIT LIMIT" "UNIFORM PFD"; do
    if ! grep -q "| $k |" "$D/a.md"; then
        echo "FAIL: check '$k' ran but is absent from the checks table"
        exit 1
    fi
done
SILENT="$(grep -c '| nothing |' "$D/a.md")"
if [ "$SILENT" -lt 4 ]; then
    echo "FAIL: expected at least 4 checks to report nothing on this document, got $SILENT"
    exit 1
fi

# ── 6. LIMITS MUST BE STATED.
if ! grep -q "## What was NOT checked" "$D/a.md"; then
    echo "FAIL: the report does not state what it did not check"
    exit 1
fi
if ! grep -q "fragment of one CCPS criterion" "$D/a.md"; then
    echo "FAIL: the independence limit must be stated; silence there is misleading by omission"
    exit 1
fi

# ── 7. A CORRECT DOCUMENT MUST NOT BE CALLED SELF-CONTRADICTORY.
#
# The biocontainment study's arithmetic is right in every row, and it states plainly that its target is not met.
# The first version of this report filed that under "The document contradicts itself", which is the same failure as
# PROC-D13 moved into the presentation layer: dressing a correct document's own conclusion up as its error.
if grep -q "## The document contradicts itself" "$D/a.md"; then
    echo "FAIL: this document's arithmetic is correct in every row and must not be reported as contradictory"
    exit 1
fi
if ! grep -q "## The study's own conclusion" "$D/a.md"; then
    echo "FAIL: a target that is not met is the study's own conclusion and must be presented as such"
    exit 1
fi
if ! grep -q "## What this review established" "$D/a.md"; then
    echo "FAIL: the report must state what it recomputed and agreed with, not only what it questions"
    exit 1
fi
if ! grep -q "4 of 4 row(s) reproduce their own stated answer" "$D/a.md"; then
    echo "FAIL: all four rows of this study are arithmetically correct and the report must say so"
    grep -i "row arithmetic" "$D/a.md" | sed 's/^/       /'
    exit 1
fi

# ── 8. A CLEAN DOCUMENT MUST STILL PRODUCE A USEFUL REPORT.
#
# Zero findings is the best possible outcome and the easiest to render uselessly. The report must still name every
# check that ran, because "we found nothing" is worth nothing without "here is what we looked for".
"$LOPA2CB" "$F/lopa_clean.csv" clean --report "$D/clean.md" >/dev/null 2>&1
if ! grep -q "| findings | 0 |" "$D/clean.md"; then
    echo "FAIL: the clean study must report zero findings"
    grep -i "findings" "$D/clean.md" | head -3 | sed 's/^/       /'
    exit 1
fi
if [ "$(grep -c '| nothing |' "$D/clean.md")" != "7" ]; then
    echo "FAIL: on a clean study all seven checks must be listed as having found nothing"
    grep '|' "$D/clean.md" | sed 's/^/       /'
    exit 1
fi
if ! grep -q "## What was NOT checked" "$D/clean.md"; then
    echo "FAIL: a clean report must still state its limits -- that is when they matter most"
    exit 1
fi
if grep -qE "^## (The document contradicts itself|The conclusion depends)" "$D/clean.md"; then
    echo "FAIL: a clean report must not print empty finding sections"
    exit 1
fi

# ── 9. A WRONG EXPONENT MUST BE PRESENTED AS WHAT IT IS.
"$LOPA2CB" "$F/lopa_arithmetic_error.csv" err --report "$D/err.md" >/dev/null 2>&1
if ! grep -q "## The document contradicts itself" "$D/err.md"; then
    echo "FAIL: a row whose own numbers contradict its own answer belongs under 'contradicts itself'"
    exit 1
fi
if ! grep -q "out by a factor of" "$D/err.md"; then
    echo "FAIL: the report must show the computed and stated values together, not just assert a defect"
    exit 1
fi

echo "OK: report is deterministic, its SHA-256 matches sha256sum, its reproduction command runs, every"
echo "    finding is locatable, all 7 checks are listed including the silent ones, limits are stated, and"
echo "    a correct study is presented as correct (4 of 4 rows confirmed) rather than as contradictory"
