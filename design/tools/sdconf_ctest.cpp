// Host test for the pure Wi-Fi store + pixie.json parse logic (no Arduino).
// Build: c++ -std=c++17 -I design/tools/vendor -I firmware/ai-usage-esp32 \
//            design/tools/sdconf_ctest.cpp -o /tmp/sdconf_ctest && /tmp/sdconf_ctest
// (ArduinoJson single header fetched to design/tools/vendor/ — see the plan.)
#include <cstdio>
#include <cstring>
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
  for (int i = 0; i < MAX_WIFI_APS; i++) {
    snprintf(ssid, sizeof(ssid), "net%d", i);
    WifiCred e; strlcpy(e.ssid, ssid, sizeof(e.ssid)); strcpy(e.pass, "p");
    wifistore_merge_into(list, &n, MAX_WIFI_APS, &e);
  }
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
  // test_sdconf() added in Task 3
  printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
  return fails ? 1 : 0;
}
