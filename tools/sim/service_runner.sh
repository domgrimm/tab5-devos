#!/bin/bash

# Cleanup any stale X11 lock files
rm -f /tmp/.X99-lock /tmp/.X11-unix/X99 2>/dev/null || true

# 1. Virtual Framebuffer (1280x720 matching Tab5 display)
Xvfb :99 -screen 0 1280x720x24 -ac +extension GLX +render -noreset &
XVFB_PID=$!

sleep 1

# 2. VNC server on port 5900
x11vnc -display :99 -forever -nopw -shared -rfbport 5900 -quiet &
VNC_PID=$!

# 3. noVNC HTML5 WebSocket bridge on port 6080
websockify --web=/usr/share/novnc 6080 localhost:5900 &
WS_PID=$!

# Ensure helper daemons terminate when service terminates
cleanup() {
    kill $XVFB_PID $VNC_PID $WS_PID 2>/dev/null || true
}
trap cleanup EXIT INT TERM

# 4. Main devOS simulator process
DISPLAY=:99 /home/dom/dev/tab5-devos/build_sim/devos_sim
