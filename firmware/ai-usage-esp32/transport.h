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
