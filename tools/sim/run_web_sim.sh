#!/bin/bash
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$DIR"

echo "======================================================="
echo "  Starting devOS Remote Web UI Simulator"
echo "  Resolution: 1280x720 @ 60 FPS"
echo "======================================================="

# 1. Build devos_sim if not built or if sources changed
if [ ! -d "build_sim" ]; then
    echo "[SIM] Configuring build_sim with CMake & Ninja..."
    cmake -B build_sim -S . -G Ninja -DDEVOS_SIMULATOR=ON
fi

echo "[SIM] Compiling devos_sim..."
ninja -C build_sim devos_sim

# 2. Start Xvfb Virtual Framebuffer (:99 @ 1280x720) if not running
if ! pgrep -f "Xvfb :99" >/dev/null; then
    echo "[SIM] Starting Xvfb on display :99 (1280x720x24)..."
    nohup Xvfb :99 -screen 0 1280x720x24 -ac +extension GLX +render -noreset >/tmp/xvfb.log 2>&1 &
    sleep 1
else
    echo "[SIM] Xvfb already running on :99."
fi

# 3. Start x11vnc on port 5900 if not running
if ! pgrep -f "x11vnc.*:99" >/dev/null; then
    echo "[SIM] Starting x11vnc on port 5900..."
    nohup x11vnc -display :99 -forever -nopw -shared -rfbport 5900 >/tmp/x11vnc.log 2>&1 &
    sleep 1
else
    echo "[SIM] x11vnc already running on port 5900."
fi

# 4. Start websockify / noVNC on port 6080 if not running
NOVNC_DIR="/usr/share/novnc"
if ! pgrep -f "websockify.*6080" >/dev/null; then
    echo "[SIM] Starting websockify on port 6080 with web directory $NOVNC_DIR..."
    nohup websockify --web="$NOVNC_DIR" 6080 localhost:5900 >/tmp/websockify.log 2>&1 &
    sleep 1
else
    echo "[SIM] websockify already running on port 6080."
fi

# 5. Stop existing devos_sim process if running
if pgrep -x "devos_sim" >/dev/null; then
    echo "[SIM] Stopping previous devos_sim instance..."
    pkill -x "devos_sim" || true
    sleep 1
fi

# 6. Launch devos_sim with nohup
echo "[SIM] Launching devos_sim on DISPLAY=:99..."
nohup env DISPLAY=:99 ./build_sim/devos_sim > /tmp/devos_sim.log 2>&1 &
SIM_PID=$!

echo ""
echo "======================================================="
echo "  devOS Web Simulator is LIVE!"
echo "  PID: $SIM_PID"
echo "  Developer Verification URL:"
echo "  >>> http://100.77.11.92:6080/vnc.html <<<"
echo "======================================================="
