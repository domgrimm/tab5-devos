# MicroLink in devOS

Vendored from https://github.com/CamM2325/microlink (MIT, see LICENSE),
commit 216da3300f0493b0860247d43f7af5ce29df63a5 (2026-03-17), directory
`components/microlink`. WireGuard (`../wireguard_lwip`, BSD-3) comes from the
same repository.

devOS talks to it only through `components/devos_tailnet`.

## Local patches

1. `include/microlink_internal.h`: `ML_TASK_COORD_CORE` and
   `ML_TASK_WG_MGR_CORE` are 0 instead of 1. Core 1 runs the LVGL UI on the
   Tab5; everything network/crypto lives on core 0.
2. `include/microlink_internal.h` + `src/ml_coord.c` (`do_register`): new
   `reg_error` / `auth_url` fields filled from `RegisterResponse.Error` /
   `RegisterResponse.AuthURL`, and registration fails when either is set.
   Upstream ignored both and retried forever, so a bad auth key looked like
   "connecting..." for ever. devos_tailnet stops the client and shows the
   message.

3. `CMakeLists.txt`: `-Wno-stringop-truncation`. GCC 14 (ESP-IDF 5.4) turns
   upstream's bounded strncpy() copies into errors; GCC 13 did not.

4. Nearest DERP region (`src/ml_stun.c` `ml_stun_pick_derp_region`,
   `src/ml_coord.c`, `src/microlink.c`, `derp_measured_region` in
   `include/microlink_internal.h`). Upstream always asked to be homed on
   region 9 (Dallas). A peer can only be relayed through its own home
   region, so from Australia every peer homed on Sydney answered
   "DERP PeerGone" and was unreachable unless a direct UDP path happened to
   work. Now, once per boot after the DERPMap arrives, one STUN binding
   request goes to every region from a private socket; the fastest region
   becomes the home region and the PreferredDERP sent to control. It runs
   from the top of the coord task loop (after the peer fetch, before DERP
   connects) and is not stored in NVS: an earlier version wrote it to flash
   from that task and the device hard-reset once.

5. `src/ml_peer_nvs.c`: `ml_peer_nvs_save()` no longer writes the whole
   peer table to flash (and commits) for every peer added: that was 24
   flash writes in a burst from the WG task at every boot. It updates the
   RAM table; `ml_peer_nvs_flush_if_idle()` writes it once it has been
   quiet for 20 s, called by devos_tailnet from the UI task. Two hard
   watchdog resets happened during flash writes from core 0 while the UI
   was busy; UI-core flash writes (settings) never did.

Everything else is upstream as-is (cellular, network switching and the HTTP
config server stay disabled in Kconfig).

## wireguard_lwip patches (for the WireGuard app, `components/devos_wireguard`)

W1. `src/wireguardif.c/.h`: `wireguardif_enable_socket_bind()`. MicroLink sets
    the file-global magicsock flag and upstream never cleared it, so a plain
    tunnel created after Tailscale had run once came up with no UDP socket
    and no timer.
W2. `src/wireguardif.c/.h`: `wireguardif_add_allowed_ip()` (wraps the static
    `peer_add_ip`) and `WIREGUARD_MAX_SRC_IPS` 2 -> 4, so a peer can have
    several AllowedIPs.
W3. `src/wireguard-platform-esp32.c`: handshake TAI64N timestamps use the
    wall clock once it is set (after 2023), else uptime as before. Peers
    drop initiations that aren't newer than the last one they saw, so
    uptime-based stamps failed after every reboot.
