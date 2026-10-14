#!/bin/bash
# Does the casebook fingerprint actually detect a modified header?
#
# This test exists because of a defect found earlier in this project: a duplicate-`idle` check that compared actions
# against a single stored index and could therefore never fire. It passed, it looked like verification, and it verified
# nothing. An integrity check is the easiest place in a codebase to make that mistake, because the happy path always
# agrees with itself.
#
# So this does not check that a correct header verifies. It takes the REAL generated header, changes one number in the
# attack table, compiles it, and requires the check to fail. Then it changes a capacity constant instead, and requires
# the same. If either mutation goes undetected, the fingerprint is decoration.
set -u

GEN="$1"        # path to a generated casebook header
NAMESPACE="$2"  # e.g. quorum::casebook::gen::centrifugal_pump
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

INC_SOLVER="$3"
INC_ARBITER="$4"
INC_BINDING="$5"

cat > "$WORK/probe.cpp" <<EOF
#include <cstdio>
#include "casebook.hpp"
int main() {
    std::printf("%u %u\n", ${NAMESPACE}::computed_fingerprint(), ${NAMESPACE}::kCasebookFingerprint);
    return ${NAMESPACE}::verify_casebook() ? 0 : 1;
}
EOF

build_and_run() {
    local label="$1"
    if ! g++ -std=c++17 -O1 -I"$WORK" -I"$INC_SOLVER" -I"$INC_ARBITER" -I"$INC_BINDING" \
             "$WORK/probe.cpp" -o "$WORK/probe" 2>"$WORK/err"; then
        echo "FAIL: $label did not compile"
        head -5 "$WORK/err"
        exit 1
    fi
    "$WORK/probe" >/dev/null 2>&1
    return $?
}

# ── 1. the unmodified header must verify ────────────────────────────────────
cp "$GEN" "$WORK/casebook.hpp"
if build_and_run "unmodified header"; then
    echo "OK:   the generated header verifies against its own fingerprint"
else
    echo "FAIL: a freshly generated header does not verify -- the emitted and recomputed"
    echo "      fingerprints disagree, so one of the two implementations is wrong"
    exit 1
fi

# ── 2. one edge redirected ──────────────────────────────────────────────────
# The realistic corruption and the realistic hand-edit: an attack pointing somewhere else. Rewires the framework and
# changes what the device concludes, while the file still looks entirely plausible.
cp "$GEN" "$WORK/casebook.hpp"
python3 - "$WORK/casebook.hpp" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path).read()
# First entry of kCasebook: change the target. Deliberately a small, plausible edit.
start = text.index('kCasebook[] = {')
m = re.search(r'\{(\d+),\s*(\d+)\}', text[start:])
old = m.group(0)
new = '{%s, %d}' % (m.group(1), int(m.group(2)) + 1)
text = text[:start] + text[start:].replace(old, new, 1)
open(path, 'w').write(text)
print("      mutated %s -> %s" % (old, new))
PY
if build_and_run "edge-mutated header"; then
    echo "FAIL: an attack edge was redirected and verify_casebook() still returned true."
    echo "      The fingerprint does not cover the attack table, so it is decoration."
    exit 1
else
    echo "OK:   a single redirected attack edge is detected"
fi

# ── 3. a capacity constant changed ──────────────────────────────────────────
# Subtler and worse: capacities decide whether add_attack is refused, so shrinking one silently changes behaviour at the
# margins without touching a single edge.
cp "$GEN" "$WORK/casebook.hpp"
python3 - "$WORK/casebook.hpp" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path).read()
m = re.search(r'(kMaxAttacks\s*=\s*)(\d+)', text)
if not m:
    sys.exit("could not find kMaxAttacks")
new = m.group(1) + str(int(m.group(2)) + 1)
text = text[:m.start()] + new + text[m.end():]
open(path, 'w').write(text)
print("      mutated kMaxAttacks %s -> %d" % (m.group(2), int(m.group(2)) + 1))
PY
if build_and_run "capacity-mutated header"; then
    echo "FAIL: a capacity constant was changed and verify_casebook() still returned true."
    echo "      Capacities decide whether an attack is refused, so they must be covered."
    exit 1
else
    echo "OK:   a changed capacity constant is detected"
fi

echo "the fingerprint covers the attack table and the capacities, and is not vacuous"
exit 0
