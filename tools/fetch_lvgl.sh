#!/usr/bin/env bash
# Fetch LVGL (v9.2, the release devOS is built and tested against) into
# components/lvgl. It isn't vendored in the repo: both the firmware and the
# simulator build it from there.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/components/lvgl"
COMMIT="933b2352bfec08b231e289546286aa0ef63d5ef8"   # release/v9.2 (9.2.3-dev)

if [ -f "$DEST/lvgl.h" ]; then
    echo "LVGL already present in components/lvgl"
    exit 0
fi
rm -rf "$DEST"
git init -q "$DEST"
git -C "$DEST" fetch -q --depth 1 https://github.com/lvgl/lvgl.git "$COMMIT"
git -C "$DEST" checkout -q FETCH_HEAD
echo "LVGL ${COMMIT:0:7} -> components/lvgl"
