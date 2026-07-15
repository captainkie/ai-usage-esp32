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
static char               g_bridge_desc[48] = "(not set)";
static WiFiManagerParameter *g_pHost = nullptr;
static WiFiManagerParameter *g_pPort = nullptr;

static void net_save_params() {
  if (g_pHost) g_host = g_pHost->getValue();
  if (g_pPort) g_port = g_pPort->getValue();
  g_host.trim(); g_port.trim();
  if (g_port.length() == 0) g_port = String(DEFAULT_BRIDGE_PORT);
  g_prefs.begin("aiusage", false);
  g_prefs.putString("host", g_host);
  g_prefs.putString("port", g_port);
  g_prefs.end();
  snprintf(g_bridge_desc, sizeof(g_bridge_desc), "%s:%s", g_host.c_str(), g_port.c_str());
}

// Bring up Wi-Fi. Opens the captive portal ("AI-Usage-Bar-Setup") when there
// are no saved credentials or no bridge IP yet; there the user picks Wi-Fi and
// types the Mac's IP + port. Blocks until connected.
static void net_begin() {
  g_prefs.begin("aiusage", true);
  g_host = g_prefs.getString("host", "");
  g_port = g_prefs.getString("port", String(DEFAULT_BRIDGE_PORT));
  g_prefs.end();

  WiFiManager wm;
  wm.setConfigPortalTimeout(WM_AP_TIMEOUT_S);
  static WiFiManagerParameter pHost("host", "Mac bridge IP (e.g. 192.168.1.20)", g_host.c_str(), 24);
  static WiFiManagerParameter pPort("port", "Bridge port", g_port.c_str(), 6);
  g_pHost = &pHost; g_pPort = &pPort;
  wm.addParameter(&pHost);
  wm.addParameter(&pPort);
  wm.setSaveParamsCallback(net_save_params);

  // Force the portal the first time (no bridge IP yet); otherwise auto-connect.
  if (g_host.length() == 0) wm.startConfigPortal(WM_AP_NAME);
  else                      wm.autoConnect(WM_AP_NAME);

  snprintf(g_bridge_desc, sizeof(g_bridge_desc), "%s:%s", g_host.c_str(), g_port.c_str());
}

// Re-open the portal on demand (e.g. long-press a side button).
static void net_portal() {
  WiFiManager wm;
  static WiFiManagerParameter pHost("host", "Mac bridge IP", g_host.c_str(), 24);
  static WiFiManagerParameter pPort("port", "Bridge port", g_port.c_str(), 6);
  g_pHost = &pHost; g_pPort = &pPort;
  wm.addParameter(&pHost); wm.addParameter(&pPort);
  wm.setSaveParamsCallback(net_save_params);
  wm.startConfigPortal(WM_AP_NAME);
}

static const char *net_bridge_desc() { return g_bridge_desc; }

static void parse_window(JsonVariantConst w, Window *dst) {
  if (w.isNull()) { dst->util = -1; dst->reset_in = -1; return; }
  dst->util     = w["util"]     | -1;
  dst->reset_in = w["reset_in"] | (long)-1;
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

  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, body)) { strcpy(out->err, "json"); return false; }

  JsonObjectConst provs = doc["providers"];
  const char *keys[PV_COUNT] = { "claude", "codex", "gemini" };
  for (int i = 0; i < PV_COUNT; i++) {
    JsonObjectConst pr = provs[keys[i]];
    ProviderState *ps = &out->p[i];
    ps->linked = pr["linked"] | false;
    strlcpy(ps->model,  pr["model"]  | "", sizeof(ps->model));
    strlcpy(ps->effort, pr["effort"] | "", sizeof(ps->effort));
    parse_window(pr["five_hour"], &ps->five);
    parse_window(pr["seven_day"], &ps->seven);
  }
  out->ok = true;
  out->stamp_ms = millis();
  out->err[0] = 0;
  return true;
}
