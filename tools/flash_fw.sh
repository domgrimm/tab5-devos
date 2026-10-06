#!/usr/bin/env bash
#
# Install a devOS build onto a Tab5 from a folder of firmware images.
#
# This file ships inside the release zip next to the images, as `install.sh`.
# It needs only python3 + esptool on the machine doing the flashing - no repo,
# no ESP-IDF, no checkout:
#
#   ./install.sh                       # auto-detect the port and flash
#   ./install.sh -p /dev/cu.usbmodem1234
#   ./install.sh --erase               # full chip erase first (see below)
#   ./install.sh --dry-run             # print the esptool command, run nothing
#
# Offsets and flash settings come from the `flash_args` file next to the images
# when present (that is what `idf.py build` wrote), otherwise from the defaults
# below, so the layout stays authoritative.
#
# --erase wipes the whole chip, including NVS: Wi-Fi credentials, the Tailscale
# state and the secrets device key are all lost and must be re-provisioned. It
# is only needed when the partition layout changed in an incompatible way; the
# usual install does not need it.

set -euo pipefail

# `set -e` aborts silently, which is indistinguishable from "nothing happened".
# Make any unexpected failure say so, with the line, so it can never again look
# like the script did nothing.
trap 'rc=$?; printf "%s: unexpected failure (exit %d) at line %s\n" "$(basename "$0")" "$rc" "$LINENO" >&2' ERR

DIR="$(cd "$(dirname "$0")" && pwd)"

PORT=""
BAUD="460800"
CHIP="esp32p4"
ERASE=0
MONITOR=0
DRY_RUN=0
ASSUME_YES=0
LIST_ONLY=0

# Fallbacks, used only when there is no flash_args next to the images.
FLASH_SETTINGS="--flash_mode dio --flash_freq 80m --flash_size 16MB"
IMAGE_LIST="0x2000 bootloader.bin
0x8000 partition-table.bin
0xf000 ota_data_initial.bin
0x20000 tab5-devos.bin"

usage() {
    cat <<EOF
Install a devOS build onto an ESP32-P4 Tab5 over USB.

Usage: $(basename "$0") [options]

  -p, --port PORT     serial port (default: auto-detect)
  -b, --baud BAUD     flash baud rate (default: 460800)
      --chip CHIP     esptool chip (default: esp32p4)
      --dir DIR       folder holding the images (default: this script's folder)
      --list          list the serial ports found and exit
      --erase         full chip erase before flashing (wipes NVS)
      --monitor       open a serial monitor after flashing
      --dry-run       print the esptool command instead of running it
  -y, --yes           don't ask for confirmation
  -h, --help          this text

Environment:
  ESPTOOL             esptool command to use (default: auto-detect)
EOF
}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        -p|--port)  PORT="${2:-}"; shift 2 ;;
        -b|--baud)  BAUD="${2:-}"; shift 2 ;;
        --chip)     CHIP="${2:-}"; shift 2 ;;
        --dir)      DIR="${2:-}"; shift 2 ;;
        --list)     LIST_ONLY=1; shift ;;
        --erase)    ERASE=1; shift ;;
        --monitor)  MONITOR=1; shift ;;
        --dry-run)  DRY_RUN=1; shift ;;
        -y|--yes)   ASSUME_YES=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *)          usage >&2; die "unknown option: $1" ;;
    esac
done

[ -d "$DIR" ] || die "no such folder: $DIR"

# ---- images and flash settings -------------------------------------------
# flash_args is one optional settings line then "offset path" pairs:
#   --flash_mode dio --flash_freq 80m --flash_size 16MB
#   0x2000 bootloader/bootloader.bin
# Paths are resolved relative to this folder, so both a raw build/ dir (which
# keeps bootloader/ and partition_table/ subdirectories) and the flattened
# release folder work.
if [ -f "$DIR/flash_args" ]; then
    settings=""
    images=""
    while IFS= read -r line; do
        case "$line" in
            --flash_*) settings="$line" ;;
            0x*)       images="$images${line%% *} ${line#* }
" ;;
        esac
    done < "$DIR/flash_args"
    [ -n "$settings" ] && FLASH_SETTINGS="$settings"
    [ -n "$images" ] && IMAGE_LIST="${images%
}"
fi

# Every image must be present; report all the misses at once.
missing=""
while read -r off name; do
    [ -n "${name:-}" ] || continue
    [ -f "$DIR/$name" ] || missing="$missing ${name##*/}"
done <<EOF
$IMAGE_LIST
EOF
if [ "$LIST_ONLY" -eq 0 ] && [ -n "$missing" ]; then
    if [ ! -f "$DIR/flash_args" ]; then
        die "no firmware images in $DIR. This script is normally shipped next to them;
       from the repo, point it at the build:  --dir build"
    fi
    die "missing firmware image(s) in $DIR:$missing"
fi

# ---- the serial port ------------------------------------------------------
# The ESP32-P4's built-in USB-Serial/JTAG is a CDC-ACM device: /dev/cu.usbmodem*
# on macOS, /dev/ttyACM* on Linux. A separate USB-UART bridge (CP210x, CH34x,
# FTDI) enumerates under a different name, so the device name alone identifies
# the board - no extra tools needed. On Linux the USB vendor id is read too and
# an Espressif (303a:) device also counts as the board.

# Human description of a port.
port_kind() {
    case "$1" in
        /dev/cu.usbmodem*)       printf 'Espressif USB-Serial/JTAG  <- the Tab5' ;;
        /dev/ttyACM*)            printf 'CDC-ACM serial  <- the Tab5' ;;
        /dev/cu.wchusbserial*)   printf 'CH34x USB-UART bridge' ;;
        /dev/cu.SLAB_USBtoUART*) printf 'CP210x USB-UART bridge' ;;
        /dev/cu.usbserial*)      printf 'USB-UART bridge' ;;
        /dev/ttyUSB*)            printf 'USB-UART bridge' ;;
        *)                       printf 'USB serial' ;;
    esac
}

# "vid:pid" from sysfs on Linux, empty elsewhere (not needed there).
port_vidpid() {
    local base d vid pid
    base="/sys/class/tty/$(basename "$1")/device"
    for d in "$base" "$base/.."; do
        [ -r "$d/idVendor" ] || continue
        vid="$(cat "$d/idVendor" 2>/dev/null || true)"
        pid="$(cat "$d/idProduct" 2>/dev/null || true)"
        if [ -n "$vid" ] && [ -n "$pid" ]; then printf '%s:%s' "$vid" "$pid"; return 0; fi
    done
    return 0
}

# 0 when this port is very likely the Tab5.
port_is_board() {
    case "$1" in
        /dev/cu.usbmodem*|/dev/ttyACM*) return 0 ;;
    esac
    case "$(port_vidpid "$1")" in
        303a:*) return 0 ;;
    esac
    return 1
}

find_ports() {
    # Always succeeds: an unmatched glob makes ls exit non-zero, and a failing
    # command substitution in an assignment aborts the whole script under
    # `set -e` - silently, with no output at all.
    case "$(uname -s)" in
        Darwin) ls /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* \
                   /dev/cu.usbserial* 2>/dev/null || true ;;
        Linux)  ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true ;;
    esac
}

describe_ports() {
    local p vp
    for p in $(find_ports); do
        vp="$(port_vidpid "$p")"
        printf '    %-28s %s%s\n' "$p" "$(port_kind "$p")" "${vp:+   [$vp]}"
    done
}

if [ "$LIST_ONLY" -eq 1 ]; then
    printf 'Serial ports:\n'
    describe_ports
    printf '\n'
    printf 'Use one with:  %s -p <port>\n' "$(basename "$0")"
    exit 0
fi

if [ -z "$PORT" ]; then
    found="$(find_ports)"
    n=0
    boards=""
    for p in $found; do
        n=$((n + 1))
        if port_is_board "$p"; then boards="$boards $p"; fi
    done
    boards="${boards# }"

    if [ "$n" -eq 0 ]; then
        die "no serial port found.
       Plug the Tab5 into its USB-C port, then check it appears:
         ls /dev/cu.usbmodem*     (macOS)
         ls /dev/ttyACM*          (Linux)
       A charge-only cable has no data lines - try another cable."
    elif [ "$n" -eq 1 ]; then
        PORT="$found"
    else
        bn=0
        for p in $boards; do bn=$((bn + 1)); done
        if [ "$bn" -eq 1 ]; then
            PORT="$boards"
            printf '%d serial ports found; using the one that looks like the Tab5:\n' "$n"
            describe_ports
            printf '\n'
        else
            if [ "$bn" -eq 0 ]; then
                printf 'error: %d serial ports found and none looks like a Tab5.\n\n' "$n" >&2
            else
                printf 'error: %d serial ports look like a Tab5 - pick one.\n\n' "$bn" >&2
            fi
            describe_ports >&2
            printf '\nPass the one you want with -p, e.g.\n  %s -p %s\n' \
                   "$(basename "$0")" "$(find_ports | head -1)" >&2
            exit 1
        fi
    fi
fi

# ---- esptool --------------------------------------------------------------
# esptool 5 dropped the esptool.py shim, so try module invocation too.
ESPTOOL="${ESPTOOL:-}"
if [ -z "$ESPTOOL" ]; then
    if command -v esptool.py >/dev/null 2>&1; then
        ESPTOOL="esptool.py"
    elif command -v esptool >/dev/null 2>&1; then
        ESPTOOL="esptool"
    elif command -v python3 >/dev/null 2>&1 && python3 -m esptool --help >/dev/null 2>&1; then
        ESPTOOL="python3 -m esptool"
    fi
fi

if [ -z "$ESPTOOL" ] && [ "$DRY_RUN" -eq 0 ]; then
    die "esptool not found. Install it once:
    python3 -m pip install --user esptool
  or
    brew install esptool
then run this again (or set ESPTOOL to its path)."
fi
[ -n "$ESPTOOL" ] || ESPTOOL="esptool"        # only reached by --dry-run

# esptool v5 spells subcommands with hyphens; keep both spellings working.
WRITE_CMD="write_flash"
ERASE_CMD="erase_flash"
if [ "$DRY_RUN" -eq 0 ]; then
    help_text="$($ESPTOOL --help 2>&1 || true)"
    case "$help_text" in
        *write-flash*) WRITE_CMD="write-flash" ;;
    esac
    case "$help_text" in
        *erase-flash*) ERASE_CMD="erase-flash" ;;
    esac
fi

# ---- show the plan -------------------------------------------------------
printf 'devOS install\n'
printf '  port    : %s   (%s)\n' "$PORT" "$(port_kind "$PORT")"
printf '  baud    : %s\n' "$BAUD"
printf '  chip    : %s\n' "$CHIP"
printf '  images  :\n'
while read -r off name; do
    [ -n "${name:-}" ] || continue
    printf '            %-8s %s\n' "$off" "${name##*/}"
done <<EOF
$IMAGE_LIST
EOF
[ "$ERASE" -eq 1 ] && printf '  erase   : YES - this wipes NVS (Wi-Fi, Tailscale, secrets key)\n'

if [ "$ASSUME_YES" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
    printf 'Flash this? [y/N] '
    read -r reply </dev/tty || reply=""
    case "$reply" in
        y|Y|yes|YES) ;;
        *) echo "aborted."; exit 1 ;;
    esac
fi

# ---- flash ---------------------------------------------------------------
# split FLASH_SETTINGS into separate arguments
# shellcheck disable=SC2086
settings_args="$FLASH_SETTINGS"

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '+ %s\n' "$*"
    else
        "$@"
    fi
}

if [ "$ERASE" -eq 1 ]; then
    # shellcheck disable=SC2086
    run $ESPTOOL --chip "$CHIP" -p "$PORT" -b "$BAUD" $ERASE_CMD
fi

# Build the write_flash argument list from the image pairs.
set --                       # positional args become the esptool argv tail
# shellcheck disable=SC2086
set -- $ESPTOOL --chip "$CHIP" -p "$PORT" -b "$BAUD" \
       --before default_reset --after hard_reset \
       $WRITE_CMD $settings_args
while read -r off name; do
    [ -n "${name:-}" ] || continue
    set -- "$@" "$off" "$DIR/$name"
done <<EOF
$IMAGE_LIST
EOF

run "$@"

if [ "$DRY_RUN" -eq 0 ]; then
    printf '\nDone. The Tab5 reboots into the new firmware.\n'
    printf 'Monitor: screen %s 115200      (exit: Ctrl-A then k, then y)\n' "$PORT"
    if [ "$MONITOR" -eq 1 ] && command -v screen >/dev/null 2>&1; then
        exec screen "$PORT" 115200
    fi
fi
