#!/bin/bash
# Disassemble the safety function for a named Cortex-M core and report per-loop instruction costs.
#
# This is the input to wcet/test_static_bound.cpp's machine model. The model is hand-transcribed from this output, so
# when the solver changes, run this and update the model -- otherwise the static bound silently describes old code.
#
# Usage: tools/wcet-disassemble.sh [cortex-m33|cortex-m4|...]
#
# The core is an argument because the static bound in wcet/ was counted from Cortex-M33 disassembly (the Pico 2's
# RP2350), and the first board actually in hand is an Uno R4 Minima -- a Cortex-M4 at 48 MHz. Those are different
# instruction counts from the same source, so a measurement taken on one core cannot validate a bound derived from
# the other. Naming the core in the argument makes that impossible to forget.
set -eu
cd "$(dirname "$0")/.."

CORE="${1:-cortex-m33}"
mkdir -p /tmp/quorum-wcet
cat > /tmp/quorum-wcet/solve_only.cpp <<'CPP'
#include "quorum/solver/grounded.hpp"
using namespace quorum::solver;
// The pump's exact shape, so the disassembly is the code that actually ships.
extern "C" Status quorum_solve(Grounded<44>& g, const Framework<44, 80>& fw) { return g.solve(fw); }
CPP

if ! docker image inspect quorum-arm:1 >/dev/null 2>&1 && ! docker image inspect quorum-multi:1 >/dev/null 2>&1; then
    echo "need the pinned toolchain image; run tools/board-matrix.sh once to build it"
    exit 1
fi
IMAGE=quorum-multi:1
docker image inspect "$IMAGE" >/dev/null 2>&1 || IMAGE=quorum-arm:1

exec docker run --rm -i -e CORE="$CORE" -v "$PWD:/src" -v /tmp/quorum-wcet:/w -w /src "$IMAGE" bash -s <<'SCRIPT'
echo "core: $CORE"
arm-none-eabi-g++ -std=c++17 -mcpu="$CORE" -mthumb -Os -fno-exceptions -fno-rtti \
  -Wall -Wextra -Werror -Isolver/include -c /w/solve_only.cpp -o /w/solve.o
arm-none-eabi-objdump -d --no-show-raw-insn /w/solve.o > /w/solve.asm

printf 'total instructions in solve():        %s\n' "$(grep -cE '^\s+[0-9a-f]+:' /w/solve.asm)"
printf 'memory accesses to steps_ (off 412):  %s\n' "$(grep -c '#412' /w/solve.asm || true)"
echo
echo "Backward branches mark loop back-edges; each is the bottom of one loop."
grep -E '\b(b|bgt|blt|ble|bge|bne|beq)\b(\.[nw])?\s+[0-9a-f]+' /w/solve.asm | head -30
echo
echo "Full disassembly follows; transcribe per-loop costs into wcet/test_static_bound.cpp's Model."
echo "-------------------------------------------------------------------------------"
sed -n '/quorum_solve>:/,$p' /w/solve.asm
SCRIPT
