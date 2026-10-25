#!/bin/bash
# build-deb.sh <version>  -- e.g. 2.0.12-subspace1
# Packages the flrig built in this tree (run ./configure && make first) as
# flrig-subspace: /usr/bin/flrig-subspace, its own menu entry and icon,
# beside any stock flrig. Writes the .deb to $OUT (default: this tree).
set -euo pipefail
VER=${1:?usage: build-deb.sh <version>}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ST=$ROOT/pkg-staging
BIN=$ROOT/src/flrig
[ -x "$BIN" ] || { echo "no $BIN -- run ./configure && make first"; exit 1; }

rm -rf "$ST"
mkdir -p "$ST/DEBIAN" "$ST/usr/bin" "$ST/usr/share/applications" \
         "$ST/usr/share/icons/hicolor/64x64/apps" "$ST/usr/share/doc/flrig-subspace"
install -m 755 -s "$BIN" "$ST/usr/bin/flrig-subspace"
install -m 644 "$ROOT/packaging/deb/flrig-subspace.desktop" "$ST/usr/share/applications/"
install -m 644 "$ROOT/data/flrig.png" "$ST/usr/share/icons/hicolor/64x64/apps/flrig-subspace.png"
install -m 644 "$ROOT/COPYING" "$ST/usr/share/doc/flrig-subspace/copyright"
sed "s/@VERSION@/$VER/" "$ROOT/packaging/deb/control.in" > "$ST/DEBIAN/control"
find "$ST" -type d -exec chmod 755 {} +   # never group-writable in a .deb
chmod 644 "$ST/DEBIAN/control"

OUT=${OUT:-$ROOT}/flrig-subspace_${VER}_amd64.deb
dpkg-deb --build --root-owner-group "$ST" "$OUT"
echo "$OUT"
