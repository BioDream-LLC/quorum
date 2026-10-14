#!/usr/bin/env bash
# Build quorum studio for Windows (dist/quorum-studio.exe) and Linux (dist/quorum-studio), in Docker.
#
# Builds everything, all in Docker: the Pico 2 runtime firmware (firmware/pico2/build-runtime.sh, quorum-pico:1 plus
# the Pico SDK at $PICO_SDK_PATH, default ~/pico-sdk), the Uno R4 runtime sketch (firmware/arduino/build-runtime.sh,
# quorum-arduino:1), the C++ tools natively and with MinGW, then the Go binaries with the tools and both firmwares
# embedded, then the installer.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
"$root/firmware/pico2/build-runtime.sh"
"$root/firmware/arduino/build-runtime.sh"

docker build -q -t quorum-studio-build:1 "$here" >/dev/null
# Version 0.<year>.<day of year>: each field must fit in 16 bits for Windows file properties.
VERSION=${VERSION:-0.$(date +%y).$(date +%j | sed "s/^0*//")}
docker run --rm -u "$(id -u):$(id -g)" -e VERSION="$VERSION" -e SIGN_PFX -e SIGN_PASS -e SIGN_TS -e HOME=/tmp -e GOCACHE=/tmp/gocache -e GOPATH=/tmp/gopath \
    -v "$root":/src -w /src quorum-studio-build:1 bash -euc '
  targets="cbc hazop2cb lopa2cb fmea2cb qblob learn2cb"
  cmake -S . -B /tmp/lin -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-static >/dev/null
  cmake --build /tmp/lin -j --target $targets >/dev/null
  cmake -S . -B /tmp/win -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=/src/studio/mingw.cmake >/dev/null
  cmake --build /tmp/win -j --target $targets >/dev/null

  bin=studio/internal/tools/bin
  rm -rf $bin studio/web/examples && mkdir -p $bin/linux $bin/windows $bin/firmware studio/web/examples
  for t in $targets; do
    cp "$(find /tmp/lin -name "$t" -type f -perm -u+x | head -1)" $bin/linux/
    cp "$(find /tmp/win -name "$t.exe" -type f | head -1)" $bin/windows/
  done
  cp build/quorum_runtime.uf2 $bin/firmware/quorum_runtime_pico2.uf2
  cp build/quorum_runtime_unor4.bin $bin/firmware/quorum_runtime_unor4.bin
  cp casebook/pump/pump.cb casebook/tank/tank.cb casebook/example/gearbox.cb studio/web/examples/

  cd studio
  test -z "$(gofmt -l .)" || { gofmt -l .; echo "gofmt" >&2; exit 1; }
  go vet ./...
  go test ./...
  mkdir -p dist
  CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o dist/quorum-studio ./cmd/quorum-studio
  CGO_ENABLED=0 GOOS=windows GOARCH=amd64 go build -trimpath -ldflags="-s -w" -o dist/quorum-studio.exe ./cmd/quorum-studio
  sign() {   # Authenticode, only when a certificate is supplied: SIGN_PFX=<file.pfx> SIGN_PASS=<password>
    [[ -n "${SIGN_PFX:-}" ]] || return 0
    osslsigncode sign -pkcs12 "$SIGN_PFX" -pass "${SIGN_PASS:-}" -n "quorum studio" -i "https://biodream.example" \
        -h sha256 ${SIGN_TS:+-ts "$SIGN_TS"} -in "$1" -out "$1.signed" >/dev/null && mv "$1.signed" "$1"
    osslsigncode verify -in "$1" 2>&1 | grep -q "Signature verification: ok" || { echo "signature check failed: $1" >&2; exit 1; }
    echo "signed $1"
  }
  sign dist/quorum-studio.exe
  makensis -V2 -DVERSION="$VERSION" installer.nsi
  sign "dist/quorum-studio-setup-$VERSION.exe"
'
ls -l "$here/dist"
