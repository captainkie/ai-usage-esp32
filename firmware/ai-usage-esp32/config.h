// Tunables + shared data model for the AI Usage Bar firmware.
#pragma once
#include <stdint.h>

// ---- Wi-Fi provisioning ----
#define WM_AP_NAME       "AI-Usage-Bar-Setup"   // captive-portal SSID
#define WM_AP_TIMEOUT_S  600                     // portal timeout before retrying STA

// ---- multi-network Wi-Fi store (WiFiMulti + TF-card seed) ----
#define MAX_WIFI_APS         6         // max saved networks (home/office/hotspot/…)
#define WIFI_JOIN_TIMEOUT_MS 8000      // WiFiMulti.run() connect timeout per boot
#define SD_CONFIG_PATH       "/sdcard/pixie.json"   // TF-card root via the SDMMC /sdcard mount

typedef struct { char ssid[33]; char pass[64]; } WifiCred;

// ---- bridge polling ----
#define POLL_INTERVAL_MS   15000                 // how often to ask the Mac bridge
#define HTTP_TIMEOUT_MS    6000
#define DEFAULT_BRIDGE_PORT 8787

// ---- render ----
#define RENDER_INTERVAL_MS 120                   // mascot/gauge animation tick
#define SCREEN_W 640
#define SCREEN_H 172

// ---- traffic-light thresholds (utilisation %) ----
#define GOOD_MAX 60      // < 60  -> green
#define WARN_MAX 85      // 60-85 -> amber ; > 85 -> red

// ---- palette (0xRRGGBB) ----
#define COL_BG      0x0A0C14
#define COL_INK     0xEAECF2
#define COL_DIM     0x767C90
#define COL_CLAY    0xD97757
#define COL_GOOD    0x37E0A1
#define COL_WARN    0xFFC24B
#define COL_CRIT    0xFF5D6E
#define COL_TRACK   0x1B1F2C
#define COL_LIVE    0x37E0A1

// provider identity (index 0..2)
enum { PV_CLAUDE = 0, PV_CODEX, PV_GEMINI, PV_COUNT };
static const uint32_t PROVIDER_COLOR[PV_COUNT] = { 0xE08040, 0x12B886, 0x4285F4 };
static const char *PROVIDER_NAME[PV_COUNT]     = { "Claude", "Codex", "Gemini" };

// ---- shared state (written by the net task, read by the render timer) ----
typedef struct {
  int  util;        // 0..100, -1 = no data
  long reset_in;    // seconds until reset at poll time, -1 = unknown
} Window;

typedef struct {
  bool   linked;
  bool   stale;     // usage is a cached last-known-good reading (bridge is 429-backing-off)
  char   model[40];
  char   effort[12];
  Window five;
  Window seven;
} ProviderState;

// ---- Mac system stats (mirrors the bridge `system` block; -1/empty = no data) ----
// NOTE: SystemState must be declared before UsageState because UsageState embeds it.
typedef struct { char name[32]; int cpu; } TopProc;
typedef struct {
  bool ok;                 // system block present this fetch
  int  cpu;                // -1 = n/a
  int  mem_util;   float mem_used_gb, mem_total_gb;
  int  disk_util;  int   disk_used_gb, disk_total_gb;
  int  net_down_kbps, net_up_kbps;
  int  batt;               // -1 = no battery
  int  temp_c;             // -1 = n/a (bridge does not emit temp today)
  TopProc top[3]; int top_n;
} SystemState;

typedef struct {
  bool          ok;                 // last fetch succeeded
  ProviderState p[PV_COUNT];
  SystemState   sys;                // Mac monitor stats (screen ②) — same fetch, same g_mux
  unsigned long stamp_ms;           // millis() at fetch (for countdown math)
  char          err[24];            // e.g. "unauthorized", "wifi", "http 500"
} UsageState;

static inline uint32_t util_color(int u) {
  if (u < 0) return COL_DIM;
  if (u > WARN_MAX) return COL_CRIT;
  if (u >= GOOD_MAX) return COL_WARN;
  return COL_GOOD;
}

// ---- Mac remote (screen ③) ----
// Each remote button maps to a fixed JSON body sent to POST /action. The token
// is injected by net_action() at send time; do not put it here. The allowlist
// mirrors the bridge (bridge/lib/actions.mjs). open_app/shortcut names must
// exist in the Mac's actions.json — "Music"/"Safari"/"Focus" are the defaults
// from bridge/actions.example.json; rename there and you must match here.
enum { ACT_YT, ACT_MUSIC, ACT_SAFARI, ACT_FOCUS,
       ACT_PREV, ACT_PLAYPAUSE, ACT_NEXT,
       ACT_VOL_DN, ACT_VOL_MUTE, ACT_VOL_UP,
       ACT_LOCK, ACT_SLEEP, ACT_COUNT };
static const char *ACTION_BODY[ACT_COUNT] = {
  "{\"action\":\"open_url\",\"url\":\"https://youtube.com\"}",
  "{\"action\":\"open_app\",\"name\":\"Music\"}",
  "{\"action\":\"open_app\",\"name\":\"Safari\"}",
  "{\"action\":\"open_url\",\"url\":\"https://claude.ai\"}",
  "{\"action\":\"media\",\"key\":\"prev\"}",
  "{\"action\":\"media\",\"key\":\"playpause\"}",
  "{\"action\":\"media\",\"key\":\"next\"}",
  "{\"action\":\"volume\",\"dir\":\"down\"}",
  "{\"action\":\"volume\",\"dir\":\"mute\"}",
  "{\"action\":\"volume\",\"dir\":\"up\"}",
  "{\"action\":\"lock\"}",
  "{\"action\":\"display_sleep\"}",
};
