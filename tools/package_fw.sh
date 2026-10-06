#!/usr/bin/env bash
#
# Bundle a devOS build into a zip you can copy to a Mac and install with one
# script - no repo, no ESP-IDF, no browser on the other side.
#
#   idf.py build
#   tools/package_fw.sh                          # -> dist/devos-<ver>-<build>.zip
#   scp dist/devos-*.zip macbook:~/              # then, on the MacBook:
#   unzip devos-*.zip && cd devos-* && ./install.sh
#
# The zip holds the four flash images, `install.sh` (see tools/flash_fw.sh),
# a double-click `install.command` for macOS, the offsets the build used and a
# short README.

set -euo pipefail

# See flash_fw.sh: never fail silently under `set -e`.
trap 'rc=$?; printf "%s: unexpected failure (exit %d) at line %s\n" "$(basename "$0")" "$rc" "$LINENO" >&2' ERR

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build}"
OUT_ROOT=""
NOTES=""
FORCE=0

usage() {
    cat <<EOF
Package a devOS build for a one-script USB install.

Usage: $(basename "$0") [options]

  --build DIR   ESP-IDF build folder (default: build/)
  --out DIR     where to put the zip (default: dist/)
  --notes TEXT  short note shown in the bundle README
  --force       package even if the build looks stale
  -h, --help    this text

Environment:
  BUILD         same as --build
EOF
}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --build) BUILD="${2:-}"; shift 2 ;;
        --out)   OUT_ROOT="${2:-}"; shift 2 ;;
        --notes) NOTES="${2:-}"; shift 2 ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option: $1" ;;
    esac
done

[ -n "$OUT_ROOT" ] || OUT_ROOT="$ROOT/dist"

# ---- inputs ---------------------------------------------------------------
APP="$BUILD/tab5-devos.bin"
FLASH_ARGS="$BUILD/flash_args"
[ -f "$APP" ] || die "no app image at $APP - run 'idf.py build' first"
[ -f "$FLASH_ARGS" ] || die "no $FLASH_ARGS - run 'idf.py build' first"

# A failed (or skipped) `idf.py build` leaves the previous image in place, so
# packaging blindly ships whatever was there before - which is how a stale zip
# gets flashed and tested by mistake. Refuse unless the image is newer than
# every source file.
if [ "$FORCE" -eq 0 ]; then
    stale="$(find "$ROOT/components" "$ROOT/main" -name '*.[ch]' -newer "$APP" -print -quit 2>/dev/null)"
    if [ -n "$stale" ]; then
        die "the build is stale: ${stale#$ROOT/} is newer than $(basename "$APP").
       Run 'idf.py build' first, and check it succeeded - a failed build leaves
       the old image behind. Use --force to package anyway."
    fi
fi

# Version from the source of truth, so the bundle can never disagree with the
# firmware about what it is.
CFG="$ROOT/components/devos_config/include/devos_config.h"
[ -f "$CFG" ] || die "cannot find $CFG"
VERSION="$(sed -n 's/.*DEVOS_VERSION_MAJOR *\([0-9]*\).*/\1/p' "$CFG" | head -1).$(sed -n 's/.*DEVOS_VERSION_MINOR *\([0-9]*\).*/\1/p' "$CFG" | head -1).$(sed -n 's/.*DEVOS_VERSION_PATCH *\([0-9]*\).*/\1/p' "$CFG" | head -1)"
case "$VERSION" in
    *..*|.*|*.) die "could not read DEVOS_VERSION_* from $CFG" ;;
esac

# The image carries its own build id (the ELF hash esptool stamps in), which is
# how a Tab5 tells two builds of the same version apart - so name the zip with
# it and the two can never be confused either.
BUILD_ID=""
if command -v python3 >/dev/null 2>&1; then
    BUILD_ID="$(python3 - "$ROOT" "$APP" <<'PY' 2>/dev/null || true
import sys
sys.path.insert(0, sys.argv[1] + "/tools")
try:
    from make_ota_manifest import image_info
    with open(sys.argv[2], "rb") as f:
        print(image_info(f.read())[0][:8])
except Exception:
    pass
PY
)"
fi
if [ -z "$BUILD_ID" ]; then
    BUILD_ID="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || true)"
fi
[ -n "$BUILD_ID" ] || BUILD_ID="$(date +%Y%m%d%H%M)"

NAME="devos-$VERSION-$BUILD_ID"
STAGE="$OUT_ROOT/$NAME"
ZIP="$OUT_ROOT/$NAME.zip"

# ---- stage ----------------------------------------------------------------
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Flatten the images and keep the offsets flash_args gave us.
copied=0
while read -r off path; do
    [ -n "${path:-}" ] || continue
    base="${path##*/}"
    src="$BUILD/$path"
    [ -f "$src" ] || src="$BUILD/$base"
    [ -f "$src" ] || die "flash_args lists $path but $src is missing"
    cp "$src" "$STAGE/$base"
    copied=$((copied + 1))
done < <(awk '/^0x/ { print $1, $2 }' "$FLASH_ARGS")
[ "$copied" -gt 0 ] || die "no images listed in $FLASH_ARGS"

# A normalized flash_args for install.sh: same settings line, flat paths.
awk '/^--/ { print; next } /^0x/ { nm = $2; sub(/.*\//, "", nm); print $1, nm }' \
    "$FLASH_ARGS" > "$STAGE/flash_args"

cp "$ROOT/tools/flash_fw.sh" "$STAGE/install.sh"
chmod +x "$STAGE/install.sh"

cat > "$STAGE/install.command" <<'EOF'
#!/bin/bash
# Double-click this on macOS to install. The window stays open at the end.
cd "$(dirname "$0")" || exit 1
./install.sh "$@"
rc=$?
echo
[ "$rc" -eq 0 ] || echo "install.sh exited with status $rc"
printf 'Press Return to close this window.'
read -r _
EOF
chmod +x "$STAGE/install.command"

{
    echo "devOS $VERSION  (build $BUILD_ID)"
    echo
    [ -n "$NOTES" ] && { echo "What changed: $NOTES"; echo; }
    echo "Install onto a Tab5 over USB-C. Needs python3 and esptool on this"
    echo "machine only; no repo or ESP-IDF."
    echo
    echo "  ./install.sh                 auto-detects the port"
    echo "  ./install.sh -p /dev/cu.usbmodemXXXX"
    echo "  ./install.sh --dry-run       print the command, flash nothing"
    echo "  ./install.sh --erase         full erase first (wipes Wi-Fi/Tailscale/keys)"
    echo
    echo "On macOS you can also double-click install.command."
    echo
    echo "esptool is missing? Install it once:"
    echo "  python3 -m pip install --user esptool"
    echo
    echo "Files: install.sh, the flash images, and the offsets from this build."
} > "$STAGE/README.txt"

# ---- zip ------------------------------------------------------------------
mkdir -p "$OUT_ROOT"
if command -v zip >/dev/null 2>&1; then
    ( cd "$OUT_ROOT" && zip -rq "$NAME.zip" "$NAME" )
else
    python3 -m zipfile -c "$ZIP" "$STAGE"
fi

printf 'packaged  %s\n' "$ZIP"
printf '  version %s, build %s, %d image(s)\n' "$VERSION" "$BUILD_ID" "$copied"

# Print the command to run on the *other* machine (the pull direction is the
# one that works without the build box being able to reach the laptop). Prefer
# the Tailscale address, which works from anywhere on the tailnet.
HOST_ADDR="$(command -v tailscale >/dev/null 2>&1 && tailscale ip -4 2>/dev/null | head -1 || true)"
if [ -z "$HOST_ADDR" ]; then
    HOST_ADDR="$(hostname -I 2>/dev/null | awk '{print $1}')"
fi
[ -n "$HOST_ADDR" ] || HOST_ADDR="$(hostname)"
printf '  pull it (run this on the Mac):\n'
printf '    scp %s@%s:%s ~/Downloads/\n' "$(whoami)" "$HOST_ADDR" "$ZIP"
printf '  then:  unzip -q ~/Downloads/%s -d ~/Downloads && cd ~/Downloads/%s && ./install.sh\n' \
       "${NAME}.zip" "$NAME"
