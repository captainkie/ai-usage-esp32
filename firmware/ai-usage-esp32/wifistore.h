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
  static StaticJsonDocument<1024> doc;   // static: keep this off the boot-path stack
  doc.clear();
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
  static StaticJsonDocument<1024> doc;   // static: keep this off the boot-path stack
  doc.clear();
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
  static WifiCred list[MAX_WIFI_APS];    // static: this boot-path runs single-threaded
  int n = wifistore_load(list, MAX_WIFI_APS);
  if (!wifistore_merge_into(list, &n, MAX_WIFI_APS, e)) return;
  static char buf[1024];
  wifistore_serialize(list, n, buf, sizeof(buf));
  Preferences p; p.begin("aiusage", false);
  p.putString("aps", buf);
  p.end();
}
#endif
