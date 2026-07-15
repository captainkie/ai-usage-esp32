// Tunables + shared data model for the AI Usage Bar firmware.
#pragma once
#include <stdint.h>

// ---- Wi-Fi provisioning ----
#define WM_AP_NAME       "AI-Usage-Bar-Setup"   // captive-portal SSID
#define WM_AP_TIMEOUT_S  180                     // portal timeout before retrying STA

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
  char   model[40];
  char   effort[12];
  Window five;
  Window seven;
} ProviderState;

typedef struct {
  bool          ok;                 // last fetch succeeded
  ProviderState p[PV_COUNT];
  unsigned long stamp_ms;           // millis() at fetch (for countdown math)
  char          err[24];            // e.g. "unauthorized", "wifi", "http 500"
} UsageState;

static inline uint32_t util_color(int u) {
  if (u < 0) return COL_DIM;
  if (u > WARN_MAX) return COL_CRIT;
  if (u >= GOOD_MAX) return COL_WARN;
  return COL_GOOD;
}
