#!/bin/bash
#
# Flash the ESP-Hosted *slave* firmware onto the Tab5's ESP32-C6 co-processor.
#
# Wi-Fi on the Tab5 runs on the C6; the ESP32-P4 talks to it over SDIO via the
# esp_hosted component (see components/devos_net/idf_component.yml and the
# CONFIG_ESP_HOSTED_* block in sdkconfig.defaults). The C6 must run the matching
# ESP-Hosted slave firmware or esp_wifi_init() on the P4 will fail.
#
# The slave sources are pulled into managed_components/ when you build the P4
# app (idf.py build). This script builds that slave for the C6 and flashes it.
#
# Usage:
#   tools/flash_c6_slave.sh [C6_SERIAL_PORT] [BAUD]
# e.g.
#   tools/flash_c6_slave.sh /dev/ttyACM1 460800
#
# NOTE on wiring: how the C6 is exposed for flashing is board-specific. On the
# Tab5 the C6 download UART is not the same USB-C port used for the P4 — consult
# M5Stack's Tab5 docs for the C6 flashing header / jumper. Point this script at
# that port.

set -euo pipefail

PORT="${1:-/dev/ttyACM1}"
BAUD="${2:-460800}"
PROJ_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SLAVE_DIR="$PROJ_ROOT/managed_components/espressif__esp_hosted/slave"

echo "=========================================================="
echo "  ESP-Hosted slave -> ESP32-C6   (port: $PORT @ $BAUD)"
echo "=========================================================="

if ! command -v idf.py >/dev/null 2>&1; then
    echo "ERROR: idf.py not on PATH. Run 'source \$IDF_PATH/export.sh' first." >&2
    exit 1
fi

if [ ! -d "$SLAVE_DIR" ]; then
    echo "ERROR: esp_hosted slave not found at:" >&2
    echo "  $SLAVE_DIR" >&2
    echo "Build the P4 app first so the component manager downloads it:" >&2
    echo "  idf.py set-target esp32p4 && idf.py build" >&2
    exit 1
fi

echo "[1/3] Building slave for esp32c6 (transport: SDIO, to match the P4 host)..."
idf.py -C "$SLAVE_DIR" -B "$SLAVE_DIR/build_c6" set-target esp32c6 build

echo "[2/3] Flashing C6 on $PORT ..."
idf.py -C "$SLAVE_DIR" -B "$SLAVE_DIR/build_c6" -p "$PORT" -b "$BAUD" flash

echo "[3/3] Done. Power-cycle the Tab5, then check the P4 log for:"
echo "        \"Wi-Fi station started (radio on ESP32-C6 via ESP-Hosted)\""
echo "      and no \"esp_wifi_init failed\" line."
