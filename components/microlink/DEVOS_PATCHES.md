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
   becomes the home region and the PreferredDERP sent to control (and is
   kept in NVS `microlink/derp_pref` so the next boot registers with it).

Everything else is upstream as-is (cellular, network switching and the HTTP
config server stay disabled in Kconfig).
