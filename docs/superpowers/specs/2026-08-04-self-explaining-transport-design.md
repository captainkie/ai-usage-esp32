# Self-Explaining Transport (device says what's wrong + user-selectable path) — Design

> **Status:** design approved 2026-08-04. Firmware-only; the bridge is not touched.
> Build-then-verify-**with-user** on the board. BLE is explicitly out of scope here
> (roadmap — see §8).

## 1. Context — why this exists

On 2026-08-04 the device sat on **"connecting…"** at the office and only a physical
reset brought it back. The investigation found three stacked defects, all fixed in
PR #23 (`e61d474`, `7582bb7`, `d59c865`):

| # | Defect | Fixed by |
|---|--------|----------|
| 1 | Bridge pinned `USB_PORT` to a `/dev/cu.usbmodem…` that no longer existed — USB dead for a week, 3,686 log lines of `stty: No such file` | `e61d474` |
| 2 | `net_portal()` had no timeout; one tap on the LIVE chip wedged `loop()` forever | `d59c865` |
| 3 | No automatic recovery from a wedged superloop | `d59c865` (60 s WDT) |

Those fixes stop the failures. They do **not** fix the thing that made a half-day
disappear: **the device knew exactly what was wrong the entire time and never said so.**

`net_fetch()` writes a precise reason into `g_state.err` — `"wifi"`, `"http -1"`,
`"no bridge"`, `"http 401"` — `loop()` prints it to serial (`ai-usage-esp32.ino:1000`),
and `render_cb()` then **discards it** and draws `"connecting..."`
(`ai-usage-esp32.ino:744`). Reading the real reason requires a serial monitor at 115200
baud. Nobody receiving this as a gift will do that.

This spec makes the device explain itself, and lets the user pin which transport it uses.

## 2. Goals / non-goals

**Goals**

1. The panel states the actual reason it has no data, and the next action to take.
2. The panel states which transport is live (USB / Wi-Fi), not a generic "LIVE".
3. The user can force a transport via `pixie.json` (and the setup portal).
4. The decision logic is a pure function, unit-tested on the host.

**Non-goals**

- BLE (§8). The type is designed to accept it later; no BLE code here.
- Health scoring, failover hysteresis, connection history, a 5th screen tile.
- Any bridge change. Blast radius stays inside `firmware/`.

**Success criteria** — the four situations that actually cost time today are each
diagnosable **from the panel alone, without a Mac, cable, or serial monitor**:
bridge not running · wrong Wi-Fi password · client-isolated network · rejected token.

## 3. Architecture

One new header-only unit, matching how `sdconf.h` / `wifistore.h` are built:

**`firmware/ai-usage-esp32/transport.h`**

Single responsibility: answer *"what is the connection state, and what should the user
do about it?"* It performs **no I/O and owns no globals** — `loop()` feeds it facts it
already has, the UI reads the verdict.

```c
enum TransportKind { TR_NONE = 0, TR_USB, TR_WIFI };   // TR_BLE slots in later
enum TransportPref { PREF_AUTO = 0, PREF_USB, PREF_WIFI };

typedef struct {
  TransportKind active;     // what delivered the last good frame
  bool          healthy;    // active transport is serving data right now
  char          label[10];  // "USB" / "WI-FI" / "--"
  char          hint[64];   // reason + next step; "" when healthy
} TransportState;

TransportState transport_evaluate(bool usb_plugged,
                                  uint32_t ms_since_usb_frame,  // UINT32_MAX = never
                                  bool wifi_connected,
                                  bool have_data,
                                  const char *last_err,         // g_state.err
                                  TransportPref pref);
```

Pure and **total**: every input combination returns a valid state. No asserts, no
allocation, fixed-size buffers, so it is safe to call every loop tick and safe for the
render task to read.

`usb_plugged` comes from `HWCDC::isPlugged()` (public static, SOF/timer-based —
verified present in core 3.3.11). This is what makes "no cable" distinguishable from
"cable in, bridge not running" — the exact case that broke today.

## 4. Decision table

`USB_FRESH_MS = 30000`, matching the existing `usbFresh` window in `loop()`.
Under `PREF_AUTO`, **USB wins while its frames are fresh** — this preserves today's
behaviour exactly; the change is that it becomes visible, not that it becomes different.

| Condition | `label` | `hint` |
|---|---|---|
| USB frame fresh | `USB` | *(empty — model name shows)* |
| Wi-Fi connected, data OK | `WI-FI` | *(empty — model name shows)* |
| `usb_plugged`, no fresh frame | `USB` | `USB ready · start the bridge on your Mac` |
| No cable, `err == "wifi"` | `--` | `no wi-fi · plug in USB or tap to set up` |
| Wi-Fi up, `err == "http -1"` | `WI-FI` | `network blocks this Mac · use USB` |
| Wi-Fi up, `err == "no bridge"` | `WI-FI` | `bridge not found · check it's running` |
| `http 401` | *(as selected)* | `pairing token rejected · tap to re-enter` |
| Other `http NNN` / `json` / `url` | *(as selected)* | `bridge error NNN · check the Mac` |
| Preference locked, that path down | locked kind | `USB locked in pixie.json · not available` |

`net_fetch()` builds these with `snprintf(out->err, …, "http %d", code)`, so the HTTP
rows are matched by **parsing the number** after the `"http "` prefix, not by comparing
whole strings. `-1` (connect refused/failed) and `401` get their own rows above; every
other code falls through to the generic row. `"wifi"`, `"no bridge"`, `"json"` and
`"url"` are exact literals and are compared as such.

**A locked transport does not silently fall back.** It reports that the requested path
is unavailable. Silent switching is precisely what hid the week-long USB outage; the
default is `auto`, so most users never meet this branch.

**Precedence under `PREF_AUTO`.** The table above does not by itself say which link
wins when USB is plugged-but-silent *and* Wi-Fi is connected-but-failing at the same
time — that ambiguity produced a real bug (§9 does not cover it either; found during
on-device verification). The precedence, most specific first:

1. `usb_fresh` → `USB` (fresh frames within `USB_FRESH_MS`)
2. Wi-Fi *actually delivering* (`wifi_connected && have_data && last_err` empty) → `WI-FI`
3. `usb_plugged` (cable in, no fresh frame) → `USB`
4. `wifi_connected` (associated, not delivering) → `WI-FI`
5. otherwise → `--` (none)

Step 3 outranks step 4: a plugged-in cable beats a Wi-Fi link that is merely
*associated* to an AP but not delivering data (e.g. a client-isolated office network),
because "start the bridge on your Mac" is one command away, while "use USB" is a dead
end when USB is already plugged in. Wi-Fi only outranks the cable when it is actually
working (step 2) — a healthy link should not be abandoned for a silent one just because
the cable happens to be in.

## 5. Where it surfaces

**5.1 The status chip (`lblLive`, top-right, present on every screen)**

Today it renders `LIVE` (green) / `CACHED` (amber, when the bridge serves a stale
last-known-good reading) / dim `LIVE`. **`CACHED` must not be lost** — it is the honest
staleness signal added in PR #14/#15.

New rendering, preserving both meanings:

| State | Chip |
|---|---|
| Healthy | `USB` / `WI-FI` — green |
| Healthy but stale | `USB·CACHED` / `WI-FI·CACHED` — amber |
| Down | `USB` / `WI-FI` / `--` — grey |

**`label` carries the transport name only** (`"WI-FI"` = 5 chars, fits `label[10]`);
`render_cb()` appends `·CACHED` itself, because it already reads `pr->stale`. Staleness
describes the *usage data*, not the *link* — keeping it out of `transport.h` preserves
the module's single responsibility and avoids a 12-character label overflowing the
buffer.

**5.2 The model line on screen ① (`lblModel`)**

When there is no live data, draw `hint` instead of `"connecting..."`. When healthy,
behaviour is unchanged (model name). Screens ②③④ have no model line; the chip covers
them, and screen ① is the default view.

**5.3 Serial** — unchanged. `[net] fetch failed: <err>` stays for our own debugging.

## 6. Configuration

`pixie.json` gains one optional key (absent ⇒ `auto`):

```json
{ "transport": "auto" }
```

- `sdconf.h` — parse into `PixieConfig.transport`; unknown/missing string ⇒ `auto`.
- NVS — persist via `prefs.putUChar("transport", …)` beside `host` / `port` / `token`,
  so it survives card removal like every other imported setting.
- Setup portal — one extra `WiFiManagerParameter` (`auto` / `usb` / `wifi`) so users
  without a TF card can set it too. Written by the existing `net_save_params()`.

`firmware/pixie.example.json` and the README's `pixie.json` table are updated.

## 7. Concurrency

`transport_evaluate()` is called from `loop()` (the net task). Its result is copied into
a global under `portENTER_CRITICAL(&g_mux)`, the same guard already used for `g_state`,
because the LVGL render task runs on its own task and would otherwise read a torn
struct. `label` / `hint` are fixed-size arrays — no allocation on the render path.

## 8. BLE — deliberately deferred

BLE is a roadmap item and **must not cost anything when it lands**. The blocker is not
the ESP32 side (NimBLE ships in the core) but the Mac side: Node cannot act as a BLE
central without a native module (`@abandonware/noble` → node-gyp + Xcode CLT, plus
macOS Bluetooth TCC approval which a LaunchAgent cannot reliably prompt for). That
breaks the **"zero dependencies"** promise stated in `bridge/package.json`,
`README.md:114`, and `bridge/README.md:3`.

This design leaves room without paying for it now: `TransportKind` reserves `TR_BLE`,
and `transport_evaluate()` takes facts rather than reading hardware, so a third
transport adds rows to the table and nothing else.

## 9. Testing

**Host tests** — `design/tools/transport_ctest.cpp`, following the existing
`sdconf_ctest.cpp` pattern (pure function, no board):

- every row of §4;
- the `USB_FRESH_MS` boundary (29 999 / 30 000 / 30 001 ms);
- `PREF_USB` and `PREF_WIFI` with that path both up and down;
- `ms_since_usb_frame == UINT32_MAX` (never received a frame);
- `last_err` empty, unknown, and malformed.

Build: `c++ -std=c++17 -I firmware/ai-usage-esp32 design/tools/transport_ctest.cpp && ./a.out`

**On-device** (with the user):

| Check | Expected |
|---|---|
| Bridge stopped, cable in | `USB ready · start the bridge on your Mac` |
| Cable out, Wi-Fi off | `no wi-fi · plug in USB or tap to set up` |
| Office network | `network blocks this Mac · use USB` |
| Wrong token via portal | `pairing token rejected · tap to re-enter` |
| Healthy USB and healthy Wi-Fi | chip reads `USB` / `WI-FI` correctly |
| `transport: "usb"` with cable out | locked message, no silent Wi-Fi fallback |
| Bridge 429 | chip still shows `·CACHED` (no regression) |

**Regression** — `cd bridge && node --test` stays 23/23 (no bridge change).

## 10. Risks

| Risk | Mitigation |
|---|---|
| Losing the `CACHED` signal while reworking the chip | Explicit test row; §5.1 defines both meanings |
| Hint text too long for 640 px | Cap at 64 chars, verify on-device, `LV_LABEL_LONG_WRAP` already set |
| Extra flash on a build already at 53 % | Fixed literals only; expected well under 1 KB |
| `isPlugged()` semantics differ from expectation | Verified in core 3.3.11 header; on-device row covers it |
