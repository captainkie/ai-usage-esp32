// Read-only TF-card config (/pixie.json). Header-only. NEVER writes to the card.
// sdconf_parse is pure (host-testable); sdconf_load needs the SD BSP (Arduino).
#pragma once
#include <string.h>
#include <stdio.h>
#include <ArduinoJson.h>
#include "config.h"
#ifdef ARDUINO
#include <esp_err.h>
#include "src/sdcard_bsp/sdcard_bsp.h"   // Waveshare 04_SD_Card: SDMMC mount at /sdcard
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
  static StaticJsonDocument<2048> doc;   // static: keep this off the boot-path stack
  doc.clear();
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
// Mount the TF card (SDMMC via the Waveshare 04_SD_Card BSP; mount point /sdcard) and
// read /sdcard/pixie.json. Read-only — the firmware never writes to the card. Returns
// false on no card / no file / bad JSON.
// GPIO8 SAFETY: sdcard_init() uses only the SDMMC pins (D0=40/CLK=41/CMD=39); it does NOT
// touch GPIO8 (the display's LEDC PWM backlight rail). We deliberately do NOT replicate the
// 04_SD_Card example's gpio_init() (which forces GPIO8 high), so mounting cannot dim/kill
// the panel. The SD card is powered independently of the backlight.
static bool sdconf_load(PixieConfig *out) {
  memset(out, 0, sizeof(*out));
  static bool mounted = false;
  if (!mounted) { sdcard_init(); mounted = true; }   // no-op safe if no card is inserted
  FILE *f = fopen(SD_CONFIG_PATH, "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz >= 4096) { fclose(f); return false; }
  static char buf[4096];
  size_t rd = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[rd] = '\0';
  return sdconf_parse(buf, out);
}
#endif
