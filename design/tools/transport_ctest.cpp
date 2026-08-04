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

static void test_cable_in_beats_useless_wifi() {
  // Office client isolation: the board joins Wi-Fi but cannot reach the Mac. With the
  // cable ALSO plugged in, "start the bridge" is the actionable advice — telling the
  // user to "use USB" while USB is already plugged is a dead end. Found on-device.
  TransportState t = ev(true, UINT32_MAX, true, false, "http -1", PREF_AUTO);
  CHECK(t.active == TR_USB);
  CHECK(!t.healthy);
  CHECK(strstr(t.hint, "start the bridge") != NULL);

  // Same network with the cable OUT -> the Wi-Fi explanation is the right one.
  t = ev(false, UINT32_MAX, true, false, "http -1", PREF_AUTO);
  CHECK(t.active == TR_WIFI);
  CHECK(strstr(t.hint, "use USB") != NULL);

  // Healthy Wi-Fi still outranks a cable that is plugged in but not delivering
  // (e.g. charging at home with the bridge stopped).
  t = ev(true, UINT32_MAX, true, true, "", PREF_AUTO);
  CHECK(t.active == TR_WIFI);
  CHECK(t.healthy);

  // A locked link is still never swapped, whatever the other link is doing.
  t = ev(true, UINT32_MAX, true, true, "", PREF_WIFI);
  CHECK(t.active == TR_WIFI);
  t = ev(false, UINT32_MAX, true, true, "", PREF_USB);
  CHECK(t.active == TR_USB);
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
  test_cable_in_beats_useless_wifi();
  test_pref_parse();
  test_buffers_bounded();
  printf(fails ? "\n%d CHECK(s) FAILED\n" : "\nALL PASS\n", fails);
  return fails ? 1 : 0;
}
