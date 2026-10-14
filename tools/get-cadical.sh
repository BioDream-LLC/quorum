#!/usr/bin/env bash
#
# Fetch and build the pinned CaDiCaL SAT solver used by quorum's independent cross-checks
# (tools/cross-verify-exhaustive.sh, tools/cross-verify.sh).
#
#   tools/get-cadical.sh            -> $HOME/cadical
#   CADICAL_DIR=/some/dir tools/get-cadical.sh
#
# CaDiCaL (https://github.com/arminbiere/cadical) is MIT-licensed and shares no code with quorum, which is the point:
# it checks quorum's grounded labellings without trusting anything quorum wrote. Nothing else in quorum needs it, and
# the checks report SKIP when it is absent.
set -euo pipefail

VERSION=rel-3.0.1
DIR="${CADICAL_DIR:-$HOME/cadical}"

if [ -f "$DIR/build/libcadical.a" ] && [ -f "$DIR/src/cadical.hpp" ]; then
    echo "CaDiCaL already built in $DIR ($(git -C "$DIR" describe --tags 2>/dev/null || echo unknown version))"
    exit 0
fi
if [ -e "$DIR" ]; then
    echo "$DIR exists but holds no built CaDiCaL; remove it or set CADICAL_DIR" >&2
    exit 2
fi
git clone -q --depth 1 --branch "$VERSION" https://github.com/arminbiere/cadical.git "$DIR"
(cd "$DIR" && ./configure > /dev/null && make -j"$(nproc)" > /dev/null)
echo "built CaDiCaL $VERSION in $DIR"
