# Coder's Toolkit — Implementation Plan

Offline developer helpers on the Tab5: Base64 / Hex / hashes / JWT / UUID /
Unix time. A keyboard-first "Swiss-army knife" for the moments a cyberdeck
user needs to decode a token or hash a string without leaving the device.

The scaffold in this directory already builds and runs (simulator verified);
the plan below is what remains to make it production-polished and to keep it
inside the devOS rules (AGENTS.md).

---

## Status

| Done | What |
| :--- | :--- |
| ✅ | `devos_crypto`: base64 / base64url / hex / url en+decode, **base58**, **crc32**, and a **streaming hash API** (`devos_hash_begin/update/end`). All checked against RFC / known vectors in `tools/crypto_test.c`. |
| ✅ | `coder_core.{h,c}`: pure logic (transform / hash / crc / JWT / UUID / epoch / encoded-detect), no LVGL. |
| ✅ | `app_coder.c`: descriptor, lifecycle, **11 tools**, `devos_widgets` UI, `devos_focus` keyboard handling, letter shortcuts, clipboard copy, auto-direction, `get_shortcuts()`, telemetry. |
| ✅ | Output runs through `devos_codeview` (scrolls, monospace, JSON colouring); the result buffer and codeview live in PSRAM (input cap raised to 8 KB). |
| ✅ | `devos_hashfile`: a Core 0 engine streaming a file off the SD card in 16 KB PSRAM chunks, with progress / result / error getters (AGENTS.md #1). The app polls it ~4 Hz and shows "Hashing N%". |
| ✅ | Vector icon `devos_icon_coder` (angle brackets + slash) in `devos_icons.{h,c}`. |
| ✅ | Wired: `main.c`, `main/CMakeLists.txt`, root `CMakeLists.txt`, new `components/devos_hashfile/`. |
| ✅ | Both targets build warning-free: simulator (`ninja -C build_sim`) and firmware (`idf.py build`). |
| ✅ | Host tests pass: `tools/crypto_test.c` and `tools/coder_test.c`. |
| ✅ | Simulator keyboard-first walkthrough: every tool, tool switching, `Sym+T` theming, and a real file-hash matching `sha256sum`. |

## Design decisions

- **Nothing forked (AGENTS.md #7).** `coder_core` calls `devos_crypto` for SHA/HMAC/base64/base58/hex/url/crc
  and `devos_json` for JWT pretty-printing; all live in the shared engines so any app can use them.
- **Testable core.** All logic is in `coder_core.c` (no LVGL or `devos_config`), so `tools/coder_test.c`
  builds it on the host - the same pattern as `tools/crypto_test.c`.
- **Slow work goes to Core 0.** Text hashing is a few KB and runs inline; hashing a *file* streams through
  the `devos_hashfile` worker so the UI task never blocks on a multi-MB SD read (AGENTS.md #1).
- **The codeview keeps text by pointer.** Results are always written into the long-lived `s_ctx.out`
  (PSRAM), never a stack buffer, or the view would read freed memory.
- **Binary-safe output.** A decode that yields non-printable bytes shows `0x..` hex pairs.
- **Auto-direction.** Encoded-looking input defaults the dropdown to Decode until overridden.

## Fixed along the way

- **Simulator keyboard bug:** SDL reports the *unshifted* keysym (Shift+`-` arrived as `-`), so `_` and the
  other shifted punctuation could not be typed in the simulator at all - a problem for any path or token
  containing `_`. `main.c` now maps Shift+key to its US-layout symbol and drops the Shift modifier before
  dispatch.

## Remaining work

### Acceptance on hardware
- The desktop simulator is verified; flash a Tab5 and repeat the file-hash tool against a real SD card
  (SD throughput is the only thing the simulator can't show).

### Possible next tools
- An ASCII/byte table, "hash a file" in the other direction (verify a given digest), and HMAC-over-file.
