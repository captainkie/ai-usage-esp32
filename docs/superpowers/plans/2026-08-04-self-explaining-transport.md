# Self-Explaining Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the panel state *why* it has no data and *which* link is live, and let the user pin that link in `pixie.json`.

**Architecture:** One new header-only unit, `transport.h`, exposing a pure `transport_evaluate()` that turns facts `loop()` already holds into a display label plus a plain-language hint. `loop()` calls it and publishes the result under the existing `g_mux`; `render_cb()` draws it. No bridge changes, no new dependencies.

**Tech Stack:** Arduino-ESP32 3.3.11 (ESP32-S3), LVGL 8.4, ArduinoJson 6.21.5, Waveshare ESP32-S3-Touch-LCD-3.49 BSP.

**Spec:** `docs/superpowers/specs/2026-08-04-self-explaining-transport-design.md`

## Global Constraints

- **No new dependencies anywhere.** The bridge's "zero dependencies" promise appears in `bridge/package.json`, `README.md:114`, `bridge/README.md:3`. This feature touches `firmware/` only.
- **Header-only firmware units**, matching `sdconf.h` / `wifistore.h`. Pure logic must compile on the host with no Arduino headers (guard Arduino-only code in `#ifdef ARDUINO`).
- **No heap allocation on the render path.** Fixed-size `char` arrays only.
- **Cross-task state crosses tasks under `portENTER_CRITICAL(&g_mux)`.** LVGL renders from its own task; an unguarded read tears the struct.
- **Big boot-path buffers must be `static`.** The Arduino loop task has an 8 KB stack; multi-KB locals have already caused a spinlock-assert crash in this project (`de96e2e`).
- **The `CACHED` amber state must survive.** It is the honest-staleness signal from PR #14/#15.
- **Middle dot `·` (U+00B7) does NOT render** in this build's Montserrat — use the bullet `•` (`\xE2\x80\xA2`), which is already proven in the brand label at font 14.
- **Build FQBN:** `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB`. Baseline is 53% flash / 36% RAM.
- **Flashing:** the bridge LaunchAgent holds the serial port — `launchctl bootout gui/$(id -u)/com.aiusage.bridge` first, `launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.aiusage.bridge.plist` after.

## File Structure

| File | Responsibility |
|---|---|
| `firmware/ai-usage-esp32/transport.h` | **new** — pure link-state decision logic; no I/O, no globals |
| `design/tools/transport_ctest.cpp` | **new** — host tests for the above |
| `firmware/ai-usage-esp32/config.h` | add `USB_FRESH_MS` (currently a magic `30000` in `loop()`) |
| `firmware/ai-usage-esp32/sdconf.h` | parse `"transport"` out of `pixie.json` |
| `firmware/ai-usage-esp32/net.h` | persist/read the preference; add the portal field |
| `firmware/ai-usage-esp32/ai-usage-esp32.ino` | call `transport_evaluate()`; render label + hint |
| `firmware/pixie.example.json` | document the new key |
| `README.md` | replace the serial-log table with "read it off the panel" |

---

### Task 1: `transport.h` — the pure decision core

**Files:**
- Create: `firmware/ai-usage-esp32/transport.h`
- Create: `design/tools/transport_ctest.cpp`
- Modify: `firmware/ai-usage-esp32/config.h` (add `USB_FRESH_MS`)

**Interfaces:**
- Consumes: `MAX_WIFI_APS`-style constants from `config.h`; nothing from other tasks.
- Produces: `enum TransportKind { TR_NONE=0, TR_USB, TR_WIFI }`, `enum TransportPref { PREF_AUTO=0, PREF_USB, PREF_WIFI }`, `TransportState { int active; bool healthy; char label[10]; char hint[64]; }`, `int transport_pref_parse(const char *)`, `TransportState transport_evaluate(bool usb_plugged, uint32_t ms_since_usb_frame, bool wifi_connected, bool have_data, const char *last_err, int pref)`.

- [ ] **Step 1: Add the shared freshness constant**

In `firmware/ai-usage-esp32/config.h`, directly under the `// ---- bridge polling ----` block:

```c
// How long a USB frame counts as "fresh". loop() skips the Wi-Fi poll inside this
// window, and transport.h calls the link healthy inside it — one constant so the
// two can never disagree.
#define USB_FRESH_MS 30000
```

- [ ] **Step 2: Write the failing host test**

Create `design/tools/transport_ctest.cpp`:

```cpp
// Host test for the pure transport decision logic (no Arduino, no board).
// Build: c++ -std=c++17 -I firmware/ai-usage-esp32 \
//            design/tools/transport_ctest.cpp -o /tmp/transport_ctest && /tmp/transport_ctest
#include <cstdio>
#include <cstring>
#include <cstdint>
#include "transport.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); fails++; } } while(0)

// usb_plugged, ms_since_usb_frame, wifi_connected, have_data, last_err, pref
static TransportState ev(bool up, uint32_t ms, bool wc, bool hd, const char *e, int p) {
  return transport_evaluate(up, ms, wc, hd, e, p);
}

static void test_healthy_paths() {
  TransportState t = ev(true, 1000, false, true, "", PREF_AUTO);
  CHECK(t.active == TR_USB); CHECK(t.healthy); CHECK(strcmp(t.label,"USB")==0); CHECK(t.hint[0]==0);

  t = ev(false, UINT32_MAX, true, true, "", PREF_AUTO);
  CHECK(t.active == TR_WIFI); CHECK(t.healthy); CHECK(strcmp(t.label,"WI-FI")==0); CHECK(t.hint[0]==0);
}

static void test_usb_freshness_boundary() {
  CHECK(ev(true, USB_FRESH_MS - 1, false, true, "", PREF_AUTO).healthy);
  CHECK(!ev(true, USB_FRESH_MS,     false, true, "", PREF_AUTO).healthy);
  CHECK(!ev(true, USB_FRESH_MS + 1, false, true, "", PREF_AUTO).healthy);
  CHECK(!ev(true, UINT32_MAX,       false, true, "", PREF_AUTO).healthy);
}

static void test_bridge_not_running() {
  // cable in, host present, but no frames -> the exact failure of 2026-08-04
  TransportState t = ev(true, UINT32_MAX, false, false, "wifi", PREF_AUTO);
  CHECK(t.active == TR_USB);
  CHECK(!t.healthy);
  CHECK(strstr(t.hint, "start the bridge") != NULL);
}

static void test_no_cable_no_wifi() {
  TransportState t = ev(false, UINT32_MAX, false, false, "wifi", PREF_AUTO);
  CHECK(t.active == TR_NONE);
  CHECK(strcmp(t.label, "--") == 0);
  CHECK(strstr(t.hint, "no wi-fi") != NULL);
}

static void test_client_isolation() {
  TransportState t = ev(false, UINT32_MAX, true, false, "http -1", PREF_AUTO);
  CHECK(t.active == TR_WIFI);
  CHECK(!t.healthy);
  CHECK(strstr(t.hint, "use USB") != NULL);
}

static void test_error_mapping() {
  CHECK(strstr(ev(false,UINT32_MAX,true,false,"http 401",   PREF_AUTO).hint, "token") != NULL);
  CHECK(strstr(ev(false,UINT32_MAX,true,false,"no bridge",  PREF_AUTO).hint, "not found") != NULL);
  CHECK(strstr(ev(false,UINT32_MAX,true,false,"http 500",   PREF_AUTO).hint, "500") != NULL);
  CHECK(strstr(ev(false,UINT32_MAX,true,false,"json",       PREF_AUTO).hint, "bridge") != NULL);
  CHECK(strstr(ev(false,UINT32_MAX,true,false,"url",        PREF_AUTO).hint, "bridge") != NULL);
  // never crashes on odd input
  CHECK(ev(false,UINT32_MAX,true,false,NULL,       PREF_AUTO).hint[0] != 0);
  CHECK(ev(false,UINT32_MAX,true,false,"http ",    PREF_AUTO).hint[0] != 0);
  CHECK(ev(false,UINT32_MAX,true,false,"weird",    PREF_AUTO).hint[0] != 0);
}

static void test_locked_does_not_fall_back() {
  // USB locked, cable out, Wi-Fi perfectly fine -> must NOT report Wi-Fi
  TransportState t = ev(false, UINT32_MAX, true, true, "", PREF_USB);
  CHECK(t.active == TR_USB);
  CHECK(!t.healthy);
  CHECK(strstr(t.hint, "locked") != NULL);

  // Wi-Fi locked, not connected, USB feeding fine -> must NOT report USB
  t = ev(true, 500, false, true, "wifi", PREF_WIFI);
  CHECK(t.active == TR_WIFI);
  CHECK(!t.healthy);
  CHECK(strstr(t.hint, "locked") != NULL);

  // locked to a path that IS working stays healthy
  CHECK(ev(true, 500, false, true, "", PREF_USB).healthy);
  CHECK(ev(false, UINT32_MAX, true, true, "", PREF_WIFI).healthy);
}

static void test_pref_parse() {
  CHECK(transport_pref_parse("auto") == PREF_AUTO);
  CHECK(transport_pref_parse("usb")  == PREF_USB);
  CHECK(transport_pref_parse("wifi") == PREF_WIFI);
  CHECK(transport_pref_parse("")     == PREF_AUTO);
  CHECK(transport_pref_parse(NULL)   == PREF_AUTO);
  CHECK(transport_pref_parse("USB")  == PREF_AUTO);   // case-sensitive by design
}

static void test_buffers_bounded() {
  TransportState t = ev(false, UINT32_MAX, true, false, "http -2147483648", PREF_AUTO);
  CHECK(strlen(t.label) < sizeof(t.label));
  CHECK(strlen(t.hint)  < sizeof(t.hint));
}

int main() {
  test_healthy_paths();
  test_usb_freshness_boundary();
  test_bridge_not_running();
  test_no_cable_no_wifi();
  test_client_isolation();
  test_error_mapping();
  test_locked_does_not_fall_back();
  test_pref_parse();
  test_buffers_bounded();
  printf(fails ? "\n%d CHECK(s) FAILED\n" : "\nALL PASS\n", fails);
  return fails ? 1 : 0;
}
```

- [ ] **Step 3: Run it to verify it fails**

```bash
c++ -std=c++17 -I firmware/ai-usage-esp32 design/tools/transport_ctest.cpp -o /tmp/transport_ctest
```

Expected: FAIL — `fatal error: 'transport.h' file not found`.

- [ ] **Step 4: Write `transport.h`**

Create `firmware/ai-usage-esp32/transport.h`:

```c
// Which link is delivering data, and what to tell the user when none is.
//
// transport_evaluate() is PURE — no I/O, no globals, no allocation — so it is
// host-testable like sdconf_parse() / wifistore_parse(). loop() feeds it facts it
// already has; render_cb() draws the verdict. Before this existed the firmware knew
// exactly why it had no data (net_fetch writes g_state.err) and showed the user
// "connecting..." regardless, so diagnosing it needed a serial monitor.
#pragma once
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include "config.h"

enum TransportKind { TR_NONE = 0, TR_USB, TR_WIFI };   // TR_BLE reserved — roadmap
enum TransportPref { PREF_AUTO = 0, PREF_USB, PREF_WIFI };

typedef struct {
  int  active;      // TransportKind
  bool healthy;     // the selected link is serving data right now
  char label[10];   // "USB" / "WI-FI" / "--"; render_cb appends the CACHED suffix
  char hint[64];    // reason + next step; "" when healthy
} TransportState;

// pixie.json / portal string -> preference. Anything unrecognised means auto, so a
// typo degrades to the sensible default instead of pinning a link the user did not want.
static int transport_pref_parse(const char *s) {
  if (!s)                     return PREF_AUTO;
  if (strcmp(s, "usb")  == 0) return PREF_USB;
  if (strcmp(s, "wifi") == 0) return PREF_WIFI;
  return PREF_AUTO;
}

// net_fetch() formats HTTP failures as snprintf("http %d", code), so match by parsing
// the number rather than comparing whole strings. Returns 0 when `err` is not one of
// those (0 is never a real HTTP status).
static int transport_http_code(const char *err) {
  if (!err || strncmp(err, "http ", 5) != 0) return 0;
  return (int)strtol(err + 5, NULL, 10);
}

static TransportState transport_evaluate(bool usb_plugged,
                                         uint32_t ms_since_usb_frame,   // UINT32_MAX = never
                                         bool wifi_connected,
                                         bool have_data,
                                         const char *last_err,          // g_state.err
                                         int pref) {
  TransportState t;
  memset(&t, 0, sizeof(t));
  const char *err = last_err ? last_err : "";
  bool usb_fresh = ms_since_usb_frame < USB_FRESH_MS;

  // 1. Pick the link. A locked preference is honoured even when it is down — silently
  //    switching is what hid a week-long USB outage; the user gets told instead.
  if      (pref == PREF_USB)  t.active = TR_USB;
  else if (pref == PREF_WIFI) t.active = TR_WIFI;
  else if (usb_fresh)         t.active = TR_USB;      // matches loop()'s usbFresh rule
  else if (wifi_connected)    t.active = TR_WIFI;
  else if (usb_plugged)       t.active = TR_USB;      // cable in but silent -> blame USB
  else                        t.active = TR_NONE;

  strlcpy(t.label, t.active == TR_USB ? "USB" : t.active == TR_WIFI ? "WI-FI" : "--",
          sizeof(t.label));

  // 2. Healthy? USB freshness alone decides the USB case: while USB feeds, loop() skips
  //    the Wi-Fi poll, so g_state.err still holds whatever failed before the cable went in.
  if (t.active == TR_USB && usb_fresh) { t.healthy = true; return t; }
  if (t.active == TR_WIFI && wifi_connected && have_data && err[0] == 0) {
    t.healthy = true; return t;
  }

  // 3. Explain, most specific first. "\xE2\x80\xA2" is the bullet; the middle dot
  //    U+00B7 has no glyph in this build's Montserrat.
  if (t.active == TR_USB) {
    if (usb_plugged)
      strlcpy(t.hint, "USB ready \xE2\x80\xA2 start the bridge on your Mac", sizeof(t.hint));
    else if (pref == PREF_USB)
      strlcpy(t.hint, "USB locked in pixie.json \xE2\x80\xA2 plug in the cable", sizeof(t.hint));
    else
      strlcpy(t.hint, "no wi-fi \xE2\x80\xA2 plug in USB or tap to set up", sizeof(t.hint));
    return t;
  }

  if (t.active == TR_WIFI) {
    if (!wifi_connected) {
      if (pref == PREF_WIFI)
        strlcpy(t.hint, "wi-fi locked in pixie.json \xE2\x80\xA2 not connected", sizeof(t.hint));
      else
        strlcpy(t.hint, "no wi-fi \xE2\x80\xA2 plug in USB or tap to set up", sizeof(t.hint));
      return t;
    }
    int code = transport_http_code(err);
    if (code == -1)
      strlcpy(t.hint, "network blocks this Mac \xE2\x80\xA2 use USB", sizeof(t.hint));
    else if (code == 401)
      strlcpy(t.hint, "pairing token rejected \xE2\x80\xA2 tap to re-enter", sizeof(t.hint));
    else if (code != 0)
      snprintf(t.hint, sizeof(t.hint), "bridge error %d \xE2\x80\xA2 check the Mac", code);
    else if (strcmp(err, "no bridge") == 0)
      strlcpy(t.hint, "bridge not found \xE2\x80\xA2 check it's running", sizeof(t.hint));
    else if (strcmp(err, "wifi") == 0)
      strlcpy(t.hint, "no wi-fi \xE2\x80\xA2 plug in USB or tap to set up", sizeof(t.hint));
    else
      strlcpy(t.hint, "waiting for the bridge\xE2\x80\xA6", sizeof(t.hint));
    return t;
  }

  strlcpy(t.hint, "no wi-fi \xE2\x80\xA2 plug in USB or tap to set up", sizeof(t.hint));
  return t;
}
```

- [ ] **Step 5: Run the tests to verify they pass**

```bash
c++ -std=c++17 -I firmware/ai-usage-esp32 design/tools/transport_ctest.cpp -o /tmp/transport_ctest && /tmp/transport_ctest
```

Expected: `ALL PASS`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add firmware/ai-usage-esp32/transport.h firmware/ai-usage-esp32/config.h design/tools/transport_ctest.cpp
git commit -m "feat(firmware): pure transport-state logic with host tests

The firmware already knows why it has no data — net_fetch writes 'wifi' /
'http -1' / 'no bridge' / 'http NNN' into g_state.err — and render_cb throws it
away and draws 'connecting...'. This adds the decision layer that turns those
facts into a link label and a plain-language next step.

Pure and total: no I/O, no globals, no allocation, every input combination
returns a valid state, so it is unit-tested on the host like sdconf_parse.
USB_FRESH_MS moves to config.h so loop()'s usbFresh window and the health rule
cannot drift apart. TR_BLE is reserved but not implemented."
```

---

### Task 2: carry the preference through `pixie.json`, NVS, and the portal

**Files:**
- Modify: `firmware/ai-usage-esp32/sdconf.h` (`PixieConfig`, `sdconf_parse`)
- Modify: `firmware/ai-usage-esp32/net.h` (`net_import_card`, `net_begin`, `net_save_params`, `net_portal`)
- Modify: `design/tools/sdconf_ctest.cpp` (new field coverage)
- Modify: `firmware/pixie.example.json`

**Interfaces:**
- Consumes: `transport_pref_parse()` and the `PREF_*` enum from Task 1.
- Produces: `PixieConfig.transport` (`char[8]`); the global `g_transport_pref` (`int`, one of `PREF_*`) declared in `net.h` and read by Task 3.

- [ ] **Step 1: Add the failing test to the existing host harness**

`design/tools/sdconf_ctest.cpp` already covers `sdconf_parse`. Add this function and call it from `main()`:

```cpp
static void test_transport_field() {
  PixieConfig c;
  CHECK(sdconf_parse("{\"wifi\":[],\"transport\":\"usb\"}", &c));
  CHECK(strcmp(c.transport, "usb") == 0);
  CHECK(transport_pref_parse(c.transport) == PREF_USB);

  // absent -> empty -> auto
  CHECK(sdconf_parse("{\"wifi\":[]}", &c));
  CHECK(c.transport[0] == 0);
  CHECK(transport_pref_parse(c.transport) == PREF_AUTO);

  // garbage value degrades to auto rather than pinning something unintended
  CHECK(sdconf_parse("{\"wifi\":[],\"transport\":\"carrier-pigeon\"}", &c));
  CHECK(transport_pref_parse(c.transport) == PREF_AUTO);
}
```

Add `#include "transport.h"` to the includes at the top of that file.

- [ ] **Step 2: Run it to verify it fails**

```bash
c++ -std=c++17 -I design/tools/vendor -I firmware/ai-usage-esp32 design/tools/sdconf_ctest.cpp -o /tmp/sdconf_ctest && /tmp/sdconf_ctest
```

Expected: FAIL — `no member named 'transport' in 'PixieConfig'`.

> If the compile instead fails with `'ArduinoJson.h' file not found`, the gitignored
> vendor header is missing. Restore it:
> `mkdir -p design/tools/vendor && curl -L -o design/tools/vendor/ArduinoJson.h https://github.com/bblanchon/ArduinoJson/releases/download/v6.21.5/ArduinoJson-v6.21.5.h`

- [ ] **Step 3: Add the field to `sdconf.h`**

In the `PixieConfig` struct, after `char port[6];`:

```c
  char transport[8];   // "auto" | "usb" | "wifi"; "" => auto
```

In `sdconf_parse`, next to the other `strlcpy` calls:

```c
  strlcpy(out->transport, doc["transport"] | "", sizeof(out->transport));
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
c++ -std=c++17 -I design/tools/vendor -I firmware/ai-usage-esp32 design/tools/sdconf_ctest.cpp -o /tmp/sdconf_ctest && /tmp/sdconf_ctest
```

Expected: `ALL PASS`.

- [ ] **Step 5: Persist and read it in `net.h`**

Add the include beside the others at the top of `net.h`:

```c
#include "transport.h"       // link-state decision logic + PREF_* preference
```

Add the global beside `g_host_pinned`:

```c
static int g_transport_pref = PREF_AUTO;   // user's pinned link (pixie.json / portal)
```

In `net_import_card()`, inside the `Preferences p;` block:

```c
  if (cfg.transport[0]) p.putString("transport", cfg.transport);
```

In `net_begin()`, in the second `g_prefs.begin("aiusage", true)` block (the re-read after the card import), before `g_prefs.end()`:

```c
  g_transport_pref = transport_pref_parse(g_prefs.getString("transport", "auto").c_str());
```

In `net_save_params()`, alongside the other `putString` calls — the portal parameter is added in the next step:

```c
  if (g_pTr) {
    String tr = g_pTr->getValue(); tr.trim();
    g_prefs.putString("transport", tr);
    g_transport_pref = transport_pref_parse(tr.c_str());
  }
```

and declare its handle beside `g_pTok`:

```c
static WiFiManagerParameter *g_pTr = nullptr;
```

- [ ] **Step 6: Add the portal field**

In **both** `net_begin()`'s portal block and `net_portal()`, beside the existing `pTok` parameter — a user with no TF card must be able to set this too:

```c
    static WiFiManagerParameter pTr("transport", "Link: auto / usb / wifi", "auto", 6);
    g_pTr = &pTr;
    wm.addParameter(&pTr);
```

> Each function already declares its own `static WiFiManagerParameter` locals, so
> declaring `pTr` in both is consistent with the existing code. In `net_portal()` seed
> it from the current preference instead of the literal `"auto"`:
> `g_transport_pref == PREF_USB ? "usb" : g_transport_pref == PREF_WIFI ? "wifi" : "auto"`.

- [ ] **Step 7: Document the key**

`firmware/pixie.example.json` — add after `"bridge_port": 8787`:

```json
  "transport": "auto"
```

(remember the comma on the preceding line)

- [ ] **Step 8: Verify the firmware still compiles**

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB" \
  --libraries <harness>/bsp/Arduino_Libraries/lvgl8 --libraries <harness>/bsp/Arduino_Libraries/SensorLib \
  --build-path <harness>/build <harness>/ai-usage-esp32
```

Expected: `0 errors`, ~53% flash. (Assemble the harness per `docs`/the build-flash recipe if it is not present.)

- [ ] **Step 9: Commit**

```bash
git add firmware/ai-usage-esp32/sdconf.h firmware/ai-usage-esp32/net.h design/tools/sdconf_ctest.cpp firmware/pixie.example.json
git commit -m "feat(firmware): let pixie.json and the portal pin the link

Adds transport: auto|usb|wifi, imported from the card into NVS beside host/port/
token so it survives card removal, and settable from the setup portal for users
with no card. Unrecognised values fall back to auto, so a typo cannot pin a link
the user never asked for."
```

---

### Task 3: surface it on the panel

**Files:**
- Modify: `firmware/ai-usage-esp32/ai-usage-esp32.ino` (globals, `loop()`, `render_cb()`)
- Modify: `README.md`

**Interfaces:**
- Consumes: `transport_evaluate()`, `TransportState`, `g_transport_pref` from Tasks 1–2.
- Produces: nothing further; this is the last task.

- [ ] **Step 1: Publish the state from `loop()`**

Add beside `g_have` (~line 48):

```c
static TransportState g_tr;               // published under g_mux; read by render_cb
```

In `loop()`, replace the magic number on the `usbFresh` line:

```c
  bool usbFresh = g_last_usb_ms != 0 && (millis() - g_last_usb_ms < USB_FRESH_MS);
```

Then, immediately **after** the `if (!usbFresh && (g_lastPoll == 0 || …))` polling block and before the action-queue drain, add:

```c
  // Publish the link verdict for the render task. HWCDC::isPlugged() is SOF/timer
  // based, so it separates "no cable" from "cable in, bridge not running" — the
  // failure that looked identical to every other one on the panel.
#if ARDUINO_USB_MODE == 1
  bool usbPlugged = HWCDC::isPlugged();
#else
  bool usbPlugged = (bool)Serial;
#endif
  uint32_t usbAge = g_last_usb_ms ? (uint32_t)(millis() - g_last_usb_ms) : UINT32_MAX;
  char errCopy[sizeof(g_state.err)];
  bool haveCopy;
  portENTER_CRITICAL(&g_mux);
  strlcpy(errCopy, g_state.err, sizeof(errCopy));
  haveCopy = g_have && g_state.ok;
  portEXIT_CRITICAL(&g_mux);
  TransportState tr = transport_evaluate(usbPlugged, usbAge,
                                         WiFi.status() == WL_CONNECTED,
                                         haveCopy, errCopy, g_transport_pref);
  portENTER_CRITICAL(&g_mux);
  g_tr = tr;
  portEXIT_CRITICAL(&g_mux);
```

- [ ] **Step 2: Read it in `render_cb()`**

In `render_cb()`, extend the existing snapshot block (~line 713) to copy the new struct in the same critical section:

```c
  UsageState st;
  TransportState tr;
  portENTER_CRITICAL(&g_mux);
  st = g_state;
  tr = g_tr;
  bool have = g_have;
  portEXIT_CRITICAL(&g_mux);
```

- [ ] **Step 3: Draw the hint instead of "connecting..."**

Replace the model-line block (~line 741–744):

```c
  // model + effort — when the link is unhealthy, say WHY and what to do about it
  // rather than an indefinite "connecting...".
  //
  // The hint is checked FIRST, before the model name. On a failed fetch loop() keeps
  // the previous providers block and only clears g_state.ok, so `have` stays true and
  // pr->model stays populated — testing the model first would mean the hint never
  // appeared again after the first successful poll. A healthy link produces an empty
  // hint, so this cannot hide the model name during normal operation (including the
  // CACHED/429 case, where the link itself is fine).
  if (tr.hint[0])                lv_label_set_text(lblModel, tr.hint);
  else if (have && pr->model[0]) lv_label_set_text(lblModel, pr->model);
  else if (have && !pr->linked)  lv_label_set_text(lblModel, "not linked");
  else if (have)                 lv_label_set_text(lblModel, "no live data");
  else                           lv_label_set_text(lblModel, "connecting...");
```

- [ ] **Step 4: Draw the link name on the status chip**

Replace the LIVE/CACHED block (~line 764–773). `CACHED` must survive — it is the honest-staleness signal, and the link name is appended to it rather than replacing it:

```c
  // Status chip: which link is live, plus the CACHED staleness flag. Staleness
  // describes the usage DATA, not the link, so transport.h stays out of it and the
  // suffix is composed here.
  char chip[24];
  if (have && st.ok && pr->stale) {
    snprintf(chip, sizeof(chip), LV_SYMBOL_WIFI " %s\xE2\x80\xA2CACHED", tr.label);
    lv_label_set_text(lblLive, chip);
    lv_obj_set_style_text_color(lblLive, LVC(COL_WARN), 0);
  } else {
    snprintf(chip, sizeof(chip), LV_SYMBOL_WIFI " %s", tr.label);
    lv_label_set_text(lblLive, chip);
    lv_obj_set_style_text_color(lblLive, LVC(tr.healthy ? COL_LIVE : 0x6B7180), 0);
  }
```

- [ ] **Step 5: Compile**

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB" \
  --libraries <harness>/bsp/Arduino_Libraries/lvgl8 --libraries <harness>/bsp/Arduino_Libraries/SensorLib \
  --build-path <harness>/build <harness>/ai-usage-esp32
```

Expected: `0 errors`, flash still ~53%.

- [ ] **Step 6: Flash and verify on the board (with the user)**

```bash
launchctl bootout gui/$(id -u)/com.aiusage.bridge
arduino-cli upload -p "$(ls /dev/cu.usbmodem* | head -1)" --fqbn "<fqbn above>" --input-dir <harness>/build <harness>/ai-usage-esp32
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.aiusage.bridge.plist
```

Walk the table — each row must be readable **from the panel alone**:

| Setup | Expected on panel |
|---|---|
| Bridge running, cable in | chip `USB`, green, model name shows |
| `launchctl bootout` the bridge, cable in | `USB ready • start the bridge on your Mac` |
| Cable out, home Wi-Fi up | chip `WI-FI`, green |
| Cable out, Wi-Fi off | `no wi-fi • plug in USB or tap to set up` |
| Office network | `network blocks this Mac • use USB` |
| `transport: "usb"` on the card, cable out, Wi-Fi up | `USB locked in pixie.json • plug in the cable`, **no** silent Wi-Fi fallback |
| Anthropic 429 in progress | chip still shows `•CACHED` in amber |

- [ ] **Step 7: Update the README**

In `README.md`, the `#### 📝 Note — "connecting…" has three different causes` block currently tells the reader to attach a serial monitor. Replace the intro sentence and the table's first column so the panel is the primary source, keeping the serial line as the fallback for developers:

```markdown
#### 📝 Note — when it can't reach your Mac, the panel says why

Pixie names the problem and the fix on screen; you should not need a serial monitor.
The status chip (top-right) shows which link is live — `USB` or `WI-FI` — and the line
under the brand replaces the model name with the reason:

| On the panel | What it means | What to do |
|---|---|---|
| `USB ready • start the bridge on your Mac` | Cable is in and your Mac is there, but nothing is pushing data. | Run `./bridge/install-macos.sh`. |
| `no wi-fi • plug in USB or tap to set up` | Never joined a network — usually a mistyped password. | Tap the chip and re-enter it. |
| `network blocks this Mac • use USB` | Joined, but the network blocks device-to-device traffic (client/VLAN isolation). | Nothing to fix — use USB here. |
| `bridge not found • check it's running` | Joined, but the Mac was never found. | Start the bridge, or type its IP. |
| `pairing token rejected • tap to re-enter` | The token no longer matches the bridge. | Tap the chip and paste the current one. |

Prefer one link over the other? Set `"transport": "auto" | "usb" | "wifi"` in
`pixie.json`, or in the setup portal. A pinned link is never silently swapped — if it
is unavailable the panel says so.
```

- [ ] **Step 8: Commit**

```bash
git add firmware/ai-usage-esp32/ai-usage-esp32.ino README.md
git commit -m "feat(firmware): panel names the fault and the fix

The status chip now reads USB / WI-FI instead of a generic LIVE, and the model
line carries the reason there is no data instead of an indefinite 'connecting...'.
CACHED survives as a suffix — it describes the usage data going stale, which is a
different thing from the link being down, so both stay visible.

Verified on hardware against every row of the spec's table."
```

---

## Self-Review

**Spec coverage**

| Spec section | Task |
|---|---|
| §3 architecture / `transport.h` | Task 1 |
| §4 decision table + HTTP-code parsing | Task 1 (steps 2, 4) |
| §5.1 chip, `CACHED` preserved | Task 3 step 4 |
| §5.2 model line | Task 3 step 3 |
| §5.3 serial unchanged | no change needed — nothing removes the `Serial.printf` |
| §6 `pixie.json` + NVS + portal | Task 2 |
| §7 `g_mux` guarding | Task 3 steps 1–2 |
| §8 BLE deferred, `TR_BLE` reserved | Task 1 step 4 (enum comment) |
| §9 host + on-device tests | Task 1 step 2, Task 2 step 1, Task 3 step 6 |
| §10 risks | covered by the `CACHED` row, the 64-char cap, and the bounded-buffer test |

**Placeholder scan** — no TBD/TODO; every code step carries real code. `<harness>` is a
path the operator substitutes, defined by the build recipe, not a missing decision.

**Type consistency** — `TransportState` fields (`active`, `healthy`, `label`, `hint`)
are used identically in Tasks 1–3. `transport_evaluate()`'s six-parameter signature
matches between `transport.h`, the host test's `ev()` wrapper, and the `loop()` call
site. `transport_pref_parse()` is used in `net.h` and both test files with the same
signature. `g_transport_pref` is declared in Task 2 and read in Task 3.
