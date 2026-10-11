#!/bin/bash
# Assembles build/package/vip_infiniteammo: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/vip_infiniteammo"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/vip_infiniteammo/libvip_infiniteammo.so" "$OUT/bin/"
cp "$ROOT/build/plugins/vip_infiniteammo/vip_infiniteammo.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
