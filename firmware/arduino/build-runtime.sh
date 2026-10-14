#!/usr/bin/env bash
# Build the Uno R4 runtime sketch (firmware/arduino/quorum_runtime) in Docker -> build/quorum_runtime_unor4.bin.
#
# The runtime sketch has no generated casebook in it (the casebook arrives later as a data image), so unlike the WCET
# sketch it needs no host build first: only the header-only libraries, staged as one Arduino library because the
# Arduino build adds a library's src/ to the include path and nothing else (see stage-portable.sh for that story).
#
# Toolchain: image quorum-arduino:1 (firmware/arduino/Dockerfile), the same arduino-cli and renesas_uno core versions
# the Windows hand builds used.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
out="$root/build/quorum_runtime_unor4.bin"

docker build -q -t quorum-arduino:1 "$here" >/dev/null

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/libraries/quorum/src/quorum"
for c in solver arbiter witness binding bench runtime; do
    cp -r "$root/$c/include/quorum/." "$stage/libraries/quorum/src/quorum/"
done
printf 'name=quorum\nversion=0.1.0\nauthor=quorum\nmaintainer=quorum\nsentence=quorum runtime headers.\nparagraph=Staged by build-runtime.sh.\ncategory=Other\narchitectures=*\n' \
    > "$stage/libraries/quorum/library.properties"
cp -r "$here/quorum_runtime" "$stage/"
mkdir -p "$root/build"

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$stage":/w -w /w quorum-arduino:1 \
    arduino-cli compile --fqbn arduino:renesas_uno:minima --libraries /w/libraries \
        --build-property "compiler.cpp.extra_flags=-I/w/libraries/quorum/src" \
        --build-path /tmp/b --output-dir /w/out quorum_runtime | grep -E "Sketch uses|Global variables"
cp "$stage/out/quorum_runtime.ino.bin" "$out"
echo "wrote $out ($(stat -c %s "$out") bytes)"
