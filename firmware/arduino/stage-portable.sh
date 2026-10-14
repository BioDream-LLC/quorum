#!/bin/bash
# Stage a SELF-CONTAINED Arduino sketch folder that needs no include paths and no WSL.
#
# WHY THIS EXISTS
#
# `build.sh` drives arduino-cli with six include paths, which is correct and is what CI would use. But it needs the
# board visible as /dev/ttyACM0, and **WSL2 does not pass USB through**. Getting a serial port into WSL2 means
# installing usbipd-win on the Windows side, binding the bus ID as Administrator, and re-attaching after every
# unplug and every reboot -- real work, for no benefit, when the only thing we want back from the board is a column
# of numbers over serial.
#
# The Arduino IDE on Windows sees the board immediately. What it will not do is accept arbitrary -I paths. So this
# script copies the headers into the sketch's `src/` folder, preserving the `quorum/...` structure, because the IDE
# puts the sketch folder on the include path and quoted includes resolve inside it.
#
# Output: a folder to copy to Windows, open, and upload. No libraries to install, nothing to configure.
#
# The staged copy is a SNAPSHOT. Re-run this after anything that changes a generated header, or the board will
# faithfully measure a casebook that no longer exists -- which would look exactly like a correct result.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
GEN="$ROOT/build/casebook/compiler/gen"
OUT="${1:-$ROOT/build/arduino-portable/quorum_wcet}"

if [ ! -f "$GEN/pump_generated.hpp" ] || [ ! -f "$GEN/tank_generated.hpp" ]; then
    echo "error: generated headers missing from $GEN" >&2
    echo "       build first:  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j" >&2
    exit 1
fi

rm -rf "$OUT"
# The sketch holds the .ino; the library holds every header. Two directories, because Arduino only adds a library's
# src/ to the include path.
LIB="$(dirname "$OUT")/libraries/quorum"
rm -rf "$LIB"
mkdir -p "$OUT" "$LIB/src"

# The .ino must keep its folder's name or the IDE will not open it.
cp "$ROOT/firmware/arduino/quorum_wcet/quorum_wcet.ino" "$OUT/"

# Headers, with their `quorum/<component>/` structure intact so the existing #includes resolve unchanged.
# Copying rather than rewriting the includes keeps the staged sketch byte-comparable to the real sources.
for c in solver arbiter witness binding bench runtime; do
    if [ -d "$ROOT/$c/include/quorum" ]; then
        # A LIBRARY, not a sketch subfolder. Established by running the real toolchain, having been wrong twice.
        #
        # The Arduino build never puts the sketch folder on the include path -- verified by reading the compiler
        # command line it generates. Quoted includes therefore resolve only relative to the including FILE, so the
        # .ino could find `quorum/arbiter/verdict.hpp` while that header's own `"quorum/solver/framework.hpp"`
        # could not resolve: it was looked for at `quorum/arbiter/quorum/solver/`.
        #
        # A library's `src/` directory IS added to the include path, which is the documented Arduino mechanism for
        # exactly this and the only one that works in the IDE as well as the CLI.
        #
        # The portability gate did not catch this because it compiles with an explicit `-I.`, which proves the tree
        # is self-contained and says nothing about whether the Arduino toolchain supplies that -I. It does not.
        mkdir -p "$LIB/src/quorum"
        cp -r "$ROOT/$c/include/quorum/." "$LIB/src/quorum/"
    fi
done

# Generated casebooks sit beside the .ino: the sketch includes them unqualified.
# The generated casebooks go in the library too, so every include in the sketch is a library include and there is
# exactly one rule to remember.
cp "$GEN/pump_generated.hpp" "$GEN/tank_generated.hpp" "$LIB/src/"
# The learned casebook (quorum_learned sketch), if the host has produced it.
LEARNED="$ROOT/build/learn/learned/monks_learned_generated.hpp"
if [ -f "$LEARNED" ]; then
    cp "$LEARNED" "$LIB/src/"
    rm -rf "$(dirname "$OUT")/quorum_learned"
    cp -r "$ROOT/firmware/arduino/quorum_learned" "$(dirname "$OUT")/"
fi
# The runtime sketch: no generated header at all; the casebook arrives as a data image appended by quorum studio.
rm -rf "$(dirname "$OUT")/quorum_runtime"
cp -r "$ROOT/firmware/arduino/quorum_runtime" "$(dirname "$OUT")/"
cat > "$LIB/library.properties" <<'PROPS'
name=quorum
version=0.1.0
author=quorum
maintainer=quorum
sentence=Symbolic reasoning for mission-critical equipment: grounded-semantics argumentation on a microcontroller.
paragraph=Staged by firmware/arduino/stage-portable.sh. Header-only. Do not edit in place -- regenerate.
category=Other
architectures=*
PROPS

# Record exactly what was staged. A snapshot with no provenance is how you end up measuring last week's casebook.
{
    echo "# Staged snapshot -- $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "#"
    echo "# Casebook fingerprints compiled into this sketch. Compare against cbc's output if a measurement"
    echo "# ever looks wrong; a stale snapshot measures a casebook that no longer exists and looks correct."
    grep -h "kCasebookFingerprint" "$GEN/pump_generated.hpp" "$GEN/tank_generated.hpp" 2>/dev/null \
        | sed 's/^/# /' || true
    echo "#"
    echo "# git: $(cd "$ROOT" && git rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout')"
} > "$OUT/SNAPSHOT.txt"

COUNT=$(find "$OUT" -name '*.hpp' | wc -l | tr -d ' ')

# Windows needs backslashes and the real distro name. Building this string by hand got it wrong once
# (\\wsl$\\<distro>\\home\\<user>\\...), which is the sort of instruction that wastes someone's evening.
WINPATH="\\\\wsl\$\\${WSL_DISTRO_NAME:-Ubuntu}$(printf '%s' "$OUT" | tr '/' '\\\\')"

cat <<EOF
  1. Copy the folder out of WSL. Paste this into Windows Explorer's address bar:
       $WINPATH
     then copy the quorum_wcet folder into your Arduino sketchbook
     (usually Documents\\Arduino\\).

  2. Arduino IDE -> File -> Open -> quorum_wcet.ino
     Boards Manager -> install "Arduino UNO R4 Boards" (Renesas RA4M1, Cortex-M4).

  3. Tools -> Board -> Arduino UNO R4 Minima.  Tools -> Port -> the COM port that appears
     when you plug the board in. If NO port appears, the USB-C cable is charge-only --
     the board will light up regardless, which is what makes that failure confusing.

  4. Upload, then Tools -> Serial Monitor at 115200 baud.

The sketch sweeps all 16,384 pump states and prints the worst case it measured.

WHAT THE NUMBER MEANS -- AND WHAT IT DOES NOT.

The static bound is 2,571 INSTRUCTIONS. The board reports CYCLES. Those are different
units, and an earlier version of this message conflated them: it said a measured maximum
above 2,571 meant the machine model was wrong. It does not. A cycle count on real silicon
is normally HIGHER than the instruction count, because a flash access that stalls costs
several cycles while remaining one instruction.

The instruction bound was counted from Cortex-M33 disassembly. Compiling the same source
for Cortex-M4 -- the Uno R4's core -- gives an identical instruction count and an identical
opcode mix, differing only in scheduling, so the bound itself transfers to this part.

What this board CAN settle is the 2 cycles-per-instruction assumption README uses to turn
the bound into microseconds. The sketch prints cycles divided by 2,571 and says plainly
whether it came in at or under 2 CPI. Over 2 CPI means the microsecond figures in README
are understated, which is worth knowing; it does not mean the instruction bound is wrong.

Report the number either way. A bound that holds is only interesting because it could have
failed.
EOF
