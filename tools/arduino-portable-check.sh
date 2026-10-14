#!/bin/bash
# Gate 15: the portable Arduino sketch must stage and compile.
#
# WHY THIS GATE EXISTS
#
# `firmware/arduino/stage-portable.sh` produces a self-contained sketch folder to copy to Windows, because WSL2 does
# not pass USB through and the Arduino IDE will not accept arbitrary include paths. It was verified once, by hand.
#
# PROC-D6 in docs/assumptions.tsv recorded the obvious risk: discovering it broken while holding a board is the
# wrong time. Any change to an include, a header name, or a component directory can break staging silently, and
# nothing would notice until someone tried to flash hardware.
#
# WHAT IS CHECKED
#   1. staging succeeds and produces the .ino, the headers, and a provenance snapshot
#   2. the staged folder compiles for cortex-m4 -- the R4 Minima's core -- with ONLY the sketch folder on the
#      include path, which is the property that makes it self-contained
#   3. the snapshot records the casebook fingerprints, so a stale copy can be detected later
#
# The Arduino API is supplied by a throwaway stub. The FIRST attempt at this check failed on `millis`, `F`, `F_CPU`
# and `HEX` -- all missing from the stub, none of them a real defect. That is why the stub lives here in full
# rather than being minimal: an incomplete stub makes this gate fail for the wrong reason.
set -uo pipefail
cd "$(dirname "$0")/.."

if [ ! -f build/casebook/compiler/gen/pump_generated.hpp ]; then
    echo "generated headers missing; run the host build first"
    exit 1
fi

OUT="build/arduino-portable/quorum_wcet"
if ! bash firmware/arduino/stage-portable.sh >/dev/null 2>&1; then
    echo "FAIL  staging failed"
    exit 1
fi

# The sketch holds the .ino; the LIBRARY holds every header. Arduino never puts the sketch folder on the include
# path, so a header that includes another header cannot resolve it from a sketch -- see
# firmware/arduino/RUNNING_ON_HARDWARE.md. A library's src/ is on the path.
LIB="$(dirname "$OUT")/libraries/quorum"
for f in "$OUT/quorum_wcet.ino" "$OUT/SNAPSHOT.txt" "$LIB/library.properties" \
         "$LIB/src/pump_generated.hpp" "$LIB/src/tank_generated.hpp"; do
    if [ ! -f "$f" ]; then
        echo "FAIL  staging is missing $f"
        exit 1
    fi
done

HDRS=$(find "$LIB/src" -name '*.hpp' | wc -l | tr -d ' ')
if [ "$HDRS" -lt 8 ]; then
    echo "FAIL  only $HDRS headers staged; the quorum/ tree did not copy"
    exit 1
fi

# A snapshot with no provenance is how you end up measuring last week's casebook on real hardware.
if ! grep -q "kCasebookFingerprint" "$OUT/SNAPSHOT.txt"; then
    echo "FAIL  SNAPSHOT.txt does not record the casebook fingerprints"
    sed 's/^/      /' "$OUT/SNAPSHOT.txt"
    exit 1
fi

if ! docker image inspect quorum-arm:1 >/dev/null 2>&1; then
    echo "building the pinned toolchain image (once)..."
    docker build -q -f tools/arm.Dockerfile -t quorum-arm:1 tools/ >/dev/null
fi

LOG=$(docker run --rm -v "$PWD/$OUT:/s" -v "$PWD/$LIB/src:/qlib" -w /s quorum-arm:1 bash -c '
cp quorum_wcet.ino main.cpp
cat > Arduino.h <<EOF
#pragma once
#include <cstdint>
#include <cstddef>
// ── HOSTILE ON PURPOSE ──────────────────────────────────────────────────────────────────────────────────
//
// A stub is only hostile in the ways you remember to make it hostile, and this one was not hostile at all
// until a real board rejected the code twice. Both of these are real definitions from the real headers:
//
//   bit      Arduino core, api/Common.h   -- ate a function named binding::bit(), rewriting its own definition
//            into \`std::uint32_t (1UL << (observation))\`
//   SysTick  ARM CMSIS, core_cm4.h        -- ate an enumerator named Source::SysTick
//
// In both cases GCC reported the error inside the vendor header, a long way from the cause. Any new identifier
// colliding with these now fails here instead of on a bench.
#ifndef bit
#define bit(b) (1UL << (b))
#endif
struct SysTick_Type { std::uint32_t CTRL, LOAD, VAL, CALIB; };
#define SysTick ((SysTick_Type *) 0xE000E010UL)
#define DWT ((void *) 0xE0001000UL)
#define NVIC ((void *) 0xE000E100UL)
#define SCB ((void *) 0xE000ED00UL)
#define F(x) (x)
#define F_CPU 48000000UL
enum { HEX = 16, DEC = 10, BIN = 2, OCT = 8 };
struct SerialStub {
  void begin(long) {}
  explicit operator bool() const { return true; }
  bool operator!() { return false; }
  template <class T> void print(T) {}
  template <class T> void print(T, int) {}
  template <class T> void println(T) {}
  template <class T> void println(T, int) {}
  void println() {}
  void flush() {}
};
static SerialStub Serial;
inline void delay(unsigned long) {}
inline unsigned long millis() { return 0; }
inline unsigned long micros() { return 0; }
EOF
arm-none-eabi-g++ -std=c++17 -mcpu=cortex-m4 -mthumb -Os \
  -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
  -I. -I/qlib -c main.cpp -o /tmp/staged.o 2>&1
echo "EXIT:$?"
rm -f main.cpp Arduino.h' 2>&1)

if ! printf '%s' "$LOG" | grep -q "EXIT:0"; then
    echo "FAIL  the staged sketch does not compile for cortex-m4 with only its own folder on the include path."
    echo "      That is the property that makes it self-contained, and the reason it can be opened in the"
    echo "      Arduino IDE on Windows without configuring anything."
    printf '%s\n' "$LOG" | grep -vE "^EXIT:" | head -20 | sed 's/^/      /'
    exit 1
fi

echo "portable Arduino sketch: staged ($HDRS headers), fingerprints recorded, compiles for cortex-m4 with -I. only"
