#!/usr/bin/env bash
# Pack the Linux studio download: tools/pack-linux-release.sh <quorum-studio binary> <LICENSE.md> <NOTICE> <out.tar.gz>
# The archive records root as owner and group (tar keeps the packer's user name otherwise, and anyone listing the
# archive sees it), sorted names, and no name or time in the gzip header. It refuses to finish if any other owner
# got in.
set -euo pipefail
[ $# -eq 4 ] || { echo "usage: $0 <binary> <LICENSE.md> <NOTICE> <out.tar.gz>" >&2; exit 2; }
out=$(realpath -m "$4")
top=$(basename "$out" .tar.gz)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir "$work/$top"
cp "$1" "$work/$top/quorum-studio"
cp "$2" "$work/$top/LICENSE.md"
cp "$3" "$work/$top/NOTICE"
chmod 755 "$work/$top" "$work/$top/quorum-studio"
chmod 644 "$work/$top/LICENSE.md" "$work/$top/NOTICE"
(cd "$work" && tar --owner=root:0 --group=root:0 --sort=name -cf - "$top" | gzip -9 -n > "$out")
owners=$(tar -tvzf "$out" | awk '{print $2}' | sort -u)
if [ "$owners" != "root/root" ]; then
    echo "error: $out records owner(s) $owners, not only root/root" >&2
    rm -f "$out"
    exit 1
fi
tar -tvzf "$out"
