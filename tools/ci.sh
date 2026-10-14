#!/bin/bash
# Everything that must hold before a commit. One entry point, so nothing rots quietly.
#
# The firmware and the board matrix are the parts at risk: nothing in a host build touches them, so a change to a
# generated header or an engine signature can break the Cortex-M targets and every test will still pass. That is how
# `firmware/` broke silently before, and it is why target compilation is a gate here rather than something done by hand
# when someone remembers.
#
# Usage:  tools/ci.sh            everything (needs docker for the target gates)
#         tools/ci.sh --host     host build and tests only
set -uo pipefail
cd "$(dirname "$0")/.."

HOST_ONLY=0
[ "${1:-}" = "--host" ] && HOST_ONLY=1

PASS=0
FAIL=0
SKIP=0

step() {
    local name="$1"
    shift
    printf '%-46s ' "$name"
    local out
    if out=$("$@" 2>&1); then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        FAIL=$((FAIL + 1))
        echo "$out" | tail -15 | sed 's/^/      /'
    fi
}

skip() {
    printf '%-46s SKIP  (%s)\n' "$1" "$2"
    SKIP=$((SKIP + 1))
}

echo "quorum CI"
echo "========="
echo

# ── host ────────────────────────────────────────────────────────────────────
step "configure" cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
step "build (warnings are errors)" cmake --build build -j
step "test suite" ctest --test-dir build --output-on-failure

# The safety case is checked in both directions: a claim with no test, or a test no claim references. The second is what
# finds things -- an unstated guarantee is how a safety argument quietly stops describing the product.
step "safety case matches the suite" ./tools/safety-case.sh

# The only check here that is not my code checking my other code. Skips cleanly if CaDiCaL is not present, because it
# depends on a sibling project rather than on anything in this tree.
step "independent check (CaDiCaL, exhaustive)" ./tools/cross-verify-exhaustive.sh

# And the generated document must be current, or it is a document that describes a product that no longer exists.
safety_case_current() {
    cp docs/SAFETY_CASE.md /tmp/qsc_before.md 2>/dev/null || true
    ./tools/safety-case.sh --markdown >/dev/null
    diff -q /tmp/qsc_before.md docs/SAFETY_CASE.md >/dev/null
}
step "SAFETY_CASE.md is current" safety_case_current

if [ "$HOST_ONLY" = 1 ]; then
    echo
    echo "$PASS passed, $FAIL failed, $SKIP skipped"
    exit $((FAIL > 0 ? 1 : 0))
fi

if ! command -v docker >/dev/null 2>&1 || ! docker info >/dev/null 2>&1; then
    echo
    skip "footprint and budget gate" "no docker"
    skip "board support matrix" "no docker"
    skip "pico2 firmware target compile" "no docker"
    skip "arduino sketch target compile" "no docker"
    skip "advisory host-only interlock" "no docker"
    echo
    echo "$PASS passed, $FAIL failed, $SKIP skipped"
    exit $((FAIL > 0 ? 1 : 0))
fi

# ── target ──────────────────────────────────────────────────────────────────
step "footprint and budget gate" ./tools/footprint.sh
step "board support matrix" ./tools/board-matrix.sh

# The firmware is the part a host build cannot protect.
target_compile() {
    docker run --rm -i -v "$PWD:/src" -v "$PWD/build/casebook/compiler/gen:/gen" -w /src quorum-multi:1 bash -s <<'SCRIPT'
set -e
WARN="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror"
FREE="-fno-exceptions -fno-rtti -fno-threadsafe-statics"
INC="-Isolver/include -Iarbiter/include -Iwitness/include -Ibinding/include -Ibench/include -I/gen"

arm-none-eabi-g++ -std=c++17 -mcpu=cortex-m33 -mthumb -Os $FREE $WARN $INC \
  -Ifirmware/pico2/stub -c firmware/pico2/main.cpp -o /tmp/ci_pico.o
SCRIPT
}

arduino_compile() {
    docker run --rm -i -v "$PWD:/src" -v "$PWD/build/casebook/compiler/gen:/gen" -w /src quorum-multi:1 bash -s <<'SCRIPT'
set -e
mkdir -p /tmp/ard
cat > /tmp/ard/Arduino.h <<'EOF'
#pragma once
#include <cstdint>
#ifndef F_CPU
#define F_CPU 48000000UL
#endif
#define F(x) (x)
enum { HEX = 16, DEC = 10 };
struct SerialStub {
    void begin(unsigned long) {}
    explicit operator bool() const { return true; }
    void print(const char*) {}
    void print(char) {}
    void print(unsigned long) {}
    void print(unsigned long, int) {}
    void print(int) {}
    void println() {}
    void println(const char*) {}
    void println(unsigned long) {}
    void println(int) {}
};
extern SerialStub Serial;
inline unsigned long millis() { return 0; }
inline void delay(unsigned long) {}
EOF
WARN="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror"
INC="-I/tmp/ard -Isolver/include -Iarbiter/include -Ibinding/include -Ibench/include -I/gen"
# Every Cortex-M Arduino, so a change cannot break one core while passing on another.
for cpu in cortex-m0plus cortex-m3 cortex-m4 cortex-m7 cortex-m33; do
  arm-none-eabi-g++ -std=c++17 -x c++ -mcpu=$cpu -mthumb -Os \
    -fno-exceptions -fno-rtti -fno-threadsafe-statics $WARN $INC \
    -c firmware/arduino/quorum_wcet/quorum_wcet.ino -o /tmp/ci_ino.o
done
SCRIPT
}

# The advisory tier must REFUSE to build bare-metal. A passing compile here is the failure: it would mean an unbounded
# heap-allocating NP-hard search could be linked into a safety function.
advisory_interlock() {
    docker run --rm -i -v "$PWD:/src" -w /src quorum-multi:1 bash -s <<'SCRIPT'
echo '#include "quorum/advisory/semantics.hpp"' > /tmp/adv.cpp
if arm-none-eabi-g++ -std=c++17 -mcpu=cortex-m33 -mthumb -Os -fno-exceptions -fno-rtti \
     -Isolver/include -Iadvisory/include -c /tmp/adv.cpp -o /tmp/adv.o 2>/dev/null; then
  echo "advisory tier built for bare metal -- the interlock is gone"
  exit 1
fi
exit 0
SCRIPT
}

step "pico2 firmware target compile" target_compile
step "arduino sketch target compile (5 cores)" arduino_compile
step "advisory host-only interlock" advisory_interlock

# Gate 12. A full test sweep found every executable check passing and the README's numbers stale in twenty-six places,
# including the headline footprint figure and -- worse -- a WCET bound the front door UNDER-reported. Everything else here
# is checked against the thing it describes; the product's front door was not.
step "README figures match the tools" "./tools/figures.sh"

# Gate 13. The figures gate guards documents, so its FAILING path is tested. This repo has shipped one vacuous check and one
# check that was verifying a false theorem, and both looked healthy because they passed.
step "figures gate can fail" "./tools/figures-tamper.sh"
step "assumptions register" "./tools/assumptions.sh"
step "portable Arduino sketch" "./tools/arduino-portable-check.sh"

echo
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -gt 0 ] && exit 1
echo
# Read from the safety case rather than restated here. This line used to say "timing remains the only unmeasured claim",
# which was an overclaim: the safety case records two, and authorship is the larger risk. The document caught its own
# author's summary, which is the point of generating it from data.
echo "Unproven claims (docs/SAFETY_CASE.md):"
awk -F'\t' '$2 == "NONE" && $4 ~ /UNPROVEN/ { print "  " $1 ": " $3 }' docs/claims.tsv
exit 0
