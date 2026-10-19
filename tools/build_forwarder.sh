#!/usr/bin/env bash
# build_forwarder.sh -- Build Destinia NSP & NSZ forwarder for Nintendo Switch
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TITLE_ID="0100d3571a000000"
TITLE_NAME="Destinia"
AUTHOR="GAMEVIL / Clean-room"
VERSION="0.7.3"

# hacBrewPack binary and keys
HACBREWPACK="${HACBREWPACK:-$(command -v hacbrewpack || true)}"
if [ -z "$HACBREWPACK" ] && [ -x "$ROOT_DIR/tools/hacBrewPack/hacbrewpack" ]; then
    HACBREWPACK="$ROOT_DIR/tools/hacBrewPack/hacbrewpack"
fi

KEYS_FILE="${KEYS_FILE:-}"
if [ -z "$KEYS_FILE" ] && [ -f "$ROOT_DIR/tools/hacBrewPack/keys.dat" ]; then
    KEYS_FILE="$ROOT_DIR/tools/hacBrewPack/keys.dat"
elif [ -z "$KEYS_FILE" ] && [ -f "$HOME/.switch/prod.keys" ]; then
    KEYS_FILE="$HOME/.switch/prod.keys"
fi

echo "[1/5] Compiling forwarder executable with devkitA64..."
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT_DIR":/work -w /work/tools/forwarder_builder/nx-hbloader devkitpro/devkita64 make clean
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT_DIR":/work -w /work/tools/forwarder_builder/nx-hbloader devkitpro/devkita64 make

echo "[2/5] Preparing staging directories..."
STAGING="$ROOT_DIR/staging_forwarder"
rm -rf "$STAGING"
mkdir -p "$STAGING/exefs" "$STAGING/control" "$STAGING/romfs" "$ROOT_DIR/out_nsp"
touch "$STAGING/romfs/.keep"

cp "$ROOT_DIR/tools/forwarder_builder/nx-hbloader/hbl.nso" "$STAGING/exefs/main"
cp "$ROOT_DIR/tools/forwarder_builder/nx-hbloader/hbl.npdm" "$STAGING/exefs/main.npdm"

docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT_DIR":/work -w /work devkitpro/devkita64 \
    nacptool --create "$TITLE_NAME" "$AUTHOR" "$VERSION" staging_forwarder/control/control.nacp --titleid="0100D3571A000000"

cp "$ROOT_DIR/icon.jpg" "$STAGING/control/icon_AmericanEnglish.dat"

echo "[3/5] Packing NSP with hacBrewPack..."
if [ -z "$HACBREWPACK" ] || [ ! -x "$HACBREWPACK" ]; then
    echo "Error: hacbrewpack not found. Please install hacbrewpack or place it in tools/hacBrewPack/."
    exit 1
fi
if [ -z "$KEYS_FILE" ] || [ ! -f "$KEYS_FILE" ]; then
    echo "Error: Switch keys file (keys.dat / prod.keys) not found. Provide KEYS_FILE=/path/to/prod.keys"
    exit 1
fi

"$HACBREWPACK" -k "$KEYS_FILE" \
    --exefsdir "$STAGING/exefs" \
    --controldir "$STAGING/control" \
    --romfsdir "$STAGING/romfs" \
    --titleid "$TITLE_ID" \
    --nologo \
    --nspdir out_nsp

NSP_TARGET="dist/Destinia [0100D3571A000000] [v0].nsp"
NSZ_TARGET="dist/Destinia [0100D3571A000000] [v0].nsz"

mkdir -p dist
cp "out_nsp/${TITLE_ID}.nsp" "$NSP_TARGET"

echo "[4/5] Compressing to NSZ format..."
if [ -x "$(command -v nsz)" ]; then
    nsz -w -C "$NSP_TARGET" -o dist/
elif [ -x "$HOME/.local/bin/nsz" ]; then
    "$HOME/.local/bin/nsz" -w -C "$NSP_TARGET" -o dist/
fi

echo "[5/5] Success! Output files:"
ls -lh "$NSP_TARGET" "$NSZ_TARGET"
