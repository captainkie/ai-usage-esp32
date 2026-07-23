# Seamless Connectivity (WiFiMulti + TF-card config) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Carry Pixie between home ↔ office (+ hotspot) with no re-typing of Wi-Fi or the Mac IP, config portable on a TF card, Wi-Fi password kept off the removable card ("import-and-forget").

**Architecture:** WiFiManager keeps only the captive-portal UI (fallback). Connection moves to **WiFiMulti**, fed from an **NVS AP store** that is seeded at boot from an optional read-only `/pixie.json` on the TF card and from portal saves. The password ends up in on-board NVS; the card can then be removed. mDNS (already on this branch) resolves the Mac. The firmware never writes to the SD card.

**Tech Stack:** Arduino-ESP32 3.3.11, WiFiMulti (core), WiFiManager (tzapu 2.0.17), ArduinoJson v6.21.5, ESPmDNS, Waveshare `04_SD_Card` BSP driver, LVGL 8.

**Spec:** `docs/superpowers/specs/2026-07-23-seamless-connectivity-design.md`
**Branch:** `feat/seamless-firmware` (already carries mDNS; this PR supersedes draft PR #13).

---

## File Structure

| File | Responsibility | New/Mod |
|------|----------------|---------|
| `firmware/ai-usage-esp32/config.h` | `MAX_WIFI_APS`, `SD_CONFIG_PATH`, `WIFI_JOIN_TIMEOUT_MS`, `WifiCred` struct | Mod |
| `firmware/ai-usage-esp32/wifistore.h` | NVS AP store — pure JSON<->struct + merge (host-testable) + Preferences wrappers (on-device) | New |
| `firmware/ai-usage-esp32/sdconf.h` | Read-only TF-card config — pure `sdconf_parse` (host-testable) + `sdconf_load` (SD mount, on-device) | New |
| `firmware/ai-usage-esp32/net.h` | `net_begin` rewrite: import card → build WiFiMulti → connect → portal fallback | Mod |
| `firmware/ai-usage-esp32/ai-usage-esp32.ino` | serial banner on card import (setup() unchanged otherwise) | Mod |
| `firmware/pixie.example.json` | user template for the card | New |
| `firmware/ai-usage-esp32/src/sdcard_bsp/` | Waveshare `04_SD_Card` driver copied in | New (vendored) |
| `design/tools/sdconf_ctest.cpp` | host test for `sdconf_parse` + `wifistore_*` pure fns | New |
| `README.md` / `firmware/README.md` | Security + TF-card setup section | Mod |

**Pure vs hardware split (why the design is testable):** JSON parsing/serialization/merge — the bug-prone logic — is written as pure functions that compile on the host (guarded so Arduino-only headers are skipped when `ARDUINO` is undefined). SD mount, `Preferences`, `WiFiMulti`, and the portal are Arduino-bound and verified by compile + the on-device checklist.

**All commits** carry the repo's standard trailers (`Co-Authored-By:` + `Claude-Session:`).

---

## Task 1: Shared types + tunables in `config.h`

**Files:**
- Modify: `firmware/ai-usage-esp32/config.h`

- [ ] **Step 1: Add the Wi-Fi store constants + `WifiCred` struct**

Add after the `// ---- Wi-Fi provisioning ----` block:

```c
// ---- multi-network Wi-Fi store (WiFiMulti + TF-card seed) ----
#define MAX_WIFI_APS        6          // max saved networks (home/office/hotspot/…)
#define WIFI_JOIN_TIMEOUT_MS 8000      // WiFiMulti.run() connect timeout per boot
#define SD_CONFIG_PATH      "/pixie.json"   // read-only config at the TF-card root

typedef struct { char ssid[33]; char pass[64]; } WifiCred;
```

- [ ] **Step 2: Compile-sanity (host)** — `config.h` includes only `<stdint.h>`, so it stays host-safe. Verify no accidental Arduino include was added:

Run: `grep -nE '#include' firmware/ai-usage-esp32/config.h`
Expected: only `<stdint.h>` (plus any pre-existing).

- [ ] **Step 3: Commit**

```bash
git add firmware/ai-usage-esp32/config.h
git commit -m "feat(firmware): WifiCred + WiFiMulti store tunables in config.h"
```

---

## Task 2: `wifistore.h` — pure NVS-AP-store logic (TDD on host)

**Files:**
- Create: `firmware/ai-usage-esp32/wifistore.h`
- Test: `design/tools/sdconf_ctest.cpp` (shared harness, created here; extended in Task 4)

The pure functions (`wifistore_parse`, `wifistore_serialize`, `wifistore_merge_into`) contain the dedup/append/overwrite logic that is easy to get wrong. Test them on the host with the real ArduinoJson.

- [ ] **Step 1: Fetch the single-header ArduinoJson for the host test**

The firmware uses ArduinoJson v6.21.5. Grab the amalgamated single header for host compilation (gitignored — not committed):

```bash
mkdir -p design/tools/vendor
curl -sL -o design/tools/vendor/ArduinoJson.h \
  https://github.com/bblanchon/ArduinoJson/releases/download/v6.21.5/ArduinoJson-v6.21.5.h
printf 'design/tools/vendor/\n' >> .gitignore
```

- [ ] **Step 2: Write `wifistore.h` (pure fns + guarded NVS wrappers)**

```c
// NVS store of saved Wi-Fi networks for WiFiMulti. Header-only.
// Pure JSON<->struct + merge are host-testable; Preferences wrappers need Arduino.
#pragma once
#include <string.h>
#include <stdio.h>
#include <ArduinoJson.h>
#include "config.h"
#ifdef ARDUINO
#include <Preferences.h>
#endif

// Parse a JSON array `[{"ssid":"..","pass":".."}]` into out[]. Returns the count.
static int wifistore_parse(const char *json, WifiCred out[], int max) {
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, json)) return 0;
  int n = 0;
  for (JsonObjectConst e : doc.as<JsonArrayConst>()) {
    if (n >= max) break;
    const char *s = e["ssid"] | "";
    if (!s[0]) continue;
    strlcpy(out[n].ssid, s, sizeof(out[n].ssid));
    strlcpy(out[n].pass, e["pass"] | "", sizeof(out[n].pass));
    n++;
  }
  return n;
}

// Serialize list[] into a JSON array string. Returns bytes written (excl. NUL).
static size_t wifistore_serialize(const WifiCred list[], int n, char *out, size_t cap) {
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.createNestedObject();
    o["ssid"] = list[i].ssid;
    o["pass"] = list[i].pass;
  }
  return serializeJson(doc, out, cap);
}

// Merge e into list: update pass if ssid exists, append if new & room, else skip.
// Returns true if the list changed.
static bool wifistore_merge_into(WifiCred list[], int *n, int max, const WifiCred *e) {
  if (!e->ssid[0]) return false;
  for (int i = 0; i < *n; i++) {
    if (strcmp(list[i].ssid, e->ssid) == 0) {
      if (strcmp(list[i].pass, e->pass) == 0) return false;     // unchanged
      strlcpy(list[i].pass, e->pass, sizeof(list[i].pass));
      return true;
    }
  }
  if (*n >= max) return false;                                   // full -> skip (no eviction)
  strlcpy(list[*n].ssid, e->ssid, sizeof(list[*n].ssid));
  strlcpy(list[*n].pass, e->pass, sizeof(list[*n].pass));
  (*n)++;
  return true;
}

#ifdef ARDUINO
// Load the saved AP list from NVS ("aiusage"/"aps"). Returns the count.
static int wifistore_load(WifiCred out[], int max) {
  Preferences p; p.begin("aiusage", true);
  String js = p.getString("aps", "[]");
  p.end();
  return wifistore_parse(js.c_str(), out, max);
}

// Merge one network into NVS (load -> merge -> serialize -> put). No-op if unchanged.
static void wifistore_merge(const WifiCred *e) {
  WifiCred list[MAX_WIFI_APS];
  int n = wifistore_load(list, MAX_WIFI_APS);
  if (!wifistore_merge_into(list, &n, MAX_WIFI_APS, e)) return;
  char buf[1024];
  wifistore_serialize(list, n, buf, sizeof(buf));
  Preferences p; p.begin("aiusage", false);
  p.putString("aps", buf);
  p.end();
}
#endif
```

- [ ] **Step 3: Write the failing host test** (`design/tools/sdconf_ctest.cpp`)

```cpp
// Host test for the pure Wi-Fi store + pixie.json parse logic (no Arduino).
// Build: c++ -std=c++17 -I design/tools/vendor -I firmware/ai-usage-esp32 \
//            design/tools/sdconf_ctest.cpp -o /tmp/sdconf_ctest && /tmp/sdconf_ctest
#include <cstdio>
#include <cstring>
#include <cassert>
#include "wifistore.h"

static int fails = 0;
#define CHECK(c) do { if(!(c)){ printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); fails++; } } while(0)

static void test_parse() {
  WifiCred a[MAX_WIFI_APS];
  int n = wifistore_parse("[{\"ssid\":\"Home\",\"pass\":\"pw1\"},{\"ssid\":\"Office\",\"pass\":\"pw2\"}]", a, MAX_WIFI_APS);
  CHECK(n == 2);
  CHECK(strcmp(a[0].ssid, "Home") == 0);
  CHECK(strcmp(a[1].pass, "pw2") == 0);
  CHECK(wifistore_parse("not json", a, MAX_WIFI_APS) == 0);
  CHECK(wifistore_parse("[]", a, MAX_WIFI_APS) == 0);
  // entry missing ssid is skipped
  CHECK(wifistore_parse("[{\"pass\":\"x\"},{\"ssid\":\"Ok\",\"pass\":\"y\"}]", a, MAX_WIFI_APS) == 1);
}

static void test_merge() {
  WifiCred list[MAX_WIFI_APS]; int n = 0;
  WifiCred e1 = {"Home","pw1"};
  CHECK(wifistore_merge_into(list, &n, MAX_WIFI_APS, &e1) == true);  CHECK(n == 1);
  CHECK(wifistore_merge_into(list, &n, MAX_WIFI_APS, &e1) == false); CHECK(n == 1); // dup, unchanged
  WifiCred e1b = {"Home","NEWpw"};
  CHECK(wifistore_merge_into(list, &n, MAX_WIFI_APS, &e1b) == true); CHECK(n == 1); // pass updated
  CHECK(strcmp(list[0].pass, "NEWpw") == 0);
  WifiCred e2 = {"Office","pw2"};
  CHECK(wifistore_merge_into(list, &n, MAX_WIFI_APS, &e2) == true);  CHECK(n == 2); // appended
  WifiCred blank = {"",""};
  CHECK(wifistore_merge_into(list, &n, MAX_WIFI_APS, &blank) == false);             // empty ssid ignored
}

static void test_cap() {
  WifiCred list[MAX_WIFI_APS]; int n = 0;
  char ssid[16];
  for (int i = 0; i < MAX_WIFI_APS; i++) { snprintf(ssid, sizeof(ssid), "net%d", i);
    WifiCred e; strlcpy(e.ssid, ssid, sizeof(e.ssid)); strcpy(e.pass, "p"); wifistore_merge_into(list,&n,MAX_WIFI_APS,&e); }
  CHECK(n == MAX_WIFI_APS);
  WifiCred over = {"overflow","p"};
  CHECK(wifistore_merge_into(list, &n, MAX_WIFI_APS, &over) == false);               // full -> skip
  CHECK(n == MAX_WIFI_APS);
}

static void test_roundtrip() {
  WifiCred list[2] = {{"Home","pw1"},{"Office","pw2"}};
  char buf[1024]; wifistore_serialize(list, 2, buf, sizeof(buf));
  WifiCred back[MAX_WIFI_APS];
  int n = wifistore_parse(buf, back, MAX_WIFI_APS);
  CHECK(n == 2); CHECK(strcmp(back[1].ssid, "Office") == 0);
}

int main() {
  test_parse(); test_merge(); test_cap(); test_roundtrip();
  // test_sdconf() added in Task 4
  printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
  return fails ? 1 : 0;
}
```

- [ ] **Step 4: Run the test to verify it fails to build/link first, then passes**

Run:
```bash
c++ -std=c++17 -I design/tools/vendor -I firmware/ai-usage-esp32 \
    design/tools/sdconf_ctest.cpp -o /tmp/sdconf_ctest && /tmp/sdconf_ctest
```
Expected: compiles, prints `ALL PASS`, exit 0. (If `wifistore.h` had a bug, a CHECK line prints and it exits 1.)

- [ ] **Step 5: Commit**

```bash
git add firmware/ai-usage-esp32/wifistore.h design/tools/sdconf_ctest.cpp .gitignore
git commit -m "feat(firmware): wifistore.h NVS AP store + host tests (parse/merge/cap)"
```

---

## Task 3: `sdconf.h` — read-only TF-card config parse (TDD on host) + mount (on-device)

**Files:**
- Create: `firmware/ai-usage-esp32/sdconf.h`
- Test: extend `design/tools/sdconf_ctest.cpp`

- [ ] **Step 1: Write `sdconf.h`**

```c
// Read-only TF-card config (/pixie.json). Header-only. NEVER writes to the card.
// sdconf_parse is pure (host-testable); sdconf_load needs the SD BSP (Arduino).
#pragma once
#include <string.h>
#include <stdio.h>
#include <ArduinoJson.h>
#include "config.h"
#ifdef ARDUINO
#include <FS.h>
#include <SD.h>
#include "src/sdcard_bsp/sdcard_bsp.h"
#endif

typedef struct {
  WifiCred wifi[MAX_WIFI_APS]; int wifi_n;
  char token[41];
  char host[24];       // "" => discover via mDNS
  char port[6];        // "" => DEFAULT_BRIDGE_PORT
  bool present;        // valid JSON was parsed
} PixieConfig;

// Parse a pixie.json body into out. Pure; host-testable. Returns out->present.
static bool sdconf_parse(const char *json, PixieConfig *out) {
  memset(out, 0, sizeof(*out));
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, json)) return false;
  int n = 0;
  for (JsonObjectConst e : doc["wifi"].as<JsonArrayConst>()) {
    if (n >= MAX_WIFI_APS) break;
    const char *s = e["ssid"] | "";
    if (!s[0]) continue;
    strlcpy(out->wifi[n].ssid, s, sizeof(out->wifi[n].ssid));
    strlcpy(out->wifi[n].pass, e["pass"] | "", sizeof(out->wifi[n].pass));
    n++;
  }
  out->wifi_n = n;
  strlcpy(out->token, doc["token"] | "", sizeof(out->token));
  strlcpy(out->host,  doc["bridge_host"] | "", sizeof(out->host));
  long p = doc["bridge_port"] | 0;
  if (p > 0 && p < 65536) snprintf(out->port, sizeof(out->port), "%ld", p);
  out->present = true;
  return true;
}

#ifdef ARDUINO
// Mount the TF card (BSP) and read /pixie.json. Returns false if no card/file/bad JSON.
static bool sdconf_load(PixieConfig *out) {
  memset(out, 0, sizeof(*out));
  if (!sdcard_bsp_init()) return false;            // <-- confirm symbol vs 04_SD_Card BSP (Step 3)
  File f = SD.open(SD_CONFIG_PATH, FILE_READ);
  if (!f) return false;
  size_t sz = f.size();
  if (sz == 0 || sz >= 4096) { f.close(); return false; }
  static char buf[4096];
  size_t rd = f.read((uint8_t *)buf, sz);
  f.close();
  buf[rd] = '\0';
  return sdconf_parse(buf, out);
}
#endif
```

- [ ] **Step 2: Add the parse test to the host harness**

In `design/tools/sdconf_ctest.cpp`, add `#include "sdconf.h"` at the top and this function, and call `test_sdconf();` from `main()`:

```cpp
static void test_sdconf() {
  PixieConfig c;
  const char *j = "{\"wifi\":[{\"ssid\":\"Home\",\"pass\":\"pw1\"},{\"ssid\":\"Office\",\"pass\":\"pw2\"}],"
                  "\"token\":\"tok123\",\"bridge_host\":\"\",\"bridge_port\":8787}";
  CHECK(sdconf_parse(j, &c) == true);
  CHECK(c.present); CHECK(c.wifi_n == 2);
  CHECK(strcmp(c.wifi[0].ssid, "Home") == 0);
  CHECK(strcmp(c.token, "tok123") == 0);
  CHECK(c.host[0] == '\0');                 // blank host -> mDNS
  CHECK(strcmp(c.port, "8787") == 0);
  // host override + missing port
  const char *j2 = "{\"wifi\":[],\"token\":\"t\",\"bridge_host\":\"192.168.1.40\"}";
  CHECK(sdconf_parse(j2, &c)); CHECK(c.wifi_n == 0);
  CHECK(strcmp(c.host, "192.168.1.40") == 0); CHECK(c.port[0] == '\0');
  // malformed
  CHECK(sdconf_parse("{bad", &c) == false);
  CHECK(sdconf_parse("", &c) == false);
}
```

- [ ] **Step 3: Confirm the SD BSP symbol before relying on it**

The exact mount API comes from the Waveshare BSP. Copy `Examples/Arduino/04_SD_Card/…/sdcard_bsp.h` + `.cpp` (or `.ino` helpers) into `firmware/ai-usage-esp32/src/sdcard_bsp/`. Open the example's setup and confirm the init function name + whether it uses the `SD` (SPI) or `SD_MMC` object. If it is not `sdcard_bsp_init()` returning bool, adjust `sdconf_load` to match (e.g. `SD.begin(cs)` / `SD_MMC.begin()`). **Do not guess** — read the example.

Run: `grep -rnE 'begin|_init|SD_MMC|SD\.begin' firmware/ai-usage-esp32/src/sdcard_bsp/`
Expected: identifies the real mount call; update `sdconf_load` Step 1 accordingly.

- [ ] **Step 4: Run the host test (parse path only; mount is on-device)**

Run:
```bash
c++ -std=c++17 -I design/tools/vendor -I firmware/ai-usage-esp32 \
    design/tools/sdconf_ctest.cpp -o /tmp/sdconf_ctest && /tmp/sdconf_ctest
```
Expected: `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add firmware/ai-usage-esp32/sdconf.h firmware/ai-usage-esp32/src/sdcard_bsp/ design/tools/sdconf_ctest.cpp
git commit -m "feat(firmware): sdconf.h read-only pixie.json parse + SD mount (04_SD_Card BSP)"
```

---

## Task 4: `net.h` — WiFiMulti + card import + portal fallback

**Files:**
- Modify: `firmware/ai-usage-esp32/net.h`

- [ ] **Step 1: Add includes + the WiFiMulti global**

Near the top of `net.h`, after the existing includes:

```c
#include <WiFiMulti.h>
#include "wifistore.h"
#include "sdconf.h"
```
And after the `g_prefs` globals:
```c
static WiFiMulti g_wifiMulti;
```

- [ ] **Step 2: Add `net_import_card()` (seeds NVS from the card; never logs passwords)**

Insert above `net_begin()`:

```c
// Read the TF-card /pixie.json (if any) and seed the NVS AP store + token/host/port.
// Import-and-forget: after this the card can be removed; the password lives in NVS.
static void net_import_card() {
  PixieConfig cfg;
  if (!sdconf_load(&cfg)) { Serial.println("[sd] no /pixie.json (using NVS)"); return; }
  Serial.printf("[sd] pixie.json: %d wifi, token=%s, host=%s\n",
                cfg.wifi_n, cfg.token[0] ? "set" : "-", cfg.host[0] ? cfg.host : "(mDNS)");
  for (int i = 0; i < cfg.wifi_n; i++) {
    Serial.printf("[sd]  wifi[%d] ssid=%s pass=***\n", i, cfg.wifi[i].ssid);   // never print pass
    wifistore_merge(&cfg.wifi[i]);
  }
  Preferences p; p.begin("aiusage", false);
  if (cfg.token[0]) p.putString("token", cfg.token);
  if (cfg.host[0])  p.putString("host", cfg.host);
  if (cfg.port[0])  p.putString("port", cfg.port);
  p.putBool("prov", true);
  p.end();
}

// Capture the creds the portal just used into the AP store (so WiFiMulti joins them next
// boot) and add them to the live WiFiMulti. Call right after startConfigPortal returns.
static void net_capture_portal(WiFiManager &wm) {
  WifiCred e;
  strlcpy(e.ssid, wm.getWiFiSSID().c_str(), sizeof(e.ssid));
  strlcpy(e.pass, wm.getWiFiPass().c_str(), sizeof(e.pass));
  if (e.ssid[0]) { wifistore_merge(&e); g_wifiMulti.addAP(e.ssid, e.pass); }
}
```

- [ ] **Step 3: Rewrite `net_begin()`**

Replace the whole `net_begin()` body with:

```c
static void net_begin() {
  // 1. saved bridge config
  g_prefs.begin("aiusage", true);
  g_host  = g_prefs.getString("host", "");
  g_port  = g_prefs.getString("port", String(DEFAULT_BRIDGE_PORT));
  g_token = g_prefs.getString("token", "");
  g_prefs.end();

  // 2. seed from the TF card (may set host/port/token + AP store), then re-read
  net_import_card();
  g_prefs.begin("aiusage", true);
  g_host  = g_prefs.getString("host", g_host);
  g_port  = g_prefs.getString("port", g_port);
  g_token = g_prefs.getString("token", g_token);
  bool provisioned = g_prefs.getBool("prov", false) || g_host.length() > 0;
  g_prefs.end();

  // 3. build WiFiMulti from the NVS AP store
  WifiCred aps[MAX_WIFI_APS];
  int naps = wifistore_load(aps, MAX_WIFI_APS);
  for (int i = 0; i < naps; i++) g_wifiMulti.addAP(aps[i].ssid, aps[i].pass);
  if (naps > 0) provisioned = true;

  // 4. connect to the strongest reachable saved network
  if (naps > 0) {
    Serial.printf("[net] WiFiMulti: %d saved AP(s), joining strongest...\n", naps);
    g_wifiMulti.run(WIFI_JOIN_TIMEOUT_MS);
  }

  // 5. captive portal fallback only on first-ever run with nothing to join.
  //    If provisioned but nothing joined (carried elsewhere): do NOT block — USB drives
  //    the display; tapping LIVE reopens the portal on demand.
  if (WiFi.status() != WL_CONNECTED && !provisioned) {
    WiFiManager wm;
    wm.setConfigPortalTimeout(WM_AP_TIMEOUT_S);
    static WiFiManagerParameter pHost("host", "Mac bridge IP (blank = auto-find via mDNS)", g_host.c_str(), 24);
    static WiFiManagerParameter pPort("port", "Bridge port", g_port.c_str(), 6);
    static WiFiManagerParameter pTok("token", "Pairing token (from the Mac bridge)", g_token.c_str(), 40);
    g_pHost = &pHost; g_pPort = &pPort; g_pTok = &pTok;
    wm.addParameter(&pHost); wm.addParameter(&pPort); wm.addParameter(&pTok);
    wm.setSaveParamsCallback(net_save_params);
    bool ok = false;
    while (!ok) {
      wm.startConfigPortal(WM_AP_NAME);
      net_capture_portal(wm);
      g_prefs.begin("aiusage", true); ok = g_prefs.getBool("prov", false); g_prefs.end();
      if (ok && WiFi.status() != WL_CONNECTED) g_wifiMulti.run(WIFI_JOIN_TIMEOUT_MS);
    }
    // refresh host/port the portal may have set
    g_prefs.begin("aiusage", true);
    g_host = g_prefs.getString("host", g_host);
    g_port = g_prefs.getString("port", g_port);
    g_token = g_prefs.getString("token", g_token);
    g_prefs.end();
  }

  snprintf(g_bridge_desc, sizeof(g_bridge_desc), "%s:%s",
           g_host.length() ? g_host.c_str() : "(mDNS)", g_port.c_str());
}
```

- [ ] **Step 4: Capture creds in the on-demand portal too**

In `net_portal()`, after `wm.startConfigPortal(WM_AP_NAME);` add:

```c
  net_capture_portal(wm);
```

- [ ] **Step 5: Compile the firmware** (host unit tests already green; this is the toolchain compile)

Reassemble the sketch per the "COMPILE-VERIFIED GREEN" recipe in project memory (Waveshare display BSP + `08_Audio_Test` codec `src/` **+ the new `04_SD_Card` `src/sdcard_bsp/`** + repo firmware over it), then:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB" \
  --libraries <old>/arduino/libraries <sketch-dir>
```
Expected: 0 errors. Note the flash/RAM %. If `WiFiMulti.h` is missing, it ships with the ESP32 core (`#include <WiFiMulti.h>`, class `WiFiMulti`).

- [ ] **Step 6: Commit**

```bash
git add firmware/ai-usage-esp32/net.h
git commit -m "feat(firmware): WiFiMulti auto-join + TF-card import + portal capture (net.h)"
```

---

## Task 5: `.ino` serial banner + example config + docs

**Files:**
- Modify: `firmware/ai-usage-esp32/ai-usage-esp32.ino`
- Create: `firmware/pixie.example.json`
- Modify: `README.md`, `firmware/README.md`

- [ ] **Step 1: Serial banner (setup() already calls `net_begin()`)**

`net_import_card()` already logs the card contents, and `net_begin()` logs the WiFiMulti join, so setup() needs no structural change. Confirm `net_begin()` is still called after the display + `ui_setup_screen()` are up (so the panel shows guidance during any portal). No code change unless the order is wrong.

> **Deviation from spec §4.5:** the "importing config…" indicator is **serial-only**, not an on-panel line — detecting the card happens inside `net_begin()`, and mutating the LVGL setup screen mid-boot from the net path is fragile for little gain. The existing `ui_setup_screen()` still shows during the portal.

Run: `grep -n 'net_begin\|ui_setup_screen' firmware/ai-usage-esp32/ai-usage-esp32.ino`
Expected: `ui_setup_screen()` (or equivalent) precedes `net_begin()` in `setup()`.

- [ ] **Step 2: Write `firmware/pixie.example.json`**

```json
{
  "wifi": [
    { "ssid": "YourHomeWiFi", "pass": "home-password" },
    { "ssid": "YourOfficeWiFi", "pass": "office-password" },
    { "ssid": "YourPhoneHotspot", "pass": "hotspot-password" }
  ],
  "token": "paste-the-pairing-token-from-the-bridge",
  "bridge_host": "",
  "bridge_port": 8787
}
```

- [ ] **Step 3: Add the Security + TF-card setup section to `README.md`**

Add a `## Security & Wi-Fi setup (read before giving it away)` section containing:

```markdown
### Where your Wi-Fi password is stored
Pixie stores saved Wi-Fi networks in the ESP32's on-board flash (NVS). NVS is **not
encrypted** — someone with physical access and a flash dumper could read it — but it is
soldered to the board, so it is the higher bar. Treat the device like any other gadget
that remembers your Wi-Fi.

### The TF card (`pixie.json`) is plaintext — remove it after first boot
You can pre-load Wi-Fi + the pairing token onto a TF card as `/pixie.json` (see
`firmware/pixie.example.json`). This is convenient (edit it on any PC, survives a reflash),
**but the file is plaintext and readable on any computer.** On first boot Pixie imports the
config into NVS ("import-and-forget"), so you can **remove the card** and the password no
longer travels on removable media. Keep the card inserted only if you want the config to
survive a firmware reflash — accept that anyone who takes the card can read it.

Precedence: a card (when present) wins over portal/NVS values at boot. If you reconfigure
via the setup portal while a card is inserted, update `pixie.json` (or remove the card) or
the card values return on the next boot. Pixie **never writes** to the card.

### The pairing token is a secret
The token gates the Remote and Voice features. Treat it like a password; if it leaks,
rotate it at the bridge.

### For a giveaway unit
Use a **guest / dedicated Wi-Fi**, not your primary network. Hand it over with the card
removed (config already in NVS).

### Multi-network (home ↔ office)
List several networks in `wifi[]`; Pixie joins the strongest one in range automatically —
no re-typing when you move. Leave `bridge_host` blank to auto-find the Mac via mDNS.
```

- [ ] **Step 4: Add the `04_SD_Card` BSP + `WiFiMulti` notes to `firmware/README.md`**

In the firmware assembly steps (next to the `lv_demo_widgets` gotcha), add:

```markdown
- Copy the Waveshare `Examples/Arduino/04_SD_Card` driver (`sdcard_bsp.h/.cpp`) into
  `firmware/ai-usage-esp32/src/sdcard_bsp/` — required for reading `/pixie.json`.
- `WiFiMulti.h` ships with the ESP32 core (no extra library).
- Optional `/pixie.json` on the TF card seeds Wi-Fi + token; see the Security section in
  the top-level README.
```

- [ ] **Step 5: Commit**

```bash
git add firmware/ai-usage-esp32/ai-usage-esp32.ino firmware/pixie.example.json README.md firmware/README.md
git commit -m "docs(seamless): pixie.example.json + Security/TF-card + WiFiMulti setup notes"
```

---

## Task 6: Full compile-verify + push

- [ ] **Step 1: Clean compile of the assembled sketch**

Run the Task 4 Step 5 `arduino-cli compile` again on the final tree. Expected: 0 errors; record flash/RAM %.

- [ ] **Step 2: Run the host tests once more**

Run:
```bash
c++ -std=c++17 -I design/tools/vendor -I firmware/ai-usage-esp32 \
    design/tools/sdconf_ctest.cpp -o /tmp/sdconf_ctest && /tmp/sdconf_ctest
```
Expected: `ALL PASS`.

- [ ] **Step 3: Push the branch + update draft PR #13 (or open the seamless PR)**

```bash
git push -u origin feat/seamless-firmware
```
Update the PR body to note it now supersedes #13 (mDNS + WiFiMulti + TF-card + security docs).

---

## Task 7: On-device verification (WITH the user — do NOT flash unattended)

> Flash once; keep the current known-good firmware recoverable. Bridge must be running for
> the live-data checks. Run on a **flat home LAN** (mDNS needs link-local).

- [ ] **① SD read:** insert a card with `pixie.json` (home + a fake office SSID) → boot →
  serial shows `[sd] pixie.json: … wifi …` and each `ssid=` (pass must show `***`).
- [ ] **⚠️ Backlight/power check:** confirm the panel backlight stays on after SD mount —
  the display backlight is on **GPIO8**; verify the `04_SD_Card` BSP does not drive the same
  line / power rail (likely gated via the TCA9554 expander). If the screen goes dark on
  boot, this is the culprit — see Risks.
- [ ] **② WiFiMulti auto-join:** device joins **home** automatically (office SSID absent) →
  `[net] WiFiMulti: N saved AP(s)…` then connected.
- [ ] **③ mDNS:** `bridge_host` blank → `[mdns] found bridge at …` → dashboard shows live data
  with no typed IP.
- [ ] **④ Import-and-forget:** power off → **remove the card** → boot → still joins home
  (NVS persisted); serial shows `[sd] no /pixie.json (using NVS)`.
- [ ] **⑤ Edit-on-PC:** change the home password in `pixie.json`, reinsert → boot →
  `wifistore_merge` updates NVS → joins with the new password.
- [ ] **⑥ Portal fallback:** erase NVS (fresh flash w/ erase) + no card → captive portal opens
  → save home Wi-Fi → joins → reboot → auto-joins (no re-type).
- [ ] **⑦ Regression:** dashboard fine over USB + Wi-Fi; USB frames still drive the display;
  Remote (screens ②③) still work; screen-① LIVE tap still reopens the portal.

---

## Risks / notes

- **GPIO8 conflict (highest risk):** display backlight `BL8` vs the SD `04_SD_Card`
  power-enable both reference GPIO8 in notes. They likely differ (SD enable is probably a
  TCA9554 expander line), but **verify on-device** — a dark screen after adding SD points
  here. Mitigation: read the BSP's real pin map before wiring `sdconf_load`.
- **Memory/PSRAM:** SD + WiFiMulti + mDNS + LVGL + Wi-Fi all resident; watch the RAM % in
  the compile output and free heap at boot. (Audio/voice is a separate branch — not loaded
  here.)
- **WiFiMulti scan latency:** `run()` blocks up to `WIFI_JOIN_TIMEOUT_MS` at boot; acceptable.
- **Card override permanence:** documented in the README precedence note (card > portal).
- **Office enterprise/5GHz:** still won't join; USB + USB-actions remain the office answer
  (unchanged).
- **ArduinoJson host header** is gitignored; the test's build comment documents the one-line
  `curl`. CI (bridge-tests) does not run this C++ harness — it is a local dev check.
```