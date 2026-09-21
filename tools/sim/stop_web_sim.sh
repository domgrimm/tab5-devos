#!/bin/bash

echo "======================================================="
echo "  Stopping devOS Remote Web UI Simulator"
echo "======================================================="

# Stop all simulator processes
pkill -x devos_sim 2>/dev/null && echo "[STOP] devos_sim stopped." || true
pkill -f "x11vnc.*5900" 2>/dev/null && echo "[STOP] x11vnc stopped." || true
pkill -f "websockify.*6080" 2>/dev/null && echo "[STOP] websockify stopped." || true
pkill -f "Xvfb :99" 2>/dev/null && echo "[STOP] Xvfb :99 stopped." || true

# Remove stale locks
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99 2>/dev/null || true

echo "[DONE] Simulator and web streaming stack completely stopped."
