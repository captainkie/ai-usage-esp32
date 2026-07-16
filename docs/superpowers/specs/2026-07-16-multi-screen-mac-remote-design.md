# Multi-Screen Dashboard + Mac Remote — Design Spec

- **Date:** 2026-07-16
- **Status:** Approved (design) — ready for implementation planning
- **Related:** `design/mockup.html` (interactive preview, shipped), `bridge/ai-usage-bridge.mjs`, `firmware/ai-usage-esp32/`
- **Live preview:** <https://captainkie.github.io/ai-usage-esp32/design/mockup.html>

> **สรุปสั้น (TH):** จอ ESP32 เดิม (หน้า AI Usage) จะกลายเป็น **3 หน้าปัดสลับ** — ①
> AI Usage (เดิม) · ② Mac Monitor (CPU/RAM/Disk/Net/แบต/temp/top process) · ③ Mac Remote
> (ทางลัดเปิดเว็บ-แอป, คุมสื่อ, ปรับเสียง, ล็อก/พักจอ). ฝั่ง Mac (bridge) เพิ่มการอ่านค่าระบบ +
> endpoint สั่งงานแบบ allowlist ที่มี **pairing token** กัน. รองรับการเชื่อมต่อ **USB / Wi-Fi /
> BLE** (ทำเป็นเฟส) และมองไปถึงการทำขายแบบ "เสียบ+สแกน QR+ใช้ได้เลย".

---

## 1. Goals

1. Turn the single-screen device into a **3-screen, swipe-navigated** dashboard without
   regressing the existing AI Usage screen.
2. Add a **Mac system monitor** screen (CPU, RAM, Disk, plus Net / battery / temperature /
   top process) in the same visual language as the twin gauges.
3. Add a **Mac remote** screen: shortcuts (open web/app), media transport, volume, and
   lock / display-sleep — the device controls the Mac through the bridge.
4. Keep the project's **privacy/security posture**: nothing sensitive leaves the Mac; the
   remote's command channel is authenticated and allowlisted.
5. Support **three connection transports** (USB, Wi-Fi, BLE), delivered in phases.
6. Lay groundwork for a **"plug-and-play" commercial unit** (buy → plug in → scan QR →
   minimal setup → works).

## 2. Non-goals (YAGNI)

- **Auto-typing the macOS login password.** Rejected on security grounds (see §9). The
  device never stores or transmits the login password.
- **IMU/tilt navigation.** Swipe + buttons are enough; the QMI8658 stays unused for now.
- **Playing video/audio on the device.** The 640×172 panel is a controller/《remote》, not a
  media player. "Open YouTube" means the *Mac* opens it.
- **Bluetooth Classic.** The ESP32-S3 only supports **BLE** (Bluetooth 5 LE); Classic is not
  an option and is not needed.

## 3. Current architecture (recap)

```
Mac bridge (Node, zero-dep)  --LAN HTTP-->  ESP32-S3 (Arduino + LVGL v8)
  reads Claude token (Keychain)               polls GET /usage every 15 s
  GET api.anthropic.com/oauth/usage           renders twin arc gauges + mascot
  serves /usage JSON (percent, reset, model)  touch: pills / mascot
```

- Firmware builds the whole UI on `lv_scr_act()` in `ui_build()`; a single LVGL timer
  (`render_cb`) updates gauges/mascot; `loop()` polls the bridge.
- `config.h` holds tunables + the `UsageState`/`ProviderState`/`Window` data model.
- `net.h` does Wi-Fi provisioning (WiFiManager captive portal) + `net_fetch()` JSON parse.
- Bridge caches `/usage` for 20 s and is **read-only** today.

## 4. UX — three screens + navigation

Screens (index → name):

| # | Screen | Content |
|---|--------|---------|
| 0 | **AI Usage** | Unchanged: twin gauges, model/effort, mascot, provider pills |
| 1 | **Mac Monitor** | CPU / RAM / Disk arcs + Net/battery/temp/top-process ticker |
| 2 | **Mac Remote** | Shortcuts, media transport, volume, lock / display-sleep |

**Navigation**

- **Swipe left/right** between screens using **LVGL `lv_tileview`** (v8 native horizontal
  paging with snap + gesture handling). Screens are tiles `(0,0) (1,0) (2,0)`.
- **Page dots** indicator, shown transiently on change (fades out) so it never clutters a
  screen. Matches the mockup.
- Per-screen tap interactions continue to work (tileview distinguishes horizontal scroll
  from child clicks). Existing pill/mascot taps on screen 0 are preserved.
- Firmware refactor: split the monolithic `ui_build()` into a small **screen manager** with
  `screen_usage_build(parent)`, `screen_monitor_build(parent)`, `screen_remote_build(parent)`,
  each building into its own tile; each screen exposes an `update(state)` called by the
  render timer only for the active (and adjacent) tile.

## 5. Screen 1 — Mac Monitor

- **Three 270° arc gauges**: CPU, RAM, Disk — same geometry/traffic-light colours as the
  usage gauges (`util_color()` thresholds reused). Big % in the centre, label above, a small
  caption below (e.g. `10 cores`, `10.2 / 16 GB`, `379 / 512 GB`).
- **Bottom ticker** (compact mono chips): `⬇ down MB/s`, `⬆ up MB/s`, `🔋 battery %`,
  `🌡 temp °C`, `TOP <process> <pct>%`.
- Values ease toward targets (same 0.28 easing as the gauges) for smooth motion.
- **Temperature caveat:** on Apple Silicon, CPU/SoC temperature typically needs
  `sudo powermetrics` (or SMC access), which we will **not** require. Temperature is
  best-effort: read only if available without elevated privileges; otherwise the chip is
  hidden. Documented, not a blocker.

## 6. Screen 2 — Mac Remote

Layout (see mockup): a row of **shortcut tiles**, then a control row with three groups.

- **Shortcuts** (user-configurable, from `bridge/actions.json`): e.g. `YouTube`, `Music`,
  `Safari`, `Focus`. Tapping sends an `open_url` or `open_app` (or `shortcut`) action.
- **Media transport:** ⏮ / ⏯ / ⏭ → media-key actions.
- **Volume:** − / mute / + with a level bar.
- **System:** 🔒 lock · 💤 display-sleep.
- The device shows a brief **confirmation toast** on each press (mirrors the command the
  bridge will run). No destructive action (lock/sleep) fires without a valid pairing token.

## 7. Bridge changes (Mac side, still zero-dependency)

### 7.1 Extend `GET /usage` with a `system` block

```jsonc
{
  "ok": true,
  "updated": "…",
  "providers": { /* unchanged */ },
  "system": {
    "host": "Naren-MBP",
    "cpu":  { "util": 34, "cores": 10 },
    "mem":  { "util": 61, "used_gb": 10.2, "total_gb": 16 },
    "disk": { "util": 74, "used_gb": 379, "total_gb": 512, "mount": "/" },
    "net":  { "down_kbps": 1200, "up_kbps": 340 },
    "battery": { "percent": 82, "charging": false },   // null on desktops
    "temp_c": 54,                                        // null if unavailable
    "top": [ { "name": "Chrome", "cpu": 42 }, … ]        // top 1–3 by CPU
  }
}
```

Data sources (stdlib + shelling out, no new deps):
- CPU: `top -l 2 -n 0` (2nd sample) or `ps -A -o %cpu`; cores from `sysctl -n hw.ncpu`.
- RAM: `vm_stat` + `sysctl -n hw.memsize`.
- Disk: `df -k /`.
- Net: delta of `netstat -ibn` byte counters between polls.
- Battery: `pmset -g batt` (absent → `null`).
- Temp: best-effort; `null` if it needs sudo.
- Top: `ps -Aceo pcpu,comm -r | head`.

System sampling is cached with the existing `/usage` TTL and computed with `Promise.all`.

### 7.2 New `POST /action` endpoint (remote)

Strict **allowlist** — the bridge never runs arbitrary input:

| action | params | Mac command (illustrative) |
|--------|--------|----------------------------|
| `open_url` | `url` (https only, validated) | `open -a Safari <url>` |
| `open_app` | `name` (must be in `actions.json`) | `open -a <name>` |
| `shortcut` | `name` (must be in `actions.json`) | `shortcuts run <name>` |
| `media` | `key ∈ {playpause,next,prev}` | AppleScript media key |
| `volume` | `dir ∈ {up,down,mute}` | `osascript -e 'set volume …'` |
| `lock` | — | lock screen (CGSession suspend / keystroke) |
| `display_sleep` | — | `pmset displaysleepnow` |

- Request: `POST /action` `{ "token": "<pairing>", "action": "open_url", "url": "https://…" }`.
- Response: `{ ok: true }` or `{ ok:false, error }`. 401 on bad/missing token.
- `open_url` accepts only `https://` after URL parsing; `open_app`/`shortcut` names must
  match an entry in the user's `actions.json` (no free-form app launching).

### 7.3 Security — pairing token

- On first run the bridge generates a random token, persists it (e.g. `~/.config/ai-usage-bridge/pairing.json`, `chmod 600`), and prints it + shows it for onboarding.
- The device stores the token (Preferences) entered during setup (same portal as host/port,
  or via QR/USB in later phases).
- **Every** `POST /action` requires the token. `GET /usage` remains unauthenticated read-only
  (percentages only) — but a config flag can require the token for `/usage` too.
- Bind to LAN; document clearly in README/SECURITY.md what enabling the remote means. Remote
  can be **disabled by default** via env/flag for users who only want the dashboard.

### 7.4 mDNS / Bonjour discovery (enables zero-typing setup)

- Bridge advertises `_aiusage._tcp` (port + a short instance name) via `dns-sd`
  (spawn `dns-sd -R …`) or a tiny stdlib mDNS responder — no npm dep.
- Firmware browses for the service and auto-fills host/port → the user never types an IP.
- Falls back to manual host/port entry if discovery fails.

## 8. Transport layer — USB / Wi-Fi / BLE

All three are supported, phased by value and effort:

- **Wi-Fi (exists, primary wireless).** Current HTTP path. Gains mDNS discovery (§7.4).
- **USB (plug-and-play, best for a retail unit).** ESP32-S3 native USB-CDC. The device
  exposes a serial link; the Mac app talks to `/dev/tty.usbmodem*`. **Zero network config**
  and the remote channel is physically scoped (no LAN exposure) → strongest security for
  `/action`. A small serial framing (newline-delimited JSON) mirrors the HTTP contract:
  device sends `{"cmd":"usage"}` / `{"cmd":"action",…}`, Mac replies with the same payloads.
- **BLE (last, optional).** ESP32-S3 BLE GATT service exposing usage (notify) + an action
  characteristic (write). The Mac side needs a BLE central (CoreBluetooth via a small helper,
  or `noble`), which is the flakiest of the three — implemented last, behind a flag.

A transport abstraction on both sides keeps `usage`/`action` semantics identical regardless
of pipe (HTTP / serial / GATT).

## 9. Security & privacy

- **No login-password automation.** macOS blocks synthetic keystrokes at the secure lock
  screen, and storing a plaintext password on a giveaway/retail device is unacceptable and
  contrary to the project's stance. Convenient unlock = Touch ID / Apple Watch (out of scope).
- **Allowlist-only command channel** with a **pairing token**; destructive actions
  (lock/sleep) require it. USB transport removes LAN exposure entirely.
- **`open_url` restricted to `https://`**; app/shortcut names restricted to `actions.json`.
- Token stored `chmod 600` on Mac and in device Preferences; never logged.
- Update **SECURITY.md / PRIVACY.md** to describe the new command surface and how to disable
  the remote.

## 10. Commercial "plug-and-play" track (later phase)

Vision: customer buys the unit, plugs in, scans a QR, minimal setup, it works. Reality: the
Mac still needs a helper running (to read usage/stats and execute actions), so the piece that
makes it feel like a product is:

1. **Signed + notarized macOS menu-bar app** wrapping the bridge (one-click install; not
   "run node in Terminal"). Requires an Apple Developer account. Could fold into the sibling
   *AI Usage Bar for macOS* app.
2. **mDNS auto-discovery** (§7.4) so no IP typing.
3. **QR onboarding**: the device shows a QR → phone opens the setup page / joins the
   captive portal. USB mode barely needs it (device appears on plug-in).
4. **First-connect pairing prompt** on the Mac app ("Allow this device?") establishing the token.
5. **Pre-flashed firmware** so the customer never flashes.

**Before selling:** clear branding/attribution (builds on captainkie's macOS app + Waveshare
hardware — MIT allows commercial use, but coordinate on name/logo); do not imply an official
Anthropic product; sort board sourcing/warranty. These are business items, not engineering
blockers, and are tracked here for visibility.

## 11. Firmware structure changes

- `config.h`: extend the data model — add a `SystemState` struct (cpu/mem/disk/net/battery/
  temp/top) and a remote-actions list; add tunables (screen count, dot timings).
- `net.h`: parse the new `system` block; add `net_action(action, params, token)` (HTTP POST
  now; serial/BLE later via the transport abstraction); store pairing token in Preferences.
- Sketch: replace `ui_build()` with a screen manager + tileview; three `screen_*_build()`
  and `screen_*_update()`; keep the single render timer, dispatching to the active tile.
- The mascot/gauge engine (`mascot.c`) is untouched.

## 12. Phasing / milestones

- **Phase 1 — buildable & testable now (no board needed):** bridge `system` block, `/action`
  endpoint + pairing token + `actions.json`, mDNS advertise. Verify with `curl`/scripts on the
  Mac. Firmware written against it, tested when the AliExpress board arrives.
- **Phase 2 — device screens (needs board):** tileview + 3 screens + swipe/dots; wire monitor
  + remote to the bridge over Wi-Fi.
- **Phase 3 — USB transport:** serial framing + Mac-side serial reader; plug-and-play path.
- **Phase 4 — commercial onboarding:** signed menu-bar app, QR, pairing UX, pre-flash.
- **Phase 5 — BLE (optional):** GATT service + Mac BLE central.

## 13. Testing & verification

- **Bridge:** unit-check each system reader on this Mac; snapshot `/usage` JSON; exercise
  `/action` with valid/invalid tokens and non-allowlisted inputs (expect 401/400).
- **Mockup:** already verified in-browser (3 screens, nav, no console errors); serves as the
  visual contract for the firmware.
- **Firmware:** on-hardware test when the board arrives — panel bring-up, swipe feel, live
  data, remote round-trip; confirm page-0 parity with the shipped version.

## 14. Open questions / risks

- Temp on Apple Silicon without sudo — likely `null`; acceptable.
- `lock` implementation that works headless without a keystroke to loginwindow — verify the
  chosen method (CGSession suspend) actually locks on current macOS.
- Serial port identification when multiple USB-CDC devices are present (match by VID/PID +
  a handshake).
- BLE central reliability on macOS from Node — the reason BLE is last.

## 15. Deliverables checklist

- [x] Interactive 3-screen mockup (`design/mockup.html`) + GitHub Pages live demo.
- [ ] Bridge: `system` block in `/usage`.
- [ ] Bridge: `POST /action` + allowlist + pairing token + `actions.json`.
- [ ] Bridge: mDNS advertise.
- [ ] Firmware: screen manager + tileview + page dots.
- [ ] Firmware: Monitor screen + Remote screen wired to bridge.
- [ ] Firmware: pairing-token entry + `net_action()`.
- [ ] USB transport (both sides).
- [ ] Docs: SECURITY.md / PRIVACY.md update for the command surface.
- [ ] (Later) BLE transport; commercial onboarding (signed app, QR, pre-flash).
