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
// NOTE: confirm sdcard_bsp_init() vs the Waveshare 04_SD_Card example before flashing
// (it may be SD.begin(cs) / SD_MMC.begin()); see the plan Task 3 Step 3.
static bool sdconf_load(PixieConfig *out) {
  memset(out, 0, sizeof(*out));
  if (!sdcard_bsp_init()) return false;
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
