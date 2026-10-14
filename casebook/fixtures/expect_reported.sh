#!/bin/bash
# Assert that cbc REPORTS a defect it is not supposed to reject.
#
# The companion to expect_rejected.sh. Some defects are not grounds for refusing to compile: an inert claim produces a
# casebook that behaves correctly, and an author may have a reason. But the report must still say so, in words an author
# can act on -- and a diagnostic nobody tests is a diagnostic that quietly stops working.
#
# Usage: expect_reported.sh <cbc> <casebook> <text that must appear>
set -u
CBC="$1"
CASEBOOK="$2"
EXPECT="$3"

OUT=$("$CBC" "$CASEBOOK" 2>&1)
STATUS=$?

if [ "$STATUS" -ne 0 ]; then
    echo "FAIL: cbc rejected '$CASEBOOK' (exit $STATUS); this fixture is meant to compile and be reported on"
    echo "$OUT" | tail -10
    exit 1
fi

if ! echo "$OUT" | grep -q "$EXPECT"; then
    echo "FAIL: cbc compiled '$CASEBOOK' without reporting '$EXPECT'"
    echo "$OUT" | tail -20
    exit 1
fi

echo "OK: reported '$EXPECT'"
exit 0
