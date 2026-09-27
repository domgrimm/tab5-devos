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
if ! pgrep -f "[X]vfb :99" >/dev/null 2>&1; then
    echo "[SIM] Starting Xvfb on display :99 (1280x720x24)..."
    setsid Xvfb :99 -screen 0 1280x720x24 -ac +extension GLX +render -noreset >/tmp/xvfb.log 2>&1 &
    sleep 1
else
    echo "[SIM] Xvfb already running on :99."
fi

# 3. Start x11vnc on port 5900 if not running
if ! pgrep -f "[x]11vnc.*:99" >/dev/null 2>&1; then
    echo "[SIM] Starting x11vnc on port 5900..."
    setsid x11vnc -display :99 -forever -nopw -shared -rfbport 5900 >/tmp/x11vnc.log 2>&1 &
    sleep 1
else
    echo "[SIM] x11vnc already running on port 5900."
fi

# 4. Start websockify / noVNC on port 6080 if not running
NOVNC_DIR="/usr/share/novnc"
if ! pgrep -f "[w]ebsockify.*6080" >/dev/null 2>&1; then
    echo "[SIM] Starting websockify on port 6080 with web directory $NOVNC_DIR..."
    setsid websockify --web="$NOVNC_DIR" 6080 localhost:5900 >/tmp/websockify.log 2>&1 &
    sleep 1
else
    echo "[SIM] websockify already running on port 6080."
fi

# 5. Stop existing devos_sim process if running
if pgrep -x "devos_sim" >/dev/null; then
    echo "[SIM] Stopping previous devos_sim instance..."
    pkill -x "devos_sim" || true
    while pgrep -x "devos_sim" >/dev/null; do sleep 0.2; done
fi

# 6. Launch devos_sim with setsid -f in background
echo "[SIM] Launching devos_sim on DISPLAY=:99..."
DISPLAY=:99 setsid -f ./build_sim/devos_sim >/tmp/devos_sim.log 2>&1
sleep 1
SIM_PID=$(pgrep -x "devos_sim" | tail -n 1)


echo ""
echo "======================================================="
echo "  devOS Web Simulator is LIVE!"
echo "  PID: $SIM_PID"
echo "  Developer Verification URLs:"
for ip in $(hostname -I 2>/dev/null); do
    case "$ip" in 172.*|*:*) continue ;; esac     # skip container bridges and IPv6
    echo "  >>> http://$ip:6080/vnc.html <<<"
done
echo "======================================================="
