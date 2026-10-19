#!/bin/sh
# Build the Switch NRO inside the devkitPro container. Output: port/destinia.nro
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT":/work -w /work/port devkitpro/devkita64 \
    make -f Makefile.switch "$@"
