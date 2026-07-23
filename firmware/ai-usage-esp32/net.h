// Wi-Fi provisioning (captive portal) + polling the Mac bridge.
// Header-only: included once into the sketch translation unit.
#pragma once
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>     // tzapu/WiFiManager
#include <ESPmDNS.h>         // auto-discover the Mac bridge (_aiusage._tcp)
#include <Preferences.h>
#include <ArduinoJson.h>     // bblanchon/ArduinoJson v6
#include <WiFiMulti.h>       // auto-join the strongest of several saved networks
#include "config.h"
#include "wifistore.h"       // NVS AP store (seeds WiFiMulti)
#include "sdconf.h"          // read-only TF-card /pixie.json

static Preferences        g_prefs;
static String             g_host = "";
static String             g_port = String(DEFAULT_BRIDGE_PORT);
static String             g_token = "";        // pairing token (from the Mac bridge)
static char               g_bridge_desc[48] = "(not set)";
static WiFiManagerParameter *g_pHost = nullptr;
static WiFiManagerParameter *g_pPort = nullptr;
static WiFiManagerParameter *g_pTok  = nullptr;
static WiFiMulti             g_wifiMulti;

static void net_save_params() {
  if (g_pHost) g_host  = g_pHost->getValue();
  if (g_pPort) g_port  = g_pPort->getValue();
  if (g_pTok)  g_token = g_pTok->getValue();
  g_host.trim(); g_port.trim(); g_token.trim();
  if (g_port.length() == 0) g_port = String(DEFAULT_BRIDGE_PORT);
  g_prefs.begin("aiusage", false);
  g_prefs.putString("host", g_host);
  g_prefs.putString("port", g_port);
  g_prefs.putString("token", g_token);
  g_prefs.putBool("prov", true);   // provisioned at least once (host may be blank -> mDNS)
  g_prefs.end();
  snprintf(g_bridge_desc, sizeof(g_bridge_desc), "%s:%s",
           g_host.length() ? g_host.c_str() : "(mDNS)", g_port.c_str());
}

// Read the TF-card /pixie.json (if any) and seed the NVS AP store + token/host/port.
// Import-and-forget: after this the card can be removed; the password lives in NVS.
static void net_import_card() {
  static PixieConfig cfg;   // static: large struct, keep it off the boot-path stack
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

// Capture the creds the portal just used into the AP store (so WiFiMulti joins them on the
// next boot) and add them to the live WiFiMulti. Call right after startConfigPortal returns.
static void net_capture_portal(WiFiManager &wm) {
  WifiCred e;
  strlcpy(e.ssid, wm.getWiFiSSID().c_str(), sizeof(e.ssid));
  strlcpy(e.pass, wm.getWiFiPass().c_str(), sizeof(e.pass));
  if (e.ssid[0]) { wifistore_merge(&e); g_wifiMulti.addAP(e.ssid, e.pass); }
}

// Bring up Wi-Fi. Seeds the multi-network store from the TF card, joins the strongest
// saved network via WiFiMulti, and opens the captive portal only on the very first run
// when nothing can be joined. mDNS (net_discover) resolves the Mac after connecting.
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
  static WifiCred aps[MAX_WIFI_APS];   // static: keep it off the boot-path stack
  int naps = wifistore_load(aps, MAX_WIFI_APS);
  for (int i = 0; i < naps; i++) g_wifiMulti.addAP(aps[i].ssid, aps[i].pass);
  if (naps > 0) provisioned = true;

  // 4. connect to the strongest reachable saved network
  if (naps > 0) {
    Serial.printf("[net] WiFiMulti: %d saved AP(s), joining strongest...\n", naps);
    g_wifiMulti.run(WIFI_JOIN_TIMEOUT_MS);
  }

  // 5. captive-portal fallback only on the first-ever run with nothing to join. If
  //    provisioned but nothing joined (carried elsewhere), do NOT block — USB-serial drives
  //    the display and tapping the LIVE indicator reopens the portal on demand.
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
    g_prefs.begin("aiusage", true);
    g_host  = g_prefs.getString("host", g_host);
    g_port  = g_prefs.getString("port", g_port);
    g_token = g_prefs.getString("token", g_token);
    g_prefs.end();
  }

  snprintf(g_bridge_desc, sizeof(g_bridge_desc), "%s:%s",
           g_host.length() ? g_host.c_str() : "(mDNS)", g_port.c_str());
}

// Re-open the portal on demand (e.g. long-press a side button).
static void net_portal() {
  WiFiManager wm;
  static WiFiManagerParameter pHost("host", "Mac bridge IP", g_host.c_str(), 24);
  static WiFiManagerParameter pPort("port", "Bridge port", g_port.c_str(), 6);
  static WiFiManagerParameter pTok("token", "Pairing token (from the Mac bridge)", g_token.c_str(), 40);
  g_pHost = &pHost; g_pPort = &pPort; g_pTok = &pTok;
  wm.addParameter(&pHost); wm.addParameter(&pPort); wm.addParameter(&pTok);
  wm.setSaveParamsCallback(net_save_params);
  wm.startConfigPortal(WM_AP_NAME);
  net_capture_portal(wm);   // save the chosen network into the WiFiMulti store
}

static const char *net_bridge_desc() { return g_bridge_desc; }

// Set true to request one voice turn (from a "@VOICE" serial line or a touch);
// drained in loop() which calls voice_ask(). Declared here so net_usb_read can set it.
static volatile bool g_voice_trigger = false;

// Voice provider (screen ④ chip): the active provider name + a request to cycle it.
static char g_voice_provider[24] = "Claude";
static volatile bool g_provider_cycle = false;

// GET /voice/providers -> copy the active provider's display name into g_voice_provider.
static void net_provider_refresh() {
  if (WiFi.status() != WL_CONNECTED || g_host.length() == 0) return;
  HTTPClient http;
  String url = "http://" + g_host + ":" + g_port + "/voice/providers";
  http.setConnectTimeout(HTTP_TIMEOUT_MS); http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return;
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, http.getString())) {
      const char *act = doc["active"] | "";
      for (JsonObjectConst p : doc["providers"].as<JsonArrayConst>())
        if (strcmp(p["id"] | "", act) == 0) { strlcpy(g_voice_provider, p["name"] | "Claude", sizeof(g_voice_provider)); break; }
    }
  }
  http.end();
}

// Cycle to the next provider: read the list, POST the next id, refresh the name.
static void net_provider_cycle() {
  if (WiFi.status() != WL_CONNECTED || g_host.length() == 0 || g_token.length() == 0) return;
  HTTPClient http;
  String url = "http://" + g_host + ":" + g_port + "/voice/providers";
  http.setConnectTimeout(HTTP_TIMEOUT_MS); http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return;
  String nextId;
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    if (!deserializeJson(doc, http.getString())) {
      const char *act = doc["active"] | "";
      JsonArrayConst arr = doc["providers"].as<JsonArrayConst>();
      int n = arr.size(), cur = 0, i = 0;
      for (JsonObjectConst p : arr) { if (strcmp(p["id"] | "", act) == 0) cur = i; i++; }
      i = 0;
      for (JsonObjectConst p : arr) { if (i == (cur + 1) % n) { nextId = String((const char *)(p["id"] | "")); } i++; }
    }
  }
  http.end();
  if (nextId.length()) {
    HTTPClient h2;
    if (h2.begin("http://" + g_host + ":" + g_port + "/voice/provider")) {
      h2.addHeader("Content-Type", "application/json");
      h2.addHeader("X-Pixie-Token", g_token);
      h2.POST(String("{\"id\":\"") + nextId + "\"}");
      h2.end();
    }
    net_provider_refresh();
  }
}

static void parse_window(JsonVariantConst w, Window *dst) {
  if (w.isNull()) { dst->util = -1; dst->reset_in = -1; return; }
  dst->util     = w["util"]     | -1;
  dst->reset_in = w["reset_in"] | (long)-1;
}

// Parse a /usage JSON body into `out` (caller zeroes `out`). Returns true on
// success. Shared by the Wi-Fi HTTP path and the USB-serial push path.
static bool parse_usage_body(const char *body, UsageState *out) {
  // 16384: providers + the `system` block (cpu/mem/disk/net/battery + top[3]).
  DynamicJsonDocument doc(16384);
  if (deserializeJson(doc, body)) { strcpy(out->err, "json"); return false; }

  JsonObjectConst provs = doc["providers"];
  const char *keys[PV_COUNT] = { "claude", "codex", "gemini" };
  for (int i = 0; i < PV_COUNT; i++) {
    JsonObjectConst pr = provs[keys[i]];
    ProviderState *ps = &out->p[i];
    ps->linked = pr["linked"] | false;
    ps->stale  = pr["stale"]  | false;
    strlcpy(ps->model,  pr["model"]  | "", sizeof(ps->model));
    strlcpy(ps->effort, pr["effort"] | "", sizeof(ps->effort));
    parse_window(pr["five_hour"], &ps->five);
    parse_window(pr["seven_day"], &ps->seven);
  }

  // ---- Mac system stats (screen 2). Absent block => sys.ok stays false. ----
  JsonObjectConst sysj = doc["system"];
  SystemState *s = &out->sys;
  memset(s, 0, sizeof(*s));
  if (!sysj.isNull()) {
    s->ok = true;
    s->cpu          = sysj["cpu"]["util"] | -1;
    s->mem_util     = sysj["mem"]["util"] | -1;
    s->mem_used_gb  = sysj["mem"]["used_gb"]  | 0.0f;
    s->mem_total_gb = sysj["mem"]["total_gb"] | 0.0f;
    s->disk_util    = sysj["disk"]["util"] | -1;
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

  out->ok = true;
  out->stamp_ms = millis();
  out->err[0] = 0;
  return true;
}

// Auto-discover the Mac bridge on the LAN via mDNS (the bridge advertises
// `_aiusage._tcp`). Updates g_host/g_port on success. Lets the device find the Mac
// with no typed IP, and recover automatically when the Mac's IP changes (e.g.
// carried between home and office). Returns true if a bridge was found.
static bool g_mdns_up = false;
static bool net_discover() {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!g_mdns_up) { g_mdns_up = MDNS.begin("pixie"); if (!g_mdns_up) return false; }
  int n = MDNS.queryService("aiusage", "tcp");   // -> _aiusage._tcp
  if (n <= 0) return false;
  g_host = MDNS.address(0).toString();
  uint16_t p = MDNS.port(0);
  if (p) g_port = String(p);
  snprintf(g_bridge_desc, sizeof(g_bridge_desc), "%s:%s", g_host.c_str(), g_port.c_str());
  Serial.printf("[mdns] found bridge at %s:%s\n", g_host.c_str(), g_port.c_str());
  return true;
}

// GET http://<bridge>/usage and fill `out`. Returns true on success.
static bool net_fetch(UsageState *out) {
  memset(out, 0, sizeof(*out));
  if (WiFi.status() != WL_CONNECTED) { strcpy(out->err, "wifi"); return false; }
  if (g_host.length() == 0) net_discover();                 // no IP typed -> mDNS
  if (g_host.length() == 0) { strcpy(out->err, "no bridge"); return false; }

  String url = "http://" + g_host + ":" + g_port + "/usage";
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) { strcpy(out->err, "url"); return false; }
  int code = http.GET();
  if (code != 200) {
    snprintf(out->err, sizeof(out->err), "http %d", code);
    http.end();
    net_discover();   // maybe the Mac's IP changed (moved locations) — refresh for next poll
    return false;
  }

  String body = http.getString();
  http.end();
  return parse_usage_body(body.c_str(), out);
}

// USB-serial transport: the Mac bridge can push the same /usage JSON as newline-
// delimited frames over the USB CDC link (works with no Wi-Fi at all). Reads at
// most one full frame per call (non-blocking); returns true and fills `out` when
// a complete, valid frame arrived.
static bool net_usb_read(UsageState *out) {
  static char buf[4096];
  static size_t len = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      bool got = false;
      if (len > 0 && buf[0] == '{') {
        buf[len] = '\0';
        memset(out, 0, sizeof(*out));
        got = parse_usage_body(buf, out);
        if (!got) Serial.printf("[usb] parse fail len=%u\n", (unsigned)len);
      } else if (len > 0) {
        buf[len] = '\0';
        if (strcmp(buf, "@VOICE") == 0) g_voice_trigger = true;   // test/remote push-to-talk
      }
      len = 0;
      if (got) return true;
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    } else {
      len = 0;   // frame too long -- drop it
    }
  }
  return false;
}

// Send one allowlisted action with the pairing token injected.
// `body` is one of ACTION_BODY[] (config.h). Prefers Wi-Fi (POST /action); when
// Wi-Fi isn't connected (e.g. carried to an office where the board can't join),
// falls back to the **USB serial** link — the bridge reads `@ACT <token> <body>`
// lines from the port, so the Remote works over the cable with no Wi-Fi.
// MUST be called from the network task (loop()), never inside an LVGL callback.
static bool net_action(const char *body) {
  if (g_token.length() == 0) return false;                      // not paired yet

  // Wi-Fi path: POST /action when connected + a bridge IP is set.
  if (WiFi.status() == WL_CONNECTED && g_host.length() > 0) {
    // Splice the token into the body: {"token":"...",<body without leading '{'>
    String payload = String("{\"token\":\"") + g_token + "\"," + (body + 1);
    String url = "http://" + g_host + ":" + g_port + "/action";
    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (http.begin(url)) {
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(payload);
      http.end();
      if (code == 200) return true;
      Serial.printf("[action] POST -> HTTP %d (token len %d)\n", code, (int)g_token.length());
      // fall through to USB in case the bridge is reachable over the cable
    }
  }

  // USB fallback: the bridge reads `@ACT <token> <body>` lines off the serial port.
  Serial.printf("@ACT %s %s\n", g_token.c_str(), body);
  return true;   // fire-and-forget; the bridge validates + executes locally
}
