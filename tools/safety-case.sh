#!/bin/bash
# Check the safety case against reality, in both directions.
#
# docs/claims.tsv lists every claim this product makes and the test that establishes it. Two ways that document can be
# wrong, and the second is the one worth checking:
#
#   1. a claim names a test that does not exist    -> the claim has no evidence
#   2. a test no claim references                  -> either an undocumented guarantee, or a test that establishes
#                                                     nothing anyone cares about
#
# Direction 1 is obvious and would be caught eventually. Direction 2 is not: it is easy to write a document listing the
# claims you remember, and hard to notice the guarantee you verify and never state. That asymmetry is exactly where a
# safety argument goes wrong, so the check is symmetric and both directions are failures.
#
# Usage:  tools/safety-case.sh              check
#         tools/safety-case.sh --markdown   emit docs/SAFETY_CASE.md from the same data
set -uo pipefail
cd "$(dirname "$0")/.."

CLAIMS=docs/claims.tsv
[ -f "$CLAIMS" ] || { echo "missing $CLAIMS"; exit 1; }

if [ ! -d build ]; then
    echo "no build directory; run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
    exit 1
fi

# Registered tests, and the tests claims refer to.
ctest --test-dir build -N 2>/dev/null | sed -n 's/^ *Test *#[0-9]*: *//p' | sort -u > /tmp/qsc_registered
awk -F'\t' 'NR>1 && $1 != "ID" && $1 !~ /^#/ && $2 != "NONE" && $2 != "" { print $2 }' "$CLAIMS" | sort -u > /tmp/qsc_claimed

if [ "${1:-}" = "--markdown" ]; then
    OUT=docs/SAFETY_CASE.md
    {
        echo "# Safety case"
        echo
        echo "**Generated from \`docs/claims.tsv\` by \`tools/safety-case.sh --markdown\`. Do not edit.**"
        echo
        echo "Every claim quorum makes, the test that establishes it, and what that test does not cover. The mapping is"
        echo "checked in both directions: a claim naming a test that does not exist fails the build, and so does a test no"
        echo "claim references. The second direction is the one that finds things -- it is easy to list the claims you"
        echo "remember and hard to notice the guarantee you verify and never state."
        echo
        echo "To audit a row, run its test: \`ctest --test-dir build -R <test>\`."
        echo
        echo "The \`not covered\` column is not a disclaimer. A claim whose limits are unstated is not a weaker claim, it"
        echo "is an unusable one -- an assessor cannot tell what was actually shown."
        echo
        awk -F'\t' '
            NR == 1 { next }
            $1 == "ID" { next }
            $1 ~ /^#/ { next }
            NF < 3 { next }
            {
                if ($2 == "NONE") { unproven[++u] = $0 }
                else { proven[++p] = $0 }
            }
            END {
                print "## Claims with test evidence"
                print ""
                print "| ID | claim | test | not covered |"
                print "|---|---|---|---|"
                for (i = 1; i <= p; i++) {
                    split(proven[i], f, "\t")
                    printf "| `%s` | %s | `%s` | %s |\n", f[1], f[3], f[2], (f[4] == "" ? "--" : f[4])
                }
                print ""
                print "## Claims with no ctest"
                print ""
                print "Some are verified by CI gates that need a cross-compiler rather than by a ctest. Others are"
                print "**unproven and labelled as such** -- which is the point of writing this down."
                print ""
                print "| ID | claim | status |"
                print "|---|---|---|"
                for (i = 1; i <= u; i++) {
                    split(unproven[i], f, "\t")
                    printf "| `%s` | %s | %s |\n", f[1], f[3], (f[4] == "" ? "--" : f[4])
                }
            }
        ' "$CLAIMS"
        echo
        echo "## The two claims that matter most"
        echo
        echo "\`WCET-3\` and \`GEN-1\` are both unproven, and they are the two an assessor should press hardest on."
        echo
        echo "**\`WCET-3\`, measured timing.** There is now a static bound of 2,571 instructions derived from the"
        echo "Cortex-M33 binary, which is the stronger kind of number because it holds for states no test would think to"
        echo "run. What is missing is the memory system: RP2350 executes from QSPI flash through a cache. Hardware is on"
        echo "order; see \`firmware/pico2/RUNNING_ON_HARDWARE.md\`."
        echo
        echo "**\`GEN-1\`, authorship.** All five casebooks were written by quorum's developers, from documents they chose,"
        echo "with outcomes already known. Five casebooks across four problem shapes is real evidence about the"
        echo "*language*. It is no evidence that a reliability engineer can use it. \`casebook/AUTHORING.md\` exists so"
        echo "that can change, and until someone outside BioDream encodes an FMEA its developers have never seen, this remains the largest"
        echo "open risk in the product."
    } > "$OUT"
    echo "wrote $OUT"
fi

FAIL=0

echo "claims with no such test"
MISSING=$(comm -23 /tmp/qsc_claimed /tmp/qsc_registered)
if [ -n "$MISSING" ]; then
    echo "$MISSING" | sed 's/^/  NO SUCH TEST  /'
    FAIL=1
else
    echo "  none -- every claim names a test that exists"
fi

echo
echo "tests no claim references"
ORPHAN=$(comm -13 /tmp/qsc_claimed /tmp/qsc_registered)
if [ -n "$ORPHAN" ]; then
    echo "$ORPHAN" | sed 's/^/  UNCLAIMED     /'
    echo "  Each of these either establishes a guarantee nobody has written down, or establishes"
    echo "  nothing worth stating. Both are worth resolving; add a row to docs/claims.tsv."
    FAIL=1
else
    echo "  none -- every test backs a stated claim"
fi

echo
UNPROVEN=$(awk -F'\t' 'NR>1 && $2 == "NONE" && $4 ~ /UNPROVEN/ { print "  " $1 "  " $3 }' "$CLAIMS")
if [ -n "$UNPROVEN" ]; then
    echo "claims explicitly recorded as UNPROVEN"
    echo "$UNPROVEN"
    echo
fi

TOTAL=$(awk -F'\t' 'NR>1 && $1 != "ID" && $1 !~ /^#/ && NF>=3' "$CLAIMS" | wc -l)
WITH=$(awk -F'\t' 'NR>1 && $1 != "ID" && $1 !~ /^#/ && NF>=3 && $2 != "NONE"' "$CLAIMS" | wc -l)
echo "$TOTAL claims, $WITH with a ctest, $((TOTAL - WITH)) without"

[ "$FAIL" -ne 0 ] && { echo; echo "the safety case does not match the test suite"; exit 1; }
echo "the safety case matches the test suite"
exit 0
