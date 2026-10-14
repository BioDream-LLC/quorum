#!/bin/bash
# Build the Arduino WCET sketch with arduino-cli.
#
# Exists because the include-path invocation is long enough to get wrong by hand, and getting it wrong produces a
# confusing "no such file" rather than an obvious mistake. Everything quorum needs is header-only, so there is no
# library to install -- just five include paths, one of which is generated.
#
# Usage:  firmware/arduino/build.sh [fqbn] [serial-port]
#
# Default board is the Uno R4 Minima, which is the interesting comparison against the Pico 2: 48 MHz against 150 MHz,
# so close cycle counts with ~3x different microsecond figures would show the numbers measure quorum rather than
# RP2350.
set -eu

cd "$(dirname "$0")/../.."
ROOT="$PWD"

FQBN="${1:-arduino:renesas_uno:minima}"
PORT="${2:-}"

GEN="$ROOT/build/casebook/compiler/gen"
if [ ! -f "$GEN/pump_generated.hpp" ]; then
    echo "Generated casebook headers missing."
    echo "cbc is a host tool and its exhaustive validation is what finds the worst-case state this sketch"
    echo "measures, so the host build has to come first:"
    echo
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
    exit 1
fi

if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "arduino-cli not found. Install it from https://arduino.github.io/arduino-cli/ then:"
    echo "  arduino-cli core update-index"
    echo "  arduino-cli core install arduino:renesas_uno    # Uno R4"
    echo "  arduino-cli core install arduino:mbed_nano      # Nano 33 BLE"
    echo "  arduino-cli core install arduino:sam            # Due"
    exit 1
fi

# -std=gnu++17, not c++17: the Arduino cores use GNU extensions in their own headers, so strict ISO mode fails inside
# vendor code rather than in ours.
INCLUDES="-I$ROOT/solver/include"
INCLUDES="$INCLUDES -I$ROOT/arbiter/include"
INCLUDES="$INCLUDES -I$ROOT/witness/include"
INCLUDES="$INCLUDES -I$ROOT/binding/include"
INCLUDES="$INCLUDES -I$ROOT/bench/include"
INCLUDES="$INCLUDES -I$GEN"

FLAGS="-std=gnu++17 -Os -fno-exceptions -fno-rtti -fno-threadsafe-statics $INCLUDES"

echo "board    $FQBN"
echo "sketch   firmware/arduino/quorum_wcet"
echo

arduino-cli compile \
    --fqbn "$FQBN" \
    --build-property "compiler.cpp.extra_flags=$FLAGS" \
    --warnings all \
    firmware/arduino/quorum_wcet

if [ -n "$PORT" ]; then
    echo
    echo "uploading to $PORT"
    arduino-cli upload --fqbn "$FQBN" -p "$PORT" firmware/arduino/quorum_wcet
    echo
    echo "read it with:  arduino-cli monitor -p $PORT -c baudrate=115200"
fi
