# AGENTS.md: Developer Agent Directives for devOS Firmware

Welcome, coding agent. This document serves as the operational handbook, architectural guide, and rulebook for implementing **`devOS`**—the custom operating system and firmware for the **M5Stack Tab5** with **A164 70-Key Physical Keyboard**.

Always cross-reference [PLAN.md](file:///home/dom/dev/tab5-devos/PLAN.md) for detailed feature specifications and system state.

---

## 1. Core Architectural Invariants (Non-Negotiable Rules)

1. **Dual-Core FreeRTOS Task Pinning:**
   * **Core 0 (System, Network & Crypto):** All Wi-Fi (ESP-Hosted to C6), lwIP networking, MicroLink (Tailscale/WireGuard), mbedTLS crypto, SSH client I/O, WebSocket/SSE streaming, and power management tasks.
   * **Core 1 (Presentation & Interaction):** LVGL v9 GUI loop, PPA 2D blit acceleration, GT911 touch input handling, Tab5 A164 keyboard polling, and local MicroSD I/O.
   * *Rule:* Never block Core 1 with network calls or synchronous TLS handshakes. All cross-core communication must use FreeRTOS queues (`xQueueSendToBack`), task notifications, or event groups.

2. **Strict Memory Partitioning (Internal SRAM vs. External PSRAM):**
   * The ESP32-P4 has **32 MB external PSRAM** and ~768 KB internal SRAM.
   * *Allocate in PSRAM (`MALLOC_CAP_SPIRAM`):* LVGL draw buffers, terminal scrollback buffer (up to 10,000 lines), diff text buffers, session caches, and markdown AST nodes.
   * *Reserve Internal SRAM (`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`):* Keep at least **120 KB contiguous internal SRAM free** for mbedTLS / SSH handshakes, Wi-Fi SDIO DMA descriptors, and FreeRTOS task stacks.

3. **Responsive Tri-Pane UI & "Focus Mode":**
   * Both `app_opendev` and `app_antigravity` must share the unified `devos_agent_viewport` container from `components/devos_ui/`.
   * Support all 4 layout states: Tri-Pane (260px | 720px | 300px), Left-Only (260px | 1020px), Right-Only (980px | 300px), and Focus Mode (full-width 1280px).
   * Respect global hotkeys: `Fn + F` (Focus Mode), `Fn + [` (Left Sidebar), `Fn + ]` (Right Inspector).

4. **Dynamic Terminal PTY Resizing:**
   * `app_terminal` must send a `TIOCSWINSZ` window size update (`libssh2_channel_request_pty_size`) whenever the connections side panel is toggled, transitioning between 160 columns (collapsed) and 128 columns (open).

5. **Theme Propagating Everywhere:**
   * Any new widget, card, or screen must register with `devos_theme`.
   * Must support both **Dark Cyberdeck** and **High-Contrast Light** palettes. Colors must update instantly when `devos_theme_toggle()` is called (`Fn + T`).

6. **Automatic MicroSD Scaffolding:**
   * When a MicroSD card is mounted, `devos_storage_bootstrap()` must automatically create all missing folders (`/.ssh/`, `/notes/`, `/plans/`, `/diffs/`, `/.devos/`) and missing starter templates (`welcome.md`, `bookmarks.json`). Zero manual file creation on PC/Mac.

---

## 2. Hardware Interfaces & Pinout Reference

| Peripheral | Controller | Interface & Pins | Driver / Notes |
| :--- | :--- | :--- | :--- |
| **Main SoC** | ESP32-P4 | RISC-V Dual-core @ 400 MHz | Target `esp32p4` in ESP-IDF v5.4+ |
| **Co-SoC (Wi-Fi 6)**| ESP32-C6 | SDIO (ESP-Hosted) | P4 acts as host, C6 as slave |
| **Display** | 5.0" 1280×720 IPS | MIPI-DSI (ST7123/EK79007) | 2 partial line buffers in PSRAM, PPA 2D enabled |
| **Touch** | Goodix GT911 | I2C | 5-point multi-touch input driver for LVGL |
| **Keyboard** | A164 (70 Keys) | Ext.Port1 I2C (`0x6D`) | SDA: GPIO 0, SCL: GPIO 1, INT: GPIO 50 (STM32F030) |
| **Power Telemetry** | TI INA226 | I2C | Monitors NP-F550 voltage, current, and wattage |
| **RTC** | RX8130CE | I2C | Offline hardware clock with battery backup |
| **Storage** | MicroSD Slot | 4-bit SDMMC | Mounts at `/sdcard` via VFS FATFS |

---

## 3. Directory Layout & Module Responsibilities

```
tab5-devos/
├── CMakeLists.txt                 # Root ESP-IDF CMake configuration
├── sdkconfig.defaults             # Global target settings, PSRAM, FreeRTOS affinity
├── partitions.csv                 # Custom partition table (app, ota_0, ota_1, nvs, storage)
├── components/
│   ├── bsp_tab5/                  # Tab5 board drivers (MIPI-DSI, GT911, INA226, RTC)
│   ├── tab5_keyboard/             # A164 I2C keyboard driver, interrupt & HID decoder
│   ├── devos_core/                # OS kernel, event bus, app switcher, hotkey dispatcher
│   ├── devos_ui/                  # LVGL v9 theme engine, widgets, top bar, home dashboard
│   ├── devos_net/                 # Wi-Fi manager, DNS, lwIP virtual socket routing
│   ├── devos_storage/             # MicroSD SDMMC mount, auto-scaffolding bootstrap
│   ├── microlink/                 # Tailscale / WireGuard client component
│   ├── libssh2_port/              # libssh2 SSH client component & PTY manager
│   └── markdown_parser/           # CommonMark token parser for LVGL text renderer
├── main/
│   ├── main.c                     # Hardware bring-up, task creation, launch
│   ├── apps/
│   │   ├── app_launcher/          # Home Screen dashboard & live app tiles
│   │   ├── app_opendev/           # OpenCode / OpenChamber client (tri-pane + focus)
│   │   ├── app_terminal/          # Multi-session SSH client (collapsible panel)
│   │   ├── app_editor/            # MicroSD Markdown editor & previewer
│   │   ├── app_tailscale/         # Tailnet status & peer list
│   │   ├── app_antigravity/       # Native Antigravity GUI client (Path B)
│   │   └── app_settings/          # Wi-Fi setup, display, power, system telemetry
│   └── include/
│       └── devos_config.h         # System-wide pin mappings, buffers, constants
└── tools/
    ├── agy_bridge/                # Host-side Python daemon for Antigravity (Path B)
    │   ├── bridge_server.py       # FastAPI WebSocket bridge listening on 100.x.y.z:8420
    │   ├── transcript_watcher.py  # Realtime parser for transcript.jsonl
    │   └── requirements.txt       # Python dependencies
    └── flash_c6_slave.sh          # Helper script to flash ESP-Hosted slave to C6
```

---

## 4. Build, Simulation & Development Workflows

### 4.1 Target Firmware Build (ESP-IDF v5.4+)
```bash
# Set target on first setup
idf.py set-target esp32p4

# Build project
idf.py build

# Generate flashing package
# Artifacts: build/tab5-devos.bin, build/bootloader/bootloader.bin, build/partition_table/partition-table.bin
```

### 4.2 Remote Web UI Simulation (noVNC over Tailscale — Option 1)
To allow the developer to test and verify UI/UX progress in real-time from their local macOS browser without flashing hardware:
* **The Web Simulator Stack:**
  * Runs on the headless Linux server using `Xvfb` (Virtual Framebuffer @ 1280×720), `x11vnc`, and `websockify` / `noVNC`.
  * Renders the native `devos_sim` binary at 60 FPS in an HTML5 browser canvas.
  * Captures mouse clicks as GT911 capacitive touch events, and keyboard input as A164 physical keyboard strokes and hotkeys (`Fn + T`, `Fn + F`, `1..6`).
* **Start Web Simulator Service:**
  ```bash
  # Build simulator target
  cmake -B build_sim -S . -DDEVOS_SIMULATOR=ON
  ninja -C build_sim

  # Launch or restart web simulator background daemon
  ./tools/sim/run_web_sim.sh
  ```
* **Developer Verification URL:**
  * Accessible over Tailscale at: **`http://100.77.11.92:6080/vnc.html`** (or `http://dev-server:6080/vnc.html`).
* **Native Desktop Simulator (Local macOS / Linux with Display):**
  ```bash
  # Run directly if display server is present
  ./build_sim/devos_sim
  ```

### 4.3 Antigravity Bridge Daemon (`tools/agy_bridge/`)
To run the host sidecar daemon:
```bash
cd tools/agy_bridge
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python3 bridge_server.py --port 8420 --host 100.77.11.92
```

---

## 5. Coding & Engineering Standards

### 5.1 C99 / C++ Conventions
* Write clean, idiomatic C99 for drivers, networking, and system code.
* Use `snake_case` for functions and variables; prefix modules consistently:
  * `devos_*` for core OS and UI systems
  * `tab5_*` for board-level hardware drivers
  * `app_*` for user applications
* Use `typedef struct devos_..._t` for structs.
* All header files must have `#pragma once` or standard include guards.

### 5.2 Error Handling & Logging
* Every function returning an error must use `esp_err_t`.
* Check return values with `ESP_ERROR_CHECK` during initialization, or handle gracefully at runtime.
* Use standard tags for logging:
  ```c
  static const char *TAG = "devos_terminal";
  ESP_LOGI(TAG, "PTY resized: cols=%d, rows=%d", cols, rows);
  ```

### 5.3 LVGL Object Lifecycle Management
* Always clean up timers, event callbacks, and allocated memory when an app screen is destroyed or replaced.
* Use `lv_obj_clean()` and `lv_obj_delete()`.
* Store pointers to dynamically allocated LVGL widgets inside the app's context struct, never as loose global variables.

### 5.4 Security & Secrets
* **Never commit passwords, tokens, or private keys to git.**
* All persistent secrets (Tailscale auth keys, SSH keys, OpenChamber pairing tokens, AGY PSK) must be saved into encrypted NVS partitions (`nvs_flash`) or encrypted SD card storage.

---

## 6. Implementation Workflow for Agents

When implementing tasks from [PLAN.md](file:///home/dom/dev/tab5-devos/PLAN.md):

1. **Pick one atomic phase/component at a time** (e.g. Phase 0 Hardware Spike, Phase 1 Home Screen, Phase 3 Terminal).
2. **Review dependencies first**—ensure required hardware pins, FreeRTOS queues, and header interfaces exist before writing high-level app logic.
3. **Write modular code** in `components/` before connecting to `main.c`.
4. **Test in simulation first:**
   * After creating or modifying any UI/UX component, compile the simulator (`ninja -C build_sim`).
   * Verify the UI via `./tools/sim/run_web_sim.sh`.
   * Notify the developer with the direct verification URL (**`http://100.77.11.92:6080/vnc.html`**) so they can interactively test the UI from their Mac browser without flashing.
5. **Update PLAN.md** milestone checkboxes as features are completed and verified.
