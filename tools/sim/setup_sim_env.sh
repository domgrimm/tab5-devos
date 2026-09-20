#!/bin/bash
set -e

echo "[devOS] Installing simulation prerequisites via dnf..."
sudo dnf install -y \
    cmake \
    ninja-build \
    gcc \
    gcc-c++ \
    SDL2 \
    SDL2-devel \
    xorg-x11-server-Xvfb \
    x11vnc \
    novnc \
    python3-websockify

echo "[devOS] Prerequisites successfully installed."
