#!/bin/bash
# Measure quorum's footprint on a real Cortex-M33, and fail if it has regressed.
#
# Wraps the containerised toolchain so this is one command from the project root. Requires the host build first,
# because the generated casebook headers come from it.
set -e
cd "$(dirname "$0")/.."

if [ ! -f build/casebook/compiler/gen/tank_generated.hpp ]; then
    echo "generated headers missing; run the host build first:"
    echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"
    exit 1
fi

if ! docker image inspect quorum-arm:1 >/dev/null 2>&1; then
    echo "building the pinned toolchain image (once)..."
    docker build -q -f tools/arm.Dockerfile -t quorum-arm:1 tools/
fi

exec docker run --rm \
    -v "$PWD:/src" \
    -v "$PWD/build/casebook/compiler/gen:/gen" \
    -w /src quorum-arm:1 bash firmware/measure.sh
