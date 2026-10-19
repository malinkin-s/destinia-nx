#!/bin/sh
# Bit-exact regression: the C interpreter must reproduce the Unicorn reference, frame for frame.
# Usage: port/tests/golden.sh [APK]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
APK=${1:-$ROOT/DESTINIA_v1_06.apk}
make -C "$ROOT/port" -s
SAVE=$(mktemp -d)
"$ROOT/port/destinia" --headless --apk "$APK" --save "$SAVE" --frames 700 \
    --script "$ROOT/docs/golden/newgame3.txt" --manifest "$SAVE/manifest.txt" 2>&1 | tail -1
if cmp -s "$SAVE/manifest.txt" "$ROOT/docs/golden/newgame3_700f.md5"; then echo "PASS: 700/700 frames identical"; rm -rf "$SAVE"; else
  echo "FAIL: first differing line:"; diff "$SAVE/manifest.txt" "$ROOT/docs/golden/newgame3_700f.md5" | head -3; exit 1; fi
