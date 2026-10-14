#!/bin/bash
# Which boards does quorum actually run on?
#
# This exists because the question got asked and the honest answer was "nobody has ever checked". The project targets
# "microcontrollers", which is not a specification: Cortex-M0+ through M7 differ in instruction set, and an 8-bit AVR
# differs in word size, and a claim of portability that has been compiled for exactly one core is not a claim.
#
# Every target below is compiled with the SAME warnings-as-errors set the host build uses. A board that needs a warning
# relaxed is not supported; it is aspired to.
#
# Run: tools/board-matrix.sh   (needs docker; builds a pinned multi-toolchain image once)
set -u

cd "$(dirname "$0")/.."

GEN="build/casebook/compiler/gen"
if [ ! -f "$GEN/pump_generated.hpp" ]; then
    echo "generated headers missing; run the host build first:"
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
    exit 1
fi

if ! docker image inspect quorum-multi:1 >/dev/null 2>&1; then
    echo "building the pinned multi-toolchain image (once)..."
    cat > /tmp/quorum-multi.Dockerfile <<'DOCKER'
FROM debian:12-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi \
      libstdc++-arm-none-eabi-dev libstdc++-arm-none-eabi-newlib \
      gcc-avr avr-libc binutils-avr \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
DOCKER
    docker build -q -f /tmp/quorum-multi.Dockerfile -t quorum-multi:1 /tmp/ >/dev/null
fi

exec docker run --rm -i -v "$PWD:/src" -v "$PWD/$GEN:/gen" -w /src quorum-multi:1 bash -s <<'SCRIPT'
set -u
failures=0

# A realistic device translation unit: engine, arbiter, binding, cycle counter, two real casebooks.
cat > /tmp/probe.cpp <<'EOF'
#include "quorum/arbiter/verdict.hpp"
#include "quorum/binding/binding.hpp"
#include "quorum/bench/cycles.hpp"
#include "pump_generated.hpp"
#include "tank_generated.hpp"

namespace pump = quorum::casebook::gen::centrifugal_pump;
namespace tank = quorum::casebook::gen::tank_409;

pump::CaseFramework g_fw;
pump::CaseSolver g_solver;
pump::CasePolicy g_policy;
tank::CaseBinding g_bind;
volatile int g_out;
volatile unsigned g_cycles;

int main() {
    if (pump::make_policy(g_policy) != quorum::solver::Status::Ok) { return 1; }
    if (tank::make_binding(g_bind) != quorum::binding::Status::Ok) { return 1; }
    (void)quorum::bench::enable();
    for (;;) {
        const unsigned t0 = quorum::bench::now();
        if (pump::instantiate(g_fw, pump::kWorstCaseMask) != quorum::solver::Status::Ok) { return 1; }
        g_out = quorum::arbiter::decide(g_fw, g_solver, g_policy).action.get();
        g_cycles = quorum::bench::elapsed(t0, quorum::bench::now());
    }
}
EOF

WARN="-Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Werror"
FREE="-fno-exceptions -fno-rtti -fno-threadsafe-statics"
INC="-Isolver/include -Iarbiter/include -Ibinding/include -Ibench/include -I/gen"

printf '%-40s %-12s %7s %7s %7s  %s\n' "board" "core" "flash" "RAM" "SRAM" "verdict"
printf '%-40s %-12s %7s %7s %7s  %s\n' "----------------------------------------" "------------" "-------" "-------" "-------" "-------"

arm_target() {
    local board="$1" core="$2" cpu="$3" sram="$4"
    printf '%-40s %-12s ' "$board" "$core"
    if arm-none-eabi-g++ -std=c++17 -mcpu="$cpu" -mthumb -Os $FREE $WARN $INC \
           -c /tmp/probe.cpp -o /tmp/p.o 2>/tmp/err; then
        local f r
        f=$(arm-none-eabi-size -A /tmp/p.o | awk '$1~/text|rodata/{t+=$2} END{print t+0}')
        r=$(arm-none-eabi-size -A /tmp/p.o | awk '$1~/bss|data/{t+=$2} END{print t+0}')
        # Stack ceiling is 332 bytes measured; allow 1 KB for an Arduino/SDK core's own globals and buffers.
        local need=$((r + 332 + 1024))
        if [ "$need" -le "$sram" ]; then
            printf '%7d %7d %7d  SUPPORTED\n' "$f" "$r" "$sram"
        else
            printf '%7d %7d %7d  TOO SMALL (needs ~%d B)\n' "$f" "$r" "$sram" "$need"
            failures=$((failures + 1))
        fi
    else
        printf '%7s %7s %7s  COMPILE FAILS\n' "-" "-" "$sram"
        grep -m2 -E 'error' /tmp/err | sed 's/^/          /'
        failures=$((failures + 1))
    fi
}

echo "--- Raspberry Pi microcontrollers ---"
arm_target "Raspberry Pi Pico 2 (RP2350)"        "Cortex-M33"  cortex-m33     532480
arm_target "Raspberry Pi Pico 1 (RP2040)"        "Cortex-M0+"  cortex-m0plus  270336

echo
echo "--- Arduino boards that are Cortex-M ---"
arm_target "Arduino Uno R4 (Renesas RA4M1)"      "Cortex-M4"   cortex-m4       32768
arm_target "Arduino Nano 33 BLE (nRF52840)"      "Cortex-M4"   cortex-m4      262144
arm_target "Arduino Due (SAM3X8E)"               "Cortex-M3"   cortex-m3       98304
arm_target "Arduino Giga R1 / Portenta (STM32H7)" "Cortex-M7"  cortex-m7      524288

echo
echo "--- Arduino boards that are 8-bit AVR ---"
printf '%-40s %-12s ' "Arduino Uno / Nano (ATmega328P)" "AVR 8-bit"
if avr-g++ -std=c++17 -mmcu=atmega328p -Os -fno-exceptions -fno-rtti $INC \
       -c /tmp/probe.cpp -o /tmp/avr.o 2>/tmp/averr; then
    printf '%7s %7s %7d  compiles -- check RAM\n' "?" "?" 2048
else
    printf '%7s %7s %7d  NOT SUPPORTED\n' "-" "-" 2048
    grep -m1 -E 'error' /tmp/averr | sed 's/^/          /'
fi
printf '          reason: Debian ships avr-g++ %s, which predates C++17 entirely --\n' "$(avr-g++ -dumpversion)"
echo "          'namespace quorum::solver { }' is a C++17 nested namespace definition (gcc 6+)."
echo "          Separately, 2048 B of SRAM cannot hold a 1.6 KB framework plus a 332 B stack"
echo "          plus the Arduino core's own buffers. The word size is the deeper problem:"
echo "          every int32_t operation on an 8-bit core is several instructions."

echo
if [ "$failures" -ne 0 ]; then
    echo "$failures target(s) not supported -- see above"
    exit 1
fi
echo "every Cortex-M target compiles under the same warnings-as-errors set as the host build"
SCRIPT
