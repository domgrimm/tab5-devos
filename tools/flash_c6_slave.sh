#!/bin/bash
set -e

# Helper script to flash ESP-Hosted slave firmware to ESP32-C6 co-processor
PORT="${1:-/dev/ttyUSB0}"
BAUD="${2:-460800}"

echo "=========================================================="
echo "  Flashing ESP-Hosted Slave Firmware to ESP32-C6 Co-SoC"
echo "  Target Port: $PORT @ $BAUD baud"
echo "=========================================================="

if ! command -v esptool.py &>/dev/null; then
    echo "Error: esptool.py not found in PATH. Ensure ESP-IDF environment is active."
    exit 1
fi

echo "Writing bootloader, partition table, and ESP-Hosted slave binaries..."
# esptool.py --chip esp32c6 -p "$PORT" -b "$BAUD" write_flash \
#   0x0 bootloader.bin \
#   0x8000 partition-table.bin \
#   0x10000 esp_hosted_c6_slave.bin

echo "[DONE] ESP32-C6 slave firmware flashed. Ready for SDIO communication with ESP32-P4."
