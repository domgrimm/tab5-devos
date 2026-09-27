# AGENTS.md: Developer Agent Directives for devOS Firmware

Welcome, coding agent. This document serves as the operational handbook, architectural guide, and rulebook for implementing **`devOS`**—the custom operating system and firmware for the **M5Stack Tab5** with **A164 70-Key Physical Keyboard**.

Always cross-reference [PLAN.md](PLAN.md) for detailed feature specifications and system state.

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

3. **Collapsible Side Panels:**
   * The editor's file list and the terminal's connections panel are 260px left panels (`DEVOS_PANE_LEFT_WIDTH`). `Sym + L` shows / hides them (the terminal's first press opens the panel and moves keyboard focus into it, a second press hides it; `Esc` there returns to the shell); `Sym + F` also hides the editor's file list. An app that adds a side panel uses the same width and the same key.
   * The Tab5 keyboard has **no Fn key**: every system shortcut is `Sym + <letter/digit/arrow>` (Sym + punctuation must keep typing its symbol, e.g. `Sym + [` = `{`). User-facing text says "Sym".

4. **Dynamic Terminal PTY Resizing:**
   * `app_terminal` must send a `TIOCSWINSZ` window size update (`libssh2_channel_request_pty_size`) whenever the connections side panel is toggled, transitioning between 160 columns (collapsed) and 128 columns (open).

5. **Theme Propagating Everywhere:**
   * Any new widget, card, or screen must register with `devos_theme`.
   * Must support both **Dark Cyberdeck** and **High-Contrast Light** palettes. Colors must update instantly when `devos_theme_toggle()` is called (`Sym + T`).

6. **Automatic MicroSD Scaffolding:**
   * When a MicroSD card is mounted, `devos_storage_bootstrap()` must automatically create all missing folders (`/.ssh/`, `/notes/`, `/wireguard/`, `/.devos/`, `/.devos/logs/`) and missing starter templates (`notes/welcome.md`, `.ssh/bookmarks.json`). Zero manual file creation on PC/Mac.

7. **Shared Engines, Never Forked Renderers or Parsers:**
   * One CommonMark-subset renderer (`components/devos_mdview/`, `devos_md_render()`) serves the editor preview and any future rich-text view. Never copy it into an app — extend the component and its unit test (`tools/md_preview_test.c`).
   * One JSON reader (`components/devos_json/`), used today by `devos_ota`. Same rule: extend, don't duplicate.
   * One socket helper layer (`devos_net_socket_*`, incl. `send_all` and non-blocking `connect_start/wait`): all network code routes through it so SIGPIPE, SYN-stall, and routing fixes land once. It is also where VPN routing applies (`devos_net_set_route_hook`, used by the WireGuard tunnel), so a socket opened any other way bypasses the tunnel. A UDP or raw socket of your own calls `devos_net_socket_route()` before its first send.
   * One HTTP(S) client (`components/devos_http/`): REST calls, APIs and webhooks go through it (never `esp_http_client`, which bypasses the socket layer).
   * One set of screen building blocks (`devos_widgets.h` in `devos_ui`): new apps build buttons, fields, dialogs and lists with it so theming and focus behave the same everywhere.

8. **Modular Self-Registering Apps:**
   * All apps must implement the standardized `devos_app_descriptor_t` interface (init, show, hide, handle_key, get_telemetry_lines) and register via `devos_core_register_app()`.
   * Adding a new application must never require modifying the Home Screen (`app_launcher.c`) or hardcoding app IDs into closed enums. Use `main/apps/app_template/` as the canonical reference.
   * An app's icon is its descriptor's `draw_icon` (a vector icon from `components/devos_ui/devos_icons.c`; add yours there, on its 20 x 20 grid) or, failing that, its `icon` LV symbol. The launcher tiles, Settings > Apps and the top bar all draw icons through `devos_icon_create()`, so they match everywhere.
   * Apps can be switched off in Settings > Apps (a boot mask in devos_core, applied on restart). Always call `devos_core_register_app()`: it skips a switched-off app. Start an app's engine in `main.c` with `START_ENGINE(uid, init())`, and make the engine's status getters return "off" when its init never ran, so other code can call them without checks. Never look up another app by id; use its uid, and handle `devos_core_open_with()` returning false.

9. **Keyboard First, Touch Second:**
   * **Every screen, panel, dialog and control must be fully usable from the physical keyboard alone.** Touch is a secondary input: everything also works by touch, but nothing may be touch-only. A feature isn't done until it has been walked through keyboard-only in the simulator.
   * **One key model everywhere:**
     * Arrows move the selection or focus (Up / Down in lists and forms, all four in grids).
     * `Tab` / `Aa + Tab` move between regions or fields.
     * `Enter` activates: open, connect, confirm, press the focused button.
     * `Space` toggles checkboxes and switches (and pauses live views).
     * `Left` / `Right` change the focused value (slider, dropdown, switch).
     * `Esc` backs out one level: close the dialog, then leave the field or panel, then (unhandled) go to the Home Screen.
     * Frequent actions get a letter shortcut.
     * `Sym + <key>` stays reserved for system shortcuts (apps may use `Sym + L` for their side panel).
   * **Visible focus:** whatever the next key will act on is always highlighted: the accent focus ring from `devos_focus`, an app's selection border, or a text cursor.
   * **Discoverable:** every screen shows its keys (a hint line or footer); the welcome note lists the global ones.
   * **Dialogs** take keyboard focus when they open (first field or default button) and give it back when they close; `Enter` confirms, `Esc` cancels.
   * **Text entry** uses the hardware keyboard. On-screen keyboards appear only when no keyboard is attached (`tab5_keyboard_is_connected()`).
   * **Implementation:** forms, dialogs and button rows use `devos_focus` (`components/devos_ui/devos_focus.h`): register the controls in order, pass keys from `handle_key`. Custom lists (file lists, streams, peer rows) handle keys in `handle_key` and draw their own selection.

---

## 2. Hardware Interfaces & Pinout Reference

| Peripheral | Controller | Interface & Pins | Driver / Notes |
| :--- | :--- | :--- | :--- |
| **Main SoC** | ESP32-P4 | RISC-V Dual-core @ 400 MHz | Target `esp32p4` in ESP-IDF v5.4+ |
| **Co-SoC (Wi-Fi 6)**| ESP32-C6 | SDIO (ESP-Hosted) | P4 acts as host, C6 as slave |
| **Display** | 5.0" 1280×720 IPS | MIPI-DSI (ST7123/EK79007) | 2 partial line buffers in PSRAM, PPA 2D enabled |
| **Touch** | Goodix GT911 / ST7123 TDDI | I2C (internal bus) | Multi-touch input driver for LVGL |
| **Camera** | SC2356 ("SC202CS") | 1-lane MIPI-CSI, SCCB 0x36 (internal I2C) | `esp_cam_sensor` 0.9.0 (pinned: 1.x needs esp-idf-kconfig >= 2.5) + IDF CSI + ISP; `bsp_tab5_camera.h` |
| **I2C** | ESP32-P4 | Internal bus GPIO 31/32, Ext.Port1 GPIO 0/1 | **New `i2c_master` driver only** (the camera stack needs it; IDF aborts if the legacy `driver/i2c.h` is linked too). Use `bsp_tab5_i2c_bus_internal/external()` |
| **Keyboard** | A164 (70 Keys) | Ext.Port1 I2C (`0x6D`) | SDA: GPIO 0, SCL: GPIO 1, INT: GPIO 50 (STM32F030). Modifiers are **Sym / Aa / Ctrl / Alt, no Fn key**: run in Normal (matrix) mode, Sym = system modifier (`DEVOS_MOD_FN`), Aa = Shift (tap = caps lock) |
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
│   ├── bsp_tab5/                  # Tab5 board drivers (MIPI-DSI, touch, INA226, RTC, camera) on the i2c_master driver
│   ├── tab5_keyboard/             # A164 I2C keyboard driver, interrupt & HID decoder
│   ├── devos_config/              # devos_config.h: pins, buffers, constants, app id enum
│   ├── devos_core/                # OS kernel, event bus, app switcher, hotkey dispatcher, app on/off boot mask
│   ├── devos_ui/                  # LVGL v9 theme engine, top bar, code viewer, devos_focus, devos_widgets, devos_icons
│   ├── devos_net/                 # Wi-Fi manager, DNS, lwIP virtual socket routing (+HTTP GET)
│   ├── devos_storage/             # MicroSD SDMMC mount, auto-scaffolding bootstrap
│   ├── devos_json/                # Shared minimal JSON reader + pretty-printer (OTA, MQTT)
│   ├── devos_mqtt/                # MQTT 3.1.1 client engine (no LVGL)
│   ├── devos_wireguard/           # wg-quick parser, tunnel storage, WireGuard tunnel
│   ├── devos_http/                # HTTP/1.1 + HTTPS client (mbedTLS) over devos_net sockets
│   ├── devos_netdiag/             # ping, DNS, port scan, mDNS engines (no LVGL)
│   ├── devos_docker/              # Docker Engine / Portainer API client (no LVGL)
│   ├── devos_adsb/                # aircraft.json poller for the ADS-B radar (no LVGL)
│   ├── devos_maptiles/            # OpenStreetMap tile fetch + SD/RAM cache for map underlays (no LVGL)
│   ├── devos_cricket/             # ESPNcricinfo match lists + scorecards via ESPN's site API (no LVGL)
│   ├── devos_crypto/              # SHA-1/256/512, HMAC, PBKDF2, ChaCha20-Poly1305, base32
│   ├── devos_totp/                # encrypted TOTP vault (no LVGL)
│   ├── devos_audio/               # ES7210 / ES8388 voice memos (record + play WAV)
│   ├── devos_qr/                  # QR scanning: camera frames -> quirc (vendored in quirc/)
│   ├── devos_mdview/              # Shared CommonMark-subset renderer (`devos_md_render()`)
│   ├── devos_power/               # Power-mode state machine (active/dim/sleep)
│   ├── devos_ota/                 # OTA manifest check + target flash path
│   ├── devos_sysmon/              # 1 Hz system telemetry (battery, Wi-Fi, SD, heap, CPU, clock)
│   ├── devos_tailnet/             # Tailscale client on top of MicroLink
│   ├── devos_vterm/               # VT100 / xterm terminal emulator (no LVGL)
│   ├── microlink/                 # Tailscale client component
│   ├── wireguard_lwip/            # WireGuard for lwIP (MicroLink and devos_wireguard)
│   └── libssh2_port/              # libssh2 SSH client component & PTY manager
├── main/
│   ├── main.c                     # Hardware bring-up, task creation, launch
│   ├── apps/
│   │   ├── app_launcher/          # Home Screen dashboard & live app tiles
│   │   ├── app_terminal/          # Multi-session SSH client (collapsible panel)
│   │   ├── app_editor/            # SD card file browser, Markdown & text editor, scratchpad + voice memos
│   │   ├── app_tailscale/         # Tailnet status & peer list
│   │   ├── app_wireguard/         # WireGuard tunnels from wg-quick configs
│   │   ├── app_mqtt/              # MQTT monitor & publisher
│   │   ├── app_netdiag/           # Network: ping, DNS, port scan, Wi-Fi survey, mDNS
│   │   ├── app_rest/              # REST & webhook client
│   │   ├── app_docker/            # Docker / Portainer console
│   │   ├── app_adsb/              # ADS-B radar (dump1090 / readsb aircraft.json) + OSM underlay (adsb_map.c)
│   │   ├── app_cricket/           # Cricket: live scores, results by date, scorecards
│   │   ├── app_totp/              # Authenticator: offline TOTP from an encrypted vault
│   │   ├── app_settings/          # Wi-Fi setup, display, power, system telemetry
│   │   └── app_template/          # Starter drop-in template for modular third-party apps
│   └── include/
│       └── devos_config.h         # Forwards to components/devos_config/include/devos_config.h
└── tools/
    ├── *_test.c                   # Host-side unit tests (md_preview, modular_launcher, apps_mask, ota, vterm, crypto, cricket).
    │                              # Run from an ISOLATED CWD — engine tests persist
    │                              # sim config JSON relative to CWD. See each file's
    │                              # header for its exact gcc line.
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
  * Captures mouse clicks as GT911 capacitive touch events, and keyboard input as A164 physical keyboard strokes and hotkeys (`Sym + T`, `Sym + L`, `Sym + 1..6`).
* **Start Web Simulator Service:**
  ```bash
  # Build simulator target
  cmake -B build_sim -S . -DDEVOS_SIMULATOR=ON
  ninja -C build_sim

  # Launch or restart web simulator background daemon
  ./tools/sim/run_web_sim.sh
  ```
* **Developer Verification URL:**
  * Accessible at **`http://dev-server:6080/vnc.html`** (the dev server's LAN or Tailscale address; `run_web_sim.sh` prints both).
* **Native Desktop Simulator (Local macOS / Linux with Display):**
  ```bash
  # Run directly if display server is present
  ./build_sim/devos_sim
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
* All persistent secrets (Tailscale auth keys, SSH keys) must be saved into encrypted NVS partitions (`nvs_flash`) or encrypted SD card storage.

---

## 6. Implementation Workflow for Agents

When implementing tasks from [PLAN.md](PLAN.md):

1. **Pick one atomic phase/component at a time** (e.g. Phase 0 Hardware Spike, Phase 1 Home Screen, Phase 3 Terminal).
2. **Review dependencies first**—ensure required hardware pins, FreeRTOS queues, and header interfaces exist before writing high-level app logic.
3. **Write modular code** in `components/` before connecting to `main.c`.
4. **Test in simulation first:**
   * After creating or modifying any UI/UX component, compile the simulator (`ninja -C build_sim`).
   * Verify the UI via `./tools/sim/run_web_sim.sh`, **keyboard-only first** (invariant 9): reach every control and dialog without the mouse, then check touch.
   * Notify the developer with the direct verification URL (**`http://dev-server:6080/vnc.html`**) so they can interactively test the UI from their Mac browser without flashing.
5. **Update PLAN.md** milestone checkboxes as features are completed and verified.
