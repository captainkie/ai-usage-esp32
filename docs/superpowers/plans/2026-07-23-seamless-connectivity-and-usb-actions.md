# Seamless Connectivity + USB Actions — Implementation Plan

> Build-then-verify-with-user. The firmware pieces compile now but their on-device
> behaviour (SD read, Wi-Fi join, mDNS, USB actions) must be flashed + verified
> **together with the user** — do NOT flash over the working display unattended.

**Goal:** Carry Pixie between home ↔ office with **no re-typing**, and make the
**Remote work over USB** (so it works at the office where the board can't join Wi-Fi).

**Motivation (from the 2026-07-23 office session):** the board would not join the
office Wi-Fi (`[net] fetch failed: wifi`), and Remote actions only go over Wi-Fi, so
the Remote is dead at the office. USB via a hub + data cable is the reliable path
there — so actions must also work over USB. And switching networks means re-entering
Wi-Fi + Mac IP + token every time.

## Pieces

### 1. Actions over USB (bidirectional) — Remote works with no Wi-Fi
- **Firmware (`net.h`):** `net_action(body)` — if `WiFi.status()!=WL_CONNECTED` (or a
  USB frame is fresh), send the action over **serial** instead of HTTP: write one
  line `@ACT <token> <body>\n` to `Serial`. Return true (fire-and-forget; the bridge
  executes locally). Keep the Wi-Fi POST path when Wi-Fi IS connected.
- **Bridge (`ai-usage-bridge.mjs`):** the USB link becomes read+write. Open the port
  read-write; keep writing `/usage` frames; **also read incoming lines**. A line
  starting `@ACT <token> <json>` → check `tokensMatch(token, PAIR_TOKEN)` →
  `validateAction(json, cfg)` → `execFile` (same path as `POST /action`). Ignore
  other lines. Token still gates it.
- **Verifiable on Mac now:** the bridge read-side — feed a fake `@ACT <token> {...}`
  line into the port and confirm it executes (opens Safari, etc.).

### 2. WiFiMulti — save multiple networks, auto-join whichever is in range
- **Firmware:** store a list of `{ssid,pass}` (home, office, hotspot) in NVS (JSON in
  a Preferences key) + the SD config (piece 3). On boot, feed them all to
  `WiFiMulti` and `run()` — it joins the strongest reachable one. The captive portal
  **appends** to the list instead of replacing, so configuring a new place keeps the
  old ones.
- Falls back to the setup portal only if none join AND there's no USB.

### 3. TF-card config (`pixie.json`) — portable, survives reflash, giveaway-friendly
- **Firmware (`audio.h`→ reuse, or new `storage.h`):** mount the SD card (BSP
  `04_SD_Card` driver — `sdcard_bsp.*`, note the GPIO_NUM_8 power-enable). Read
  `/pixie.json`:
  ```json
  { "wifi": [{"ssid":"home","pass":"..."}, {"ssid":"office","pass":"..."}],
    "bridge_port": 8787, "token": "…", "bridge_host": "" }
  ```
  Merge into the WiFiMulti list + token + port. `bridge_host` optional (mDNS fills it
  when empty). On successful portal config, **write** the merged config back to the
  card. If no card / no file, behave exactly as today (NVS only).

### 4. mDNS auto-discover the Mac — stop typing the IP
- **Bridge:** already advertises `_aiusage._tcp` via `dns-sd` (`lib/mdns.mjs`). Add
  the pairing already exists; no change needed except confirming it runs.
- **Firmware (`net.h`):** if `bridge_host` is empty, use `ESPmDNS` to resolve the
  service `_aiusage._tcp` → get the Mac's IP + port. Cache it; re-resolve on fetch
  failure. Typed IP still overrides (manual escape hatch).

### 5. Small: screen ① brand long-press → reopen setup portal
- **Firmware (`.ino`):** add `LV_OBJ_FLAG_CLICKABLE` + the `brand_cb`
  `LV_EVENT_LONG_PRESSED` handler to screen ①'s `lblBrand` (currently only
  screens ②③ have it). One-liner parity with `build_topbar`.

## Build order
1. **Bridge: USB actions read-side** (testable on Mac) → commit.
2. **Firmware: USB action send** in `net_action` → compile → commit.
3. **Firmware: SD `pixie.json` read** (BSP SD driver) → compile → commit.
4. **Firmware: WiFiMulti** (NVS + SD list, portal appends) → compile → commit.
5. **Firmware: mDNS resolve** when host empty → compile → commit.
6. **Firmware: screen ① brand long-press** → compile → commit.
7. **Flash once + verify together** (checklist below).

## On-device verification checklist (WITH the user)
- [ ] Remote button over USB (no Wi-Fi) → Mac reacts (YouTube/volume).
- [ ] SD `pixie.json` read on boot (serial log shows parsed networks).
- [ ] Save home Wi-Fi + office/hotspot → both remembered → auto-joins whichever.
- [ ] mDNS: leave Mac IP blank → device finds the bridge by name.
- [ ] Long-press screen ① brand → portal opens.
- [ ] Regression: dashboard still fine over USB + Wi-Fi; USB frames still drive the display.

## Notes / risks
- **Bidirectional serial in zero-dep Node** is the trickiest bridge bit — open the
  cu.* device read-write, line-buffer the read side. Verify no conflict with the
  frame writer (single fd, one read loop + writes).
- **Memory:** SD + WiFiMulti + mDNS + LVGL + Wi-Fi + (later audio) — watch PSRAM.
- Office Wi-Fi still won't join (enterprise/5GHz) — seamless helps **home + hotspot**;
  USB + USB-actions is the office answer.
