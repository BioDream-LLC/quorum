#!/usr/bin/env bash
# Build quorum studio for macOS on a Mac, from this Linux checkout: dist/quorum-studio-macos.zip.
#
# Run studio/build.sh first: it builds the two runtime firmwares and the example list that every studio carries.
# This script then copies the committed source (git archive HEAD, so nothing uncommitted or untracked travels) and
# those build outputs to the Mac, and there builds the C++ tools as universal binaries (arm64 + x86_64), runs the Go
# tests, builds the studio for both architectures, joins them with lipo, and zips the result.
#
#   MAC=user@host  the Mac to build on (required)   MAC_KEY=<ssh key> (default ~/.ssh/id_ed25519)
#   Needs on the Mac: Xcode command-line tools (clang, lipo, codesign), Homebrew cmake and go.
#
# The program is ad-hoc signed only (Apple Silicon will not run an unsigned binary). It is not signed with an Apple
# Developer ID or notarized, so macOS Gatekeeper asks before the first run; the README says how to allow it.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
MAC=${MAC:?set MAC=user@host, the Mac to build on}
KEY=${MAC_KEY:-$HOME/.ssh/id_ed25519}
ssh_mac() { ssh -o BatchMode=yes -i "$KEY" "$MAC" "$@"; }
VERSION=${VERSION:-0.$(date +%y).$(date +%j | sed "s/^0*//")}

for f in studio/internal/tools/bin/firmware/quorum_runtime_pico2.uf2 studio/internal/tools/bin/firmware/quorum_runtime_unor4.bin \
         studio/web/examples/pump.cb; do
    [ -s "$root/$f" ] || { echo "missing $f: run studio/build.sh first" >&2; exit 1; }
done
[ -z "$(git -C "$root" status --porcelain -- studio casebook solver arbiter binding witness runtime learn CMakeLists.txt)" ] ||
    echo "note: uncommitted changes are NOT included; the Mac builds HEAD ($(git -C "$root" rev-parse --short HEAD))" >&2

work=/tmp/quorum-macos-build
ssh_mac "rm -rf $work && mkdir -p $work/src"
git -C "$root" archive --format=tar HEAD -- . ':!ip' ':!presentations' | ssh_mac "tar -x -C $work/src"
tar -C "$root" -cf - studio/internal/tools/bin/firmware studio/web/examples | ssh_mac "tar -x -C $work/src"

ssh_mac "VERSION=$VERSION WORK=$work zsh -l" <<'MAC'
set -euo pipefail
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
cd "$WORK/src"
targets=(cbc hazop2cb lopa2cb fmea2cb qblob learn2cb)
cmake -S . -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 >/dev/null
cmake --build "$WORK/build" -j 8 --target "${targets[@]}" >/dev/null
bin=studio/internal/tools/bin/darwin
mkdir -p "$bin"
for t in "${targets[@]}"; do
    f=$(find "$WORK/build" -name "$t" -type f -perm -u+x | head -1)
    lipo "$f" -verify_arch arm64 x86_64
    cp "$f" "$bin/"
done
cd studio
export GOCACHE="$WORK/gocache" GOPATH="$WORK/gopath" GOFLAGS=-mod=mod CGO_ENABLED=0
go vet ./...
go test ./... 2>&1 | tail -20
mkdir -p dist
GOARCH=arm64 go build -trimpath -ldflags="-s -w" -o dist/qs-arm64 ./cmd/quorum-studio
GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o dist/qs-amd64 ./cmd/quorum-studio
lipo -create -output dist/quorum-studio dist/qs-arm64 dist/qs-amd64
rm dist/qs-arm64 dist/qs-amd64
codesign --force --sign - dist/quorum-studio
codesign --verify dist/quorum-studio
lipo -info dist/quorum-studio
pkg="$WORK/quorum-studio-macos"
rm -rf "$pkg" && mkdir -p "$pkg"
cp dist/quorum-studio "$pkg/"
cp ../tools/licenses/PolyForm-Noncommercial-1.0.0.md "$pkg/LICENSE.md"
cat > "$pkg/README.txt" <<EOF
quorum studio $VERSION for macOS (Apple Silicon and Intel, macOS 12 or later)

Double-click quorum-studio. A Terminal window opens and your browser opens on the studio; close the Terminal
window to stop it.

The first time, macOS may say it cannot check the developer. This build is not signed with an Apple Developer ID.
To allow it: Control-click quorum-studio, choose Open, then Open again. Or, in Terminal:
    xattr -d com.apple.quarantine quorum-studio

Write to board: a Pico 2 held in BOOTSEL mode appears under /Volumes and is written directly. For an Uno R4, install
the "Arduino UNO R4 Boards" package in the Arduino IDE once; the studio uses the dfu-util it installs.

Licence: PolyForm Noncommercial 1.0.0. Commercial licences from BioDream LLC (firstmind@biodream.ai).
EOF
(cd "$WORK" && rm -f quorum-studio-macos.zip && ditto -c -k --keepParent quorum-studio-macos quorum-studio-macos.zip)
ls -l "$WORK/quorum-studio-macos.zip"
MAC

mkdir -p "$here/dist"
scp -q -i "$KEY" "$MAC:$work/quorum-studio-macos.zip" "$here/dist/quorum-studio-macos.zip"
ssh_mac "rm -rf $work"
ls -l "$here/dist/quorum-studio-macos.zip"
