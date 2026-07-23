// Wi-Fi provisioning (captive portal) + polling the Mac bridge.
// Header-only: included once into the sketch translation unit.
#pragma once
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>     // tzapu/WiFiManager
#include <Preferences.h>
#include <ArduinoJson.h>     // bblanchon/ArduinoJson v6
#include "config.h"

static Preferences        g_prefs;
static String             g_host = "";
static String             g_port = String(DEFAULT_BRIDGE_PORT);
static String             g_token = "";        // pairing token (from the Mac bridge)
static char               g_bridge_desc[48] = "(not set)";
static WiFiManagerParameter *g_pHost = nullptr;
static WiFiManagerParameter *g_pPort = nullptr;
static WiFiManagerParameter *g_pTok  = nullptr;

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
  g_prefs.end();
  snprintf(g_bridge_desc, sizeof(g_bridge_desc), "%s:%s", g_host.c_str(), g_port.c_str());
}

// Bring up Wi-Fi. Opens the captive portal ("AI-Usage-Bar-Setup") when there
// are no saved credentials or no bridge IP yet; there the user picks Wi-Fi and
// types the Mac's IP + port. Blocks until connected.
static void net_begin() {
  g_prefs.begin("aiusage", true);
  g_host  = g_prefs.getString("host", "");
  g_port  = g_prefs.getString("port", String(DEFAULT_BRIDGE_PORT));
  g_token = g_prefs.getString("token", "");
  g_prefs.end();

  WiFiManager wm;
  wm.setConfigPortalTimeout(WM_AP_TIMEOUT_S);
  static WiFiManagerParameter pHost("host", "Mac bridge IP (e.g. 192.168.1.20)", g_host.c_str(), 24);
  static WiFiManagerParameter pPort("port", "Bridge port", g_port.c_str(), 6);
  static WiFiManagerParameter pTok("token", "Pairing token (from the Mac bridge)", g_token.c_str(), 40);
  g_pHost = &pHost; g_pPort = &pPort; g_pTok = &pTok;
  wm.addParameter(&pHost);
  wm.addParameter(&pPort);
  wm.addParameter(&pTok);
  wm.setSaveParamsCallback(net_save_params);

  // Force the portal the first time (no bridge IP yet); otherwise auto-connect.
  if (g_host.length() == 0) {
    while (g_host.length() == 0) wm.startConfigPortal(WM_AP_NAME);
  } else {
    // Don't block on a captive portal if Wi-Fi can't be joined (e.g. carried to an
    // office network) — return so USB-serial can still drive the display. The user
    // can long-press the brand to reopen the portal on demand.
    wm.setEnableConfigPortal(false);
    wm.autoConnect(WM_AP_NAME);
  }

  snprintf(g_bridge_desc, sizeof(g_bridge_desc), "%s:%s", g_host.c_str(), g_port.c_str());
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
}

static const char *net_bridge_desc() { return g_bridge_desc; }

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

// GET http://<bridge>/usage and fill `out`. Returns true on success.
static bool net_fetch(UsageState *out) {
  memset(out, 0, sizeof(*out));
  if (WiFi.status() != WL_CONNECTED) { strcpy(out->err, "wifi"); return false; }
  if (g_host.length() == 0)          { strcpy(out->err, "no bridge"); return false; }

  String url = "http://" + g_host + ":" + g_port + "/usage";
  HTTPClient http;
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(url)) { strcpy(out->err, "url"); return false; }
  int code = http.GET();
  if (code != 200) { snprintf(out->err, sizeof(out->err), "http %d", code); http.end(); return false; }

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
        Serial.printf("[usb] non-json len=%u first=%d\n", (unsigned)len, (int)buf[0]);
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
