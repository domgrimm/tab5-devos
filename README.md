# devOS for the M5Stack Tab5

devOS is a keyboard-first firmware for the **M5Stack Tab5** (ESP32-P4, 5" 1280×720 display) with its **70-key keyboard**. It turns the Tab5 into a small cyberdeck for sysadmin and network work: SSH terminal, Markdown editor, Tailscale and WireGuard VPNs, MQTT, REST, Docker, network diagnostics, an ADS-B radar and a TOTP authenticator. Each of these is an app that plugs into the core the same way, so you can add your own without touching the Home Screen.

**Install it from your browser:** <https://domgrimm.github.io/tab5-devos/> (Chrome or Edge, USB-C cable). After that the Tab5 updates itself over Wi-Fi.

---

## Contents

1. [What's on it](#whats-on-it)
2. [How it works](#how-it-works)
3. [Drop-in apps](#drop-in-apps)
4. [Building](#building)
5. [Releasing](#releasing)
6. [Repository layout](#repository-layout)

---

## What's on it

| App | What it does |
| :--- | :--- |
| **Terminal** | Multi-session SSH client (libssh2 over mbedTLS), VT100/xterm emulation, 10,000-line scrollback, bookmarks from the SD card |
| **Editor** | SD-card file browser, Markdown / text editor with live preview, a scratchpad and voice memos |
| **Tailscale** | Joins your tailnet (MicroLink: ts2021, DERP, DISCO) and lists peers with latency |
| **WireGuard** | Brings up tunnels from standard `wg-quick` config files |
| **MQTT** | Broker monitor and publisher (MQTT 3.1.1) with JSON pretty-printing |
| **Network** | Ping, DNS lookup, port scan, Wi-Fi survey and mDNS browser |
| **REST** | REST and webhook client with saved requests and `{{variables}}` |
| **Docker** | Docker Engine / Portainer console: containers, logs, start / stop |
| **ADS-B** | Radar view of aircraft from a dump1090 / readsb / tar1090 `aircraft.json` feed, over an OpenStreetMap underlay cached on the SD card |
| **Authenticator** | Offline TOTP codes from an encrypted vault; add accounts by scanning a QR code with the camera |
| **Settings** | Wi-Fi, display, power, time zone, updates, and switching apps on and off |

Global keys, from any app: **Sym + H** Home Screen, **Sym + T** dark / light theme, **Sym + − / +** brightness, **Sym + 1…6** built-in apps, **Alt + Tab** previous app, **Esc** back out (and to the Home Screen when nothing else wants it). The Tab5 keyboard has no Fn key; **Sym** is the system modifier.

On first boot with a MicroSD card inserted, devOS creates the folders and starter files it needs (`/notes/`, `/.ssh/`, `/wireguard/`, `/.devos/`, a welcome note that lists the keys). You never have to prepare the card on a computer.

---

## How it works

### The two cores

The ESP32-P4 has two RISC-V cores, and devOS gives each a job:

```
 Core 0 — system, network, crypto            Core 1 — presentation
 ┌────────────────────────────────────┐      ┌─────────────────────────────────┐
 │ Wi-Fi (ESP-Hosted → ESP32-C6)       │      │ LVGL v9 GUI loop + PPA blits    │
 │ lwIP, DNS, mDNS                     │ ◄──► │ touch (GT911)                   │
 │ Tailscale / WireGuard tunnels       │queues│ keyboard polling → key dispatch │
 │ TLS, SSH I/O, HTTP, MQTT            │events│ app screens                     │
 │ power management, telemetry         │      │ MicroSD I/O                     │
 └────────────────────────────────────┘      └─────────────────────────────────┘
```

The UI core never waits on the network. An app starts work on an **engine** (a component with no LVGL in it, running on core 0) and reads results back through queues, task notifications or small status getters. A slow TLS handshake can't freeze the screen.

Memory follows the same split. The 32 MB PSRAM holds big things: LVGL draw buffers, terminal scrollback, file and diff buffers, parsed Markdown. The ~768 KB of internal SRAM is kept for what needs it: TLS / SSH handshakes, Wi-Fi DMA and task stacks. At least 120 KB of it stays free.

### Boot

`main/main.c` brings the system up in order:

1. **Board** (`bsp_tab5`): I2C buses, MIPI-DSI display, touch, battery monitor, RTC, camera.
2. **Storage** (`devos_storage`): mount the MicroSD card and create any missing folders and templates.
3. **App switches:** read which apps are switched off (Settings > Apps) *before* anything else starts.
4. **Keyboard, theme, core, power, OTA.**
5. **Network** (`devos_net`), then each app's **engine**, started only if its app is switched on.
6. **Apps:** each app's descriptor is handed to `devos_core_register_app()`. The Home Screen registers last and builds its tiles from whatever is registered.

### The core (`devos_core`)

`devos_core` is the small kernel every app talks to:

- **App registry:** up to 32 apps, each identified by a stable string **uid** (`"terminal"`, `"adsb"`). Registration calls the app's `init()` once and measures the memory it took.
- **App switcher:** shows one app's screen at a time and calls `hide()` on the old app, then `show()` on the new one.
- **Key dispatcher:** every key goes first to the global shortcuts, then to the active app's `handle_key()`. An unhandled `Esc` goes to the Home Screen.
- **Intents:** one app can ask another to do something. `devos_core_open_with("terminal", "ssh", "pi@host")` switches to the Terminal, whose `show()` picks the request up with `devos_core_take_intent()`. The Network app uses this to SSH or ping a host it found, for example.
- **Telemetry:** a 1 Hz snapshot (battery, Wi-Fi, IP, VPN state, SD, heap, CPU, clock) fed by `devos_sysmon` and drawn by the top bar and tiles.
- **App switches:** an app switched off in Settings is never initialised and its engine never starts, so it costs no RAM. Changes apply on restart. If a new set of switches fails to boot twice, devOS reverts to the last set that worked. Holding a finger on the screen at power-on switches every app back on.

### Shared building blocks

Apps don't carry their own renderers, parsers or network code. Each of these exists once, so a fix lands everywhere:

| Component | Provides |
| :--- | :--- |
| `devos_ui` | Theme engine (dark / high-contrast light, live switching), top bar, `devos_widgets` (buttons, fields, dialogs, virtual lists), `devos_focus` (keyboard focus and focus ring), `devos_icons` (vector icons) |
| `devos_net` | Wi-Fi manager, DNS + mDNS resolver, and the socket layer every connection goes through. That layer is where VPN routing applies, so a socket opened any other way would bypass the tunnel |
| `devos_http` | HTTP/1.1 + HTTPS client on top of the socket layer |
| `devos_json` | Small JSON reader and pretty-printer |
| `devos_mdview` | The CommonMark-subset renderer used by the editor preview |
| `devos_crypto` | SHA-1/256/512, HMAC, PBKDF2, ChaCha20-Poly1305, base32 |
| `devos_vterm` | VT100 / xterm terminal emulator |

The feature engines (`devos_mqtt`, `devos_docker`, `devos_adsb`, `devos_maptiles`, `devos_netdiag`, `devos_totp`, `devos_wireguard`, `devos_tailnet`, `devos_audio`, `devos_qr`) follow the same rule: no LVGL, a small C API, and status getters that report "off" if their app is switched off.

### Keyboard first

Every screen works from the keyboard alone; touch is a second way in, never the only one. The same keys mean the same thing everywhere:

| Keys | Meaning |
| :--- | :--- |
| Arrows | Move the selection or focus |
| `Tab` / `Aa + Tab` | Next / previous field or region |
| `Enter` | Activate: open, connect, confirm, press the focused button |
| `Space` | Toggle a checkbox or switch; pause live views |
| `Left` / `Right` | Change the focused value (slider, dropdown, switch) |
| `Esc` | Back out one level: dialog, then field or panel, then Home Screen |
| Letters | Frequent actions, shown in each screen's key hint line |
| `Sym + <key>` | System shortcuts; `Sym + L` shows or hides an app's side panel |

The focused control always has a visible accent ring, a selection border or a text cursor. On-screen keyboards only appear when no hardware keyboard is attached.

### Updates

`devos_ota` reads a manifest (`version`, `url`, `size`, `sha256`) from the feed, which by default is this repo's GitHub Pages site. If the version is newer than the running one, it downloads the image into the spare OTA slot over HTTPS (certificates verified), checks the hash and restarts into it. Rollback is on: an image that doesn't boot properly is replaced by the previous one. You can point the feed at your own server in Settings > System.

---

## Drop-in apps

An app is a **descriptor**: a struct of names and callbacks that it hands to the core. The core, the Home Screen, Settings > Apps and the top bar only ever see descriptors. They have no list of apps and no switch statement over app ids, so a new app appears everywhere without them changing.

### The descriptor

```c
typedef struct {
    devos_app_id_t id;          /* leave 0: the core assigns one */
    const char *uid;            /* stable id, e.g. "weather" - used for layout, switches, intents */
    const char *name;           /* short tile name */
    const char *title;          /* screen / tile title */
    const char *subtitle;       /* tile subtitle when there are no live lines */
    const char *icon;           /* LVGL symbol, e.g. LV_SYMBOL_GPS (fallback icon) */
    const char *category;       /* "tools", "network", "system", ... */
    lv_obj_t *screen;           /* the app's root object, set in init() */
    void (*init)(void);         /* once, at boot (skipped if switched off) */
    void (*show)(void);         /* the app comes to the front */
    void (*hide)(void);         /* the app goes to the back */
    bool (*handle_key)(uint32_t key, uint8_t modifiers);   /* true = handled */
    int  (*get_telemetry_lines)(char lines[3][64]);        /* live tile text */
    void (*draw_icon)(lv_layer_t *layer, const lv_area_t *area, lv_color_t color);
} devos_app_descriptor_t;
```

(The full definition is in `components/devos_core/devos_core.h`.)

### What the core does with it

| Callback | Called when | The app should |
| :--- | :--- | :--- |
| `init` | Once at boot, from `devos_core_register_app()` | Build its screen hidden, set `screen`, register its controls with `devos_theme` / `devos_focus` |
| `show` | The user opens the app (tile, hotkey, intent) | Refresh, take keyboard focus, collect any intent with `devos_core_take_intent()` |
| `hide` | Another app comes to the front | Pause timers and live views |
| `handle_key` | Every key, after the global shortcuts | Return `true` if it used the key. Leave `Esc` unhandled at the top level and the core goes Home |
| `get_telemetry_lines` | About once a second while the Home Screen is up | Write up to 3 short lines for its tile ("3 aircraft", "Tunnel up") |
| `draw_icon` | Whenever an icon is drawn | Draw a vector icon on a 20×20 grid (see `devos_icons.c`); `NULL` falls back to `icon` |

Once registered, an app automatically gets:

- **A Home Screen tile** with its icon, title and live lines. Tiles go 8 per page; users rearrange or hide them (**E** on the Home Screen) and the layout is saved by uid.
- **A row in Settings > Apps** with an on/off switch and the RAM it costs, measured when it started.
- **Theming.** Widgets built with `devos_widgets` recolour when the theme changes.
- **Keyboard focus** through `devos_focus`: rings, Tab order, Enter / Space / arrows on buttons, switches, sliders and fields.
- **Intents.** Other apps can open it with `devos_core_open_with("<uid>", action, arg)`. `open_with()` returns `false` if the app isn't there (e.g. switched off), so callers handle that rather than assuming an app exists.

### Writing one

[`main/apps/app_template/`](main/apps/app_template/) is a complete, working example that follows every rule below; copy it. Its [README](main/apps/app_template/README.md) goes step by step. In short:

**1. Create `main/apps/app_weather/app_weather.c`** (and a header declaring `app_weather_get_descriptor()`):

```c
#include "devos_core.h"
#include "devos_widgets.h"
#include "devos_focus.h"

static devos_app_descriptor_t s_desc;
static devos_focus_t s_focus;
static lv_obj_t *s_temp;

static void refresh_cb(lv_event_t *e) { /* ask the engine for new data */ }

static void weather_init(void)
{
    lv_obj_t *scr = devos_w_screen(&s_desc);           /* themed, hidden, sets s_desc.screen */
    devos_w_bar(scr, "Weather", NULL);
    s_temp = devos_w_label(scr, &lv_font_montserrat_28, DEVOS_W_TEXT, "--");
    lv_obj_t *btn = devos_w_btn(scr, "Refresh", 160, refresh_cb, NULL, NULL);
    devos_w_keys(scr);                                  /* key hint footer */

    devos_focus_init(&s_focus);
    devos_focus_add(&s_focus, btn);
}

static void weather_show(void) { devos_focus_first(&s_focus); }

static bool weather_key(uint32_t key, uint8_t mods)
{
    if (devos_focus_key(&s_focus, key, mods)) return true;   /* arrows, Enter, Tab ... */
    if (!mods && (key == 'r' || key == 'R')) { refresh_cb(NULL); return true; }
    return false;                                             /* Esc -> Home Screen */
}

static int weather_tile(char lines[3][64])
{
    snprintf(lines[0], 64, "21 C, clear");
    return 1;
}

devos_app_descriptor_t *app_weather_get_descriptor(void)
{
    s_desc.uid = "weather";
    s_desc.name = "Weather";
    s_desc.title = "Weather";
    s_desc.subtitle = "Local forecast";
    s_desc.icon = LV_SYMBOL_GPS;
    s_desc.category = "tools";
    s_desc.init = weather_init;
    s_desc.show = weather_show;
    s_desc.handle_key = weather_key;
    s_desc.get_telemetry_lines = weather_tile;
    return &s_desc;
}
```

(Check `devos_widgets.h` for the exact helper signatures; the template uses all of them.)

**2. Register it** in `devos_system_bringup()` in `main/main.c`, before the launcher:

```c
devos_core_register_app(app_weather_get_descriptor());
```

If it has a background engine, start that with `START_ENGINE("weather", weather_engine_init());` so it's skipped when the app is switched off.

**3. Add the source** to `main/CMakeLists.txt` (firmware: `APP_SRCS` and `INCLUDE_DIRS`) and to the root `CMakeLists.txt` (simulator: `DEVOS_SOURCES` and the include list).

That's all. You don't edit the launcher, Settings, the top bar or any enum.

### Rules for apps

- **Never block the UI core.** Network, TLS, crypto and slow file work go in an engine on core 0; the app polls its status or gets a queue message.
- **Use the shared pieces.** Sockets through `devos_net_socket_*` (so VPN routing works), HTTP through `devos_http`, JSON through `devos_json`, Markdown through `devos_mdview`. Extend them rather than copying them into your app.
- **Big buffers go in PSRAM** (`heap_caps_malloc(n, MALLOC_CAP_SPIRAM)`); keep internal SRAM for the network stack.
- **Keyboard first:** every control reachable by key, focus always visible, keys shown on screen, `Esc` backs out one level.
- **Theme everything:** build with `devos_widgets` or register custom widgets with `devos_theme` so both palettes work and switch instantly.
- **Clean up:** timers, callbacks and allocations belong to the app's context struct, not loose globals.
- **Refer to other apps by uid,** never by id, and handle `devos_core_open_with()` returning `false`.
- **Secrets** (keys, tokens, passwords) go in NVS or encrypted SD storage, never in source.

`AGENTS.md` has the full rulebook.

---

## Building

### Prerequisites

- ESP-IDF **v5.4** (target `esp32p4`). Espressif's container image works as-is: `docker.io/espressif/idf:v5.4`.
- LVGL isn't vendored. Fetch the pinned version once:
  ```bash
  tools/fetch_lvgl.sh
  ```

### Firmware

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Or with the container, without installing IDF:

```bash
podman run --rm -v "$PWD":/project:Z -w /project docker.io/espressif/idf:v5.4 idf.py build
```

`sdkconfig` is generated from `sdkconfig.defaults` and isn't committed. After changing the defaults, delete `sdkconfig` and run `idf.py fullclean`.

The Tab5's ESP32-C6 runs the ESP-Hosted Wi-Fi firmware. It ships on the Tab5; `tools/flash_c6_slave.sh` reflashes it if needed.

### Simulator

The whole UI also runs on a Linux or macOS desktop (SDL2), with simulated networking and telemetry. It's the quickest way to work on an app:

```bash
cmake -B build_sim -S . -DDEVOS_SIMULATOR=ON -G Ninja
ninja -C build_sim
./build_sim/devos_sim
```

Your keyboard stands in for the Tab5's. `tools/sim/run_web_sim.sh` runs it headless behind noVNC so you can use it from a browser on another machine. The simulator also shows some placeholder demo tiles to exercise Home Screen paging; they're never built into the firmware.

### Tests

Host-side unit tests live in `tools/*_test.c` (Markdown renderer, launcher, app switches, OTA, terminal emulator, crypto). Each file's header has its exact `gcc` line. Run them from an empty directory: some write config files relative to the current directory.

---

## Releasing

The GitHub Pages site in `docs/` is the browser installer and the OTA feed. To release:

1. Bump `DEVOS_VERSION_MAJOR/MINOR/PATCH` in `components/devos_config/include/devos_config.h`. Devices only take newer versions.
2. Build the firmware.
3. Regenerate the site:
   ```bash
   tools/publish_pages.py --build build --out docs --notes "What changed"
   ```
   This writes the flasher page, the ESP Web Tools manifest, the four flash images and the OTA manifest. It refuses to write anything if the images contain home-directory paths, e-mail addresses, or any string listed in `~/.config/devos/publish-deny.txt` (one per line; your own names, networks and addresses).
4. Commit and push. Pages serves `docs/` at `https://<owner>.github.io/<repo>/`.

If you fork this, change `DEVOS_OTA_DEFAULT_FEED` in `components/devos_ota/devos_ota.c` to your own Pages URL.

---

## Repository layout

```
components/
  bsp_tab5/          board drivers: display, touch, battery monitor, RTC, camera, I2C
  tab5_keyboard/     70-key keyboard driver (I2C, Sym / Aa / Ctrl / Alt)
  devos_config/      pins, sizes, version
  devos_core/        app registry, switcher, key dispatch, intents, app switches, telemetry
  devos_ui/          theme, top bar, widgets, focus, icons
  devos_net/         Wi-Fi, DNS/mDNS, socket layer + VPN routing
  devos_http/        HTTP(S) client
  devos_storage/     MicroSD mount and scaffolding
  devos_power/       active / dim / sleep
  devos_ota/         update check and install
  devos_sysmon/      1 Hz telemetry, clock, time zones
  devos_json/  devos_mdview/  devos_crypto/  devos_vterm/          shared engines
  devos_mqtt/  devos_docker/  devos_adsb/  devos_netdiag/          feature engines
  devos_maptiles/    OpenStreetMap tiles: fetch one at a time, cache on SD
  devos_totp/  devos_wireguard/  devos_tailnet/  devos_audio/  devos_qr/
  libssh2_port/      SSH client glue (libssh2 from the component registry)
  microlink/         Tailscale client (third party, MIT)
  wireguard_lwip/    WireGuard for lwIP (third party, BSD)
  quirc/             QR decoder (third party, ISC)
main/
  main.c             bring-up and app registration
  apps/app_*/        one folder per app; app_template/ is the starting point
tools/
  *_test.c           host unit tests
  sim/               web simulator scripts
  fetch_lvgl.sh      fetch the pinned LVGL
  publish_pages.py   build the Pages site (installer + OTA feed)
  flash_c6_slave.sh  reflash the ESP32-C6 Wi-Fi firmware
docs/                GitHub Pages: browser installer and OTA feed (generated)
```

`PLAN.md` is the design document and roadmap.

## Licence

devOS is released under the [MIT licence](LICENSE). Third-party components keep their own licences: `components/microlink` (MIT), `components/wireguard_lwip` (BSD-3-Clause), `components/quirc` (ISC), and LVGL (MIT, fetched by `tools/fetch_lvgl.sh`).
