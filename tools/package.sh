#!/bin/sh
# Build the release zip: NRO + README + licences. Contains NO game data (the player supplies the APK).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=${1:-1.0.0}
"$ROOT/tools/build_switch.sh"
OUT=$ROOT/dist/destinia-switch-$VER; rm -rf "$OUT" "$OUT.zip"
mkdir -p "$OUT/switch/destinia"
cp "$ROOT/port/destinia.nro" "$OUT/switch/destinia/destinia.nro"
cp "$ROOT/dist/README.txt" "$ROOT/LICENSE" "$ROOT/NOTICE.txt" "$OUT/"
# safety net: refuse to ship if anything from the APK slipped in
if [ -f "$ROOT/DESTINIA_v1_06.apk" ]; then
  /usr/bin/python3 -I - "$ROOT" "$OUT" <<'PY'
import sys, zipfile
root, out = sys.argv[1:3]
nro = open(out + "/switch/destinia/destinia.nro", "rb").read()
z = zipfile.ZipFile(root + "/DESTINIA_v1_06.apk")
for n in z.namelist():
    if z.getinfo(n).file_size > 200 and not n.startswith("META"):
        d = z.read(n)
        for off in (50, len(d) // 2):
            c = d[off:off + 32]
            if len(set(c)) > 8 and c in nro: sys.exit("game data found in NRO: " + n)
print("ok: no game data in the package")
PY
fi
(cd "$OUT" && zip -qr "../destinia-switch-$VER.zip" .)
echo "$OUT.zip"
