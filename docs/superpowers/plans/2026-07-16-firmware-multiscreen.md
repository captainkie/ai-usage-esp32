# Firmware: Multi-Screen + Mac Monitor/Remote — Implementation Plan

> **For agentic workers:** this is Phase 2 (firmware). Unlike the bridge plan, it is **not TDD** — there is no host test harness for the LVGL/Arduino UI, and no board on hand yet. Implement carefully against the existing patterns and the visual contract in `design/mockup.html`; **verification is on-hardware** (Task 6 checklist) when the AliExpress board arrives. Where possible, keep pure logic testable and compile-check with `arduino-cli` if a toolchain + the Waveshare `09_LVGL_V8_Test` BSP are available.

**Goal:** Turn the single AI-Usage screen into three swipe-navigated LVGL tiles — ① AI Usage (unchanged), ② Mac Monitor, ③ Mac Remote — fed by the bridge's new `system` block and driving it via `POST /action`.

**Architecture:** Replace the monolithic `ui_build()` with an `lv_tileview` holding three tiles; one builder per tile. The render timer updates the visible tile. Remote button taps must NOT block LVGL, so they enqueue an action string that the networking task (`loop()`) POSTs to the bridge. Wi-Fi captive portal gains a pairing-token field.

**Tech Stack:** Arduino (ESP32-S3) + LVGL v8, `Arduino_GFX`/Waveshare BSP (unchanged), `ArduinoJson` v6, `WiFiManager`, `Preferences`, `HTTPClient`.

---

## Reference (read these first)
- `firmware/ai-usage-esp32/ai-usage-esp32.ino` — current UI (`ui_build`, `render_cb`, `loop`), the LVGL-lock pattern, and the shared `g_state`/`g_mux`.
- `firmware/ai-usage-esp32/config.h` — data model (`UsageState`, `ProviderState`, `Window`), palette, `util_color()`.
- `firmware/ai-usage-esp32/net.h` — `net_begin`, `net_fetch`, WiFiManager params.
- `bridge/ai-usage-bridge.mjs` + `bridge/lib/system.mjs`/`actions.mjs` — the exact `system` JSON shape and the `/action` contract (allowlist + `token`).
- `design/mockup.html` — pixel-accurate layout of all three screens (the visual contract). Coordinates there are in the same 640×172 space.

## File Structure (all in `firmware/ai-usage-esp32/`)
- Modify `config.h` — add `SystemState` + remote action enums/labels.
- Modify `net.h` — parse `system`; add `net_action()`; store/read pairing token; add portal field.
- Modify `ai-usage-esp32.ino` — tileview + three tile builders + render dispatch + action queue + page dots.

Keep the mascot engine (`mascot.c/.h`) untouched.

---

### Task 1: `config.h` — system + remote data model

Add after the `UsageState` typedef:

```c
// ---- Mac system stats (mirrors the bridge `system` block; -1/empty = no data) ----
typedef struct { char name[20]; int cpu; } TopProc;
typedef struct {
  bool ok;                 // system block present this fetch
  int  cpu;                // -1 = n/a
  int  mem_util;   float mem_used_gb, mem_total_gb;
  int  disk_util;  int   disk_used_gb, disk_total_gb;
  int  net_down_kbps, net_up_kbps;
  int  batt;               // -1 = no battery
  int  temp_c;             // -1 = n/a
  TopProc top[3]; int top_n;
} SystemState;
```

Add `SystemState sys;` as a field of `UsageState` (so one fetch fills both providers and system, guarded by the same `g_mux`).

Add remote-action identifiers + the JSON bodies they POST (allowlist mirrors the bridge):

```c
// Each remote button maps to a fixed JSON body sent to POST /action.
// The token is injected by net_action(); do not put it here.
enum { ACT_YT, ACT_MUSIC, ACT_SAFARI, ACT_FOCUS,
       ACT_PREV, ACT_PLAYPAUSE, ACT_NEXT,
       ACT_VOL_DN, ACT_VOL_MUTE, ACT_VOL_UP,
       ACT_LOCK, ACT_SLEEP, ACT_COUNT };
static const char *ACTION_BODY[ACT_COUNT] = {
  "{\"action\":\"open_url\",\"url\":\"https://youtube.com\"}",
  "{\"action\":\"open_app\",\"name\":\"Music\"}",
  "{\"action\":\"open_app\",\"name\":\"Safari\"}",
  "{\"action\":\"shortcut\",\"name\":\"Focus\"}",
  "{\"action\":\"media\",\"key\":\"prev\"}",
  "{\"action\":\"media\",\"key\":\"playpause\"}",
  "{\"action\":\"media\",\"key\":\"next\"}",
  "{\"action\":\"volume\",\"dir\":\"down\"}",
  "{\"action\":\"volume\",\"dir\":\"mute\"}",
  "{\"action\":\"volume\",\"dir\":\"up\"}",
  "{\"action\":\"lock\"}",
  "{\"action\":\"display_sleep\"}",
};
```

> Note: `open_app`/`shortcut` names must exist in the Mac's `bridge/actions.json`. Document that `Music`/`Safari`/`Focus` are the defaults from `actions.example.json`; a user who renames them there must match here (future: fetch the list from the bridge).

---

### Task 2: `net.h` — parse system, send actions, pairing token

**2a. Pairing token storage** — alongside `g_host`/`g_port`:
```c
static String g_token = "";
```
In `net_begin()` read it (`g_token = g_prefs.getString("token", "");`) and in `net_save_params()` persist it. Add a portal field:
```c
static WiFiManagerParameter pTok("token", "Pairing token (from the Mac bridge)", g_token.c_str(), 40);
```
add it to `wm` and read it in `net_save_params()` (`if (g_pTok) g_token = g_pTok->getValue();`). Mirror the same field in `net_portal()`.

**2b. Parse the `system` block** inside `net_fetch()`, after the providers loop, before `out->ok = true;`:
```c
JsonObjectConst sysj = doc["system"];
SystemState *s = &out->sys;
memset(s, 0, sizeof(*s));
if (!sysj.isNull()) {
  s->ok = true;
  s->cpu       = sysj["cpu"]["util"] | -1;
  s->mem_util  = sysj["mem"]["util"] | -1;
  s->mem_used_gb  = sysj["mem"]["used_gb"]  | 0.0f;
  s->mem_total_gb = sysj["mem"]["total_gb"] | 0.0f;
  s->disk_util = sysj["disk"]["util"] | -1;
  s->disk_used_gb  = sysj["disk"]["used_gb"]  | 0;
  s->disk_total_gb = sysj["disk"]["total_gb"] | 0;
  s->net_down_kbps = sysj["net"]["down_kbps"] | 0;
  s->net_up_kbps   = sysj["net"]["up_kbps"]   | 0;
  s->batt   = sysj["battery"].isNull() ? -1 : (sysj["battery"]["percent"] | -1);
  s->temp_c = sysj["temp_c"] | -1;
  int i = 0;
  for (JsonObjectConst t : sysj["top"].as<JsonArrayConst>()) {
    if (i >= 3) break;
    strlcpy(s->top[i].name, t["name"] | "", sizeof(s->top[i].name));
    s->top[i].cpu = t["cpu"] | 0;
    i++;
  }
  s->top_n = i;
}
```
Bump the `DynamicJsonDocument` size (system + top adds fields): use `16384`.

**2c. `net_action()`** — POST to `/action` with the token injected:
```c
// Sends one allowlisted action. `body` is one of ACTION_BODY[]. Returns true on {ok:true}.
static bool net_action(const char *body) {
  if (WiFi.status() != WL_CONNECTED || g_host.length() == 0) return false;
  if (g_token.length() == 0) return false;                      // not paired
  // splice the token into the body: {"token":"...",<body without leading '{'>
  String payload = String("{\"token\":\"") + g_token + "\"," + (body + 1);
  String url = "http://" + g_host + ":" + g_port + "/action";
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  http.end();
  return code == 200;
}
```

---

### Task 3: `ai-usage-esp32.ino` — tileview, tiles, dispatch, action queue

**3a. Screen state + action queue** (near the other statics):
```c
static lv_obj_t *g_tv;                       // tileview
static lv_obj_t *tile[3];                     // usage / monitor / remote
static lv_obj_t *dot[3];                      // page dots
// remote action queue (LVGL event -> net task; never POST inside LVGL)
static volatile int g_action = -1;            // ACT_* or -1
```
An LVGL button callback only does `g_action = (int)(intptr_t)user_data;` (plus a toast). `loop()` drains it (Task 3e).

**3b. Refactor `ui_build()`** to build the tileview and delegate:
```c
static void ui_build() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, LVC(COL_BG), 0);

  g_tv = lv_tileview_create(scr);
  lv_obj_set_size(g_tv, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_opa(g_tv, LV_OPA_TRANSP, 0);
  lv_obj_add_event_cb(g_tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  tile[0] = lv_tileview_add_tile(g_tv, 0, 0, LV_DIR_HOR);
  tile[1] = lv_tileview_add_tile(g_tv, 1, 0, LV_DIR_HOR);
  tile[2] = lv_tileview_add_tile(g_tv, 2, 0, LV_DIR_HOR);
  screen_usage_build(tile[0]);
  screen_monitor_build(tile[1]);
  screen_remote_build(tile[2]);
  build_dots(scr);                     // 3 dots overlaid bottom-centre, above tiles
}
```
Move ALL current `ui_build` body (topbar, gauges, labels, mascot, pills) into `screen_usage_build(lv_obj_t *parent)`, changing every `lv_*_create(scr)` to `lv_*_create(parent)` and keeping positions identical (tile is full-screen, same coord space). Keep the existing pill/mascot event callbacks.

**3c. `screen_monitor_build(parent)`** — mirror the mockup's page ②:
- Topbar: brand "AI·USAGE" (left), centered "MAC · MONITOR", "LIVE" (right) — reuse the usage topbar styling.
- Three `lv_arc` gauges at cx = {110, 320, 530}, cy = 84, R = 36, same style as the usage arcs (270° meter, rounded indicator, `COL_TRACK`). Store handles `arcCpu/arcRam/arcDisk` + big value labels (`lv_font_montserrat_40`) + captions (`lv_font_montserrat_14`) + a bottom ticker label.
- No mascot here.

**3d. `screen_remote_build(parent)`** — mirror page ③:
- Topbar (brand / "MAC · REMOTE" / LIVE).
- Shortcut row: 4 `lv_btn` (YouTube/Music/Safari/Focus) → user_data = ACT_YT/…; small labels.
- Controls row: media (⏮⏯⏭ → ACT_PREV/PLAYPAUSE/NEXT), volume (−/mute/+ → ACT_VOL_*), system (🔒/💤 → ACT_LOCK/ACT_SLEEP). Use text glyphs; touch targets ≥ ~36px.
- A hidden toast label (top layer) shown ~1.5 s on tap.
- One shared callback:
```c
static void remote_cb(lv_event_t *e) {
  int a = (int)(intptr_t)lv_event_get_user_data(e);
  g_action = a;                        // net task will POST it
  show_toast(a);                       // brief on-screen confirmation
}
```

**3e. Render dispatch + action drain**
- In `render_cb`, keep the usage-tile updates, and additionally update the monitor tile from `st.sys` (arc values via `lv_arc_set_value`, colours via `util_color`, captions, ticker). Guard when `!st.sys.ok` (show "—"). The remote tile is static (no per-frame update) except the toast fade.
- In `loop()`, after the poll block, drain the queue OUTSIDE the LVGL lock:
```c
if (g_action >= 0) {
  int a = g_action; g_action = -1;
  bool ok = net_action(ACTION_BODY[a]);
  if (!ok) Serial.printf("[action] %d failed\n", a);
}
```

**3f. Page dots** — `build_dots()` creates three 6px dots centered at the bottom; `tile_changed_cb` reads the active tile index (`lv_obj_get_scroll_x(g_tv) / SCREEN_W` rounded, or track via the event) and restyles the active dot brighter. Keep it simple; transient fade is optional.

---

### Task 4: Captive-portal pairing field (done in Task 2a) — verify wiring
Confirm the token field appears in both `net_begin()`'s first-run portal and `net_portal()`, is saved to `Preferences`, and that `net_action()` refuses to send when empty. Update `firmware/README.md` to mention entering the pairing token (printed by the bridge) during setup.

---

### Task 5: Docs
- Update `firmware/README.md`: three screens + swipe; the pairing-token setup step; that Monitor/Remote need the bridge's `system`/`/action` (bridge ≥ the Phase-1 build).

---

### Task 6: On-hardware verification checklist (when the board arrives)
Do NOT mark the feature done until these pass on the real device:
- [ ] Builds in Arduino IDE on top of `09_LVGL_V8_Test` (fonts montserrat 14/20/40 enabled in `lv_conf.h`).
- [ ] Boots to ① AI Usage identical to the shipped version (twin gauges, mascot, pills, countdown).
- [ ] Swipe left/right moves ①→②→③ smoothly; page dots track; per-screen taps still work (pills/mascot on ①).
- [ ] ② Monitor shows live CPU/RAM/Disk with correct traffic-light colours + net/batt/temp/top from the bridge; degrades to "—" when the bridge has no `system`.
- [ ] ③ Remote: tapping YouTube opens it on the Mac; media/volume work; 🔒/💤 work; **all fail safely with no pairing token** and show the toast.
- [ ] Wi-Fi portal accepts + persists the pairing token.

Only after all six: open the PR and request review.

## Self-Review
- **Spec coverage:** §4 nav (tileview+dots) → Task 3a/3b/3f. §5 Monitor → Task 3c + render. §6 Remote → Task 3d + action queue. §7.1 system parse → Task 2b. §7.2/§7.3 action + token → Task 2a/2c + config actions. Firmware structure §11 → Tasks 1–3.
- **No-block rule:** remote taps enqueue; only `loop()` does HTTP — no HTTP inside an LVGL callback.
- **Consistency:** `SystemState` fields used in render (Task 3e) match Task 1 + the parse in Task 2b; `ACTION_BODY[]` indices match the `ACT_*` enum and `net_action` splicing.
