#!/usr/bin/env bash
# Packages the PortMaster port: dist/hardtruck2.zip, and the same tree
# unpacked in dist/port/ (copy its contents into the device's ports/ folder).
#
#   scripts/package_port.sh [--screenshot FILE.png]
#
# Needs build-aarch64/ht2.aarch64 (docker/build_aarch64.sh). The libraries
# that are not on every CFW come out of the same builder image the binary was
# linked in (docker/Dockerfile.aarch64), with their licenses.
#
# No game material goes in: gamedata/ holds only its README. A screenshot of
# the game is game material too, so it is not kept in git: pass one (a frame
# dump through scripts/ppm2png.py), or put it at work/screenshot.png.

set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
image=ht2-builder-aarch64
bin="$repo/build-aarch64/ht2.aarch64"
shot="$repo/work/screenshot.png"
while [ $# -gt 0 ]; do
    case "$1" in
        --screenshot) shot="$2"; shift 2 ;;
        *) echo "usage: $0 [--screenshot FILE.png]" >&2; exit 64 ;;
    esac
done

[ -f "$bin" ] || { echo "no $bin: run docker/build_aarch64.sh first" >&2; exit 1; }

# The device's glibc may be as old as the builder's (2.31): refuse a binary
# that asks for anything newer (one built outside the container would).
newest=$(readelf --dyn-syms -W "$bin" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
if [ "$(printf '%s\n' "$newest" GLIBC_2.31 | sort -V | tail -1)" != GLIBC_2.31 ]; then
    echo "$bin needs $newest, newer than the builder's GLIBC_2.31" >&2; exit 1
fi

dist="$repo/dist"
out="$dist/port"
rm -rf "$out" "$dist/hardtruck2.zip"
mkdir -p "$out"
cp -r "$repo/port/." "$out/"
rm -f "$out/README.md"                      # port/README.md documents this tree
install -m 755 "$bin" "$out/hardtruck2/ht2.aarch64"
chmod 755 "$out/Hard Truck 2.sh"

# libmpg123 (music) and SDL2_ttf (win32hle's GDI text) are not on every CFW.
# SDL2 and GLES must be the device's own (its KMSDRM and GPU driver).
libs="$out/hardtruck2/libs.aarch64"
mkdir -p "$libs/licenses"
docker run --rm --platform=linux/arm64 --user "$(id -u):$(id -g)" -v "$libs":/out "$image" sh -c '
    set -e
    d=/usr/lib/aarch64-linux-gnu
    cp -L $d/libmpg123.so.0 $d/libSDL2_ttf-2.0.so.0 /out/
    cp /usr/share/doc/libmpg123-0/copyright /out/licenses/libmpg123.txt
    cp /usr/share/doc/libsdl2-ttf-2.0-0/copyright /out/licenses/SDL2_ttf.txt
'
needed=$(readelf -d "$bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')
echo "[package] ht2.aarch64 needs: $needed(newest glibc: $newest)"

if [ -f "$shot" ]; then
    cp "$shot" "$out/hardtruck2/screenshot.png"
else
    echo "[package] no screenshot ($shot): PortMaster shows none"
fi

(cd "$out" && zip -qr9 "$dist/hardtruck2.zip" "Hard Truck 2.sh" hardtruck2)
echo "Packaged: dist/hardtruck2.zip ($(du -h "$dist/hardtruck2.zip" | cut -f1)), unpacked in dist/port/"
