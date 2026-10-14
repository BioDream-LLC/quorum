#!/usr/bin/env bash
# Build the Pico 2 runtime firmware (firmware/pico2, target quorum_runtime) in Docker -> build/quorum_runtime.uf2.
#
# Image quorum-pico:1 holds the Arm toolchain; the Pico SDK is mounted from $PICO_SDK_PATH (default ~/pico-sdk).
# The build tree is kept in build/pico2-runtime so picotool, which the SDK fetches and builds on first use, is not
# downloaded again on every run.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
sdk=${PICO_SDK_PATH:-$HOME/pico-sdk}
[[ -f "$sdk/pico_sdk_init.cmake" ]] || { echo "Pico SDK not found at $sdk (set PICO_SDK_PATH)" >&2; exit 1; }
docker image inspect quorum-pico:1 >/dev/null 2>&1 || { echo "Docker image quorum-pico:1 missing" >&2; exit 1; }
mkdir -p "$root/build"

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$root":/src -v "$sdk":/pico-sdk -w /src/firmware/pico2 quorum-pico:1 bash -euc '
  cmake -S . -B /src/build/pico2-runtime -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 ||
    cmake -S . -B /src/build/pico2-runtime -DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=Release
  cmake --build /src/build/pico2-runtime -j --target quorum_runtime >/tmp/log 2>&1 || { cat /tmp/log; exit 1; }
  ! grep -E "warning:" /tmp/log | grep -E "runtime|quorum" || { echo "warnings in quorum code" >&2; exit 1; }
  arm-none-eabi-size /src/build/pico2-runtime/quorum_runtime.elf | tail -1'
# Replace rather than overwrite: older hand builds ran the container as root and left a root-owned file.
rm -f "$root/build/quorum_runtime.uf2"
cp "$root/build/pico2-runtime/quorum_runtime.uf2" "$root/build/quorum_runtime.uf2"
echo "wrote $root/build/quorum_runtime.uf2 ($(stat -c %s "$root/build/quorum_runtime.uf2") bytes)"
