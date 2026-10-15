#!/usr/bin/env bash
# Build the Pico 2 runtime firmware (firmware/pico2, target quorum_runtime) in Docker -> build/quorum_runtime.uf2.
#
# Image quorum-pico:1 holds the Arm toolchain (built here if missing); the Pico SDK is mounted from $PICO_SDK_PATH
# (default ~/pico-sdk, else fetched into build/pico-sdk).
# The build tree is kept in build/pico2-runtime so picotool, which the SDK fetches and builds on first use, is not
# downloaded again on every run.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
image=${QUORUM_PICO_IMAGE:-quorum-pico:1}
mkdir -p "$root/build"

# A machine that has never built quorum has neither the image nor the SDK, so both are made here: the images from
# the Dockerfiles in this repository, and the SDK at the pinned release (2.1.1, with only the tinyusb submodule it
# needs) in build/pico-sdk. An SDK already at $PICO_SDK_PATH, or ~/pico-sdk, is used as it is.
if ! docker image inspect "$image" >/dev/null 2>&1; then
  echo "building Docker image $image (once)"
  docker image inspect quorum-arm:1 >/dev/null 2>&1 || docker build -q -t quorum-arm:1 -f "$root/tools/arm.Dockerfile" "$root" >/dev/null
  docker build -q -t "$image" -f "$here/Dockerfile" "$root" >/dev/null
fi
sdk=${PICO_SDK_PATH:-$HOME/pico-sdk}
if [[ ! -f "$sdk/pico_sdk_init.cmake" ]]; then
  sdk="$root/build/pico-sdk"
  if [[ ! -f "$sdk/pico_sdk_init.cmake" ]]; then
    echo "fetching the Pico SDK 2.1.1 into build/pico-sdk (once)"
    rm -rf "$sdk"
    git clone -q --depth 1 --branch 2.1.1 https://github.com/raspberrypi/pico-sdk.git "$sdk"
    git -C "$sdk" submodule update -q --init --depth 1 lib/tinyusb
  fi
fi

# The firmware project also builds the WCET targets, which include the generated pump and tank headers. A fresh
# checkout has none, so cbc is built for the host (in the same image, in its own build tree) and makes them.
if [[ ! -f "$root/build/casebook/compiler/gen/pump_generated.hpp" || ! -f "$root/build/casebook/compiler/gen/tank_generated.hpp" ]]; then
  echo "generating the pump and tank headers with cbc (once)"
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$root":/src -w /src "$image" bash -euc '
    cmake -S . -B build/gen-host -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build/gen-host -j --target cbc >/dev/null
    mkdir -p build/casebook/compiler/gen
    for c in pump tank; do
      build/gen-host/casebook/compiler/cbc casebook/$c/$c.cb --quiet --header build/casebook/compiler/gen/${c}_generated.hpp
    done'
fi

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$root":/src -v "$sdk":/pico-sdk -w /src/firmware/pico2 "$image" bash -euc '
  cmake -S . -B /src/build/pico2-runtime -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 ||
    cmake -S . -B /src/build/pico2-runtime -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
  cmake --build /src/build/pico2-runtime -j --target quorum_runtime >/tmp/log 2>&1 || { cat /tmp/log; exit 1; }
  ! grep -E "warning:" /tmp/log | grep -E "runtime|quorum" || { echo "warnings in quorum code" >&2; exit 1; }
  arm-none-eabi-size /src/build/pico2-runtime/quorum_runtime.elf | tail -1'
# Replace rather than overwrite: older hand builds ran the container as root and left a root-owned file.
rm -f "$root/build/quorum_runtime.uf2"
cp "$root/build/pico2-runtime/quorum_runtime.uf2" "$root/build/quorum_runtime.uf2"
echo "wrote $root/build/quorum_runtime.uf2 ($(stat -c %s "$root/build/quorum_runtime.uf2") bytes)"
