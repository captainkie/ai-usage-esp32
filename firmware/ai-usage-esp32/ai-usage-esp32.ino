/*
 * AI Usage Bar -- ESP32 edition
 * Waveshare ESP32-S3-Touch-LCD-3.49 (640×172, AXS15231B QSPI + capacitive touch)
 *
 * Shows your Claude Code 5-hour / weekly limits as twin arc gauges with a live
 * reset countdown, the current model + effort, touch-switchable providers, and
 * a pixel companion (cat / robot / husky / owl / T-800 / runner / dino) whose
 * colour follows the provider and whose mood follows the busiest gauge.
 *
 * Data comes from the Mac "bridge" (../bridge) over your LAN -- your token never
 * leaves the Mac.
 *
 * BUILD: this sketch is a drop-in on top of Waveshare's official
 * "09_LVGL_V8_Test" example (which brings up the panel + touch + IO expander).
 * See firmware/README.md for the exact steps, libraries, and the tiny
 * lvgl_port lock patch this file relies on.
 */
#include "user_config.h"                    // Waveshare BSP: pins + resolution
#include "lvgl_port.h"                       // Waveshare display/touch bring-up
#include "i2c_bsp.h"
#include "src/lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include <lvgl.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "mascot.h"
#include "net.h"

// Exposed by the small lvgl_port patch (see firmware/README.md).
extern "C" bool aiusage_lvgl_lock(int timeout_ms);
extern "C" void aiusage_lvgl_unlock(void);

#define LVC(rgb) lv_color_hex(rgb)

/* ---------------- shared state (net task <-> render timer) ---------------- */
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static UsageState   g_state;
static bool         g_have = false;
static int          g_provider = PV_CLAUDE;
static int          g_mascot   = MASC_RUNNER;   // default companion (tap to change)

/* ---------------- LVGL objects ---------------- */
static lv_obj_t *lblBrand, *lblModel, *lblEffort, *lblLive;
static lv_obj_t *arcFive, *arcSeven;
static lv_obj_t *lblFivePct, *lblSevenPct, *lblFiveLab, *lblSevenLab, *lblFiveRst, *lblSevenRst;
static lv_obj_t *canvasMascot, *pill[PV_COUNT];
static lv_color_t *mascotBuf = nullptr;

/* ---------------- multi-screen: tileview + monitor/remote/dots ---------------- */
static lv_obj_t *g_tv;                         // tileview holding the 3 screens
static lv_obj_t *tile[3];                       // 0 usage · 1 monitor · 2 remote
static lv_obj_t *dotsBox, *dot[3];              // page indicator (shown transiently)
static lv_obj_t *arcCpu, *arcRam, *arcDisk;     // monitor gauges (screen ②)
static lv_obj_t *mValCpu, *mValRam, *mValDisk;  // big % values
static lv_obj_t *mCapCpu, *mCapRam, *mCapDisk;  // captions under each gauge
static lv_obj_t *mTicker;                        // net/battery/temp ticker
static lv_obj_t *lblToast;                       // remote action confirmation

// Remote action queue: an LVGL tap only enqueues here; loop() does the HTTP POST
// (HTTP inside an LVGL callback would freeze rendering).
static volatile int g_action = -1;              // ACT_* or -1 (empty)
static uint32_t g_toast_until = 0;              // millis() until toast auto-hides
static uint32_t g_dots_until  = 0;              // millis() until page dots auto-hide

static float curFive = 0, curSeven = 0;   // eased gauge values (screen ①)
static float curCpu = 0, curRam = 0, curDisk = 0;   // eased gauge values (screen ②)
static uint32_t animT = 0;

/* ---------------- helpers ---------------- */
static void fmt_reset(long secs, char *out, size_t n) {
  if (secs < 0)  { snprintf(out, n, "--"); return; }
  if (secs == 0) { snprintf(out, n, "now"); return; }
  long m = secs / 60, d = m / 1440, h = (m % 1440) / 60, mm = m % 60;
  if (d > 0)      snprintf(out, n, "%ldd %02ldh", d, h);
  else if (h > 0) snprintf(out, n, "%ldh %02ldm", h, mm);
  else            snprintf(out, n, "%ldm", mm);
}

static void style_pill(int i, bool active) {
  lv_obj_t *p = pill[i];
  lv_obj_t *lbl = lv_obj_get_child(p, 0);
  if (active) {
    lv_obj_set_style_bg_color(p, LVC(PROVIDER_COLOR[i]), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lbl, LVC(0x0A0B10), 0);
  } else {
    lv_obj_set_style_bg_color(p, LVC(0x12141D), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lbl, LVC(COL_DIM), 0);
  }
}

/* ---------------- touch events ---------------- */
static void pill_cb(lv_event_t *e) {
  g_provider = (int)(intptr_t)lv_event_get_user_data(e);
}
static void mascot_cb(lv_event_t *e) {
  (void)e;
  g_mascot = (g_mascot + 1) % MASC_COUNT;
}

/* ---------------- remote action queue (LVGL tap -> net task) ---------------- */
// Short on-screen confirmations, indexed by ACT_* (see config.h).
static const char *ACTION_TOAST[ACT_COUNT] = {
  "YouTube", "Music", "Safari", "Focus",
  "Prev", "Play/Pause", "Next",
  "Volume -", "Mute", "Volume +",
  "Lock screen", "Display sleep",
};

// Reveal a brief confirmation. g_token lives in net.h (same translation unit),
// so we can warn up-front when the device isn't paired yet.
static void show_toast(int a) {
  if (!lblToast) return;
  if (g_token.length() == 0)
    lv_label_set_text(lblToast, LV_SYMBOL_WARNING " No pairing token -- run Wi-Fi setup");
  else
    lv_label_set_text_fmt(lblToast, LV_SYMBOL_OK " %s", ACTION_TOAST[a]);
  lv_obj_clear_flag(lblToast, LV_OBJ_FLAG_HIDDEN);
  g_toast_until = millis() + 1500;
}

// A remote button was tapped: enqueue the action (loop() POSTs it) + confirm.
// NEVER do network I/O here -- that would block the LVGL render task.
static void remote_cb(lv_event_t *e) {
  int a = (int)(intptr_t)lv_event_get_user_data(e);
  g_action = a;                       // net task drains this in loop()
  show_toast(a);
}

// The tileview scrolled to a new screen: light the matching page dot + reveal.
static void tile_changed_cb(lv_event_t *e) {
  (void)e;
  if (!dotsBox) return;               // tiles/dots not built yet -- ignore any early event
  lv_obj_t *act = lv_tileview_get_tile_act(g_tv);
  for (int i = 0; i < 3; i++)
    lv_obj_set_style_bg_color(dot[i], LVC(act == tile[i] ? COL_INK : 0x4A4F60), 0);
  if (dotsBox) lv_obj_clear_flag(dotsBox, LV_OBJ_FLAG_HIDDEN);
  g_dots_until = millis() + 1500;
}

/* ---------------- shared screen helpers ---------------- */
// Topbar identical to screen ①: brand (left), centered title, LIVE (right).
// Shorten a process name for the tiny caption: last component of a reverse-DNS
// bundle id (com.apple.WebKit.WebContent -> WebContent), capped to fit one line.
static void short_name(const char *in, char *out, size_t n) {
  const char *base = in;
  const char *dot = strrchr(in, '.');
  if (dot && dot[1]) base = dot + 1;
  strlcpy(out, base, n);
  if (strlen(out) > 14) out[14] = '\0';
}

// Long-press the brand to reopen the Wi-Fi/setup portal (change network / enter
// pairing token). The LVGL callback only sets a flag; loop() does the blocking work.
static volatile bool g_reprovision = false;
static void brand_cb(lv_event_t *e) { (void)e; g_reprovision = true; }

static void build_topbar(lv_obj_t *parent, const char *title) {
  lv_obj_t *b = lv_label_create(parent);
  lv_label_set_text(b, "AI\xE2\x80\xA2USAGE");
  lv_obj_set_style_text_color(b, LVC(COL_CLAY), 0);
  lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(b, 12, 9);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, brand_cb, LV_EVENT_LONG_PRESSED, NULL);

  lv_obj_t *t = lv_label_create(parent);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, LVC(COL_INK), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

  lv_obj_t *lv = lv_label_create(parent);
  lv_label_set_text(lv, LV_SYMBOL_WIFI " LIVE");
  lv_obj_set_style_text_color(lv, LVC(COL_LIVE), 0);
  lv_obj_set_style_text_font(lv, &lv_font_montserrat_14, 0);
  lv_obj_align(lv, LV_ALIGN_TOP_RIGHT, -8, 9);
}

// A 270° meter arc styled exactly like the screen ① gauges.
static lv_obj_t *make_gauge(lv_obj_t *parent, int cx, int cy, int r) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, r * 2, r * 2);
  lv_obj_set_pos(a, cx - r, cy - r);
  lv_arc_set_range(a, 0, 100);
  lv_arc_set_value(a, 0);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(a, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, LVC(COL_TRACK), LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, LVC(COL_GOOD), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
  return a;
}

/* ---------------- screen ① AI Usage (moved verbatim from ui_build) ---------------- */
static void screen_usage_build(lv_obj_t *parent) {
  // top strip
  lblBrand = lv_label_create(parent);
  lv_label_set_text(lblBrand, "AI\xE2\x80\xA2USAGE");
  lv_obj_set_style_text_color(lblBrand, LVC(COL_CLAY), 0);
  lv_obj_set_style_text_font(lblBrand, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(lblBrand, 12, 9);
  // Long-press the brand on screen ① too (parity with ②③) to reopen the setup portal.
  lv_obj_add_flag(lblBrand, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(lblBrand, brand_cb, LV_EVENT_LONG_PRESSED, NULL);

  lblModel = lv_label_create(parent);
  lv_label_set_text(lblModel, "--");
  lv_obj_set_style_text_color(lblModel, LVC(COL_INK), 0);
  lv_obj_set_style_text_font(lblModel, &lv_font_montserrat_20, 0);
  lv_obj_align(lblModel, LV_ALIGN_TOP_MID, 0, 6);

  lblEffort = lv_label_create(parent);
  lv_label_set_text(lblEffort, "");
  lv_obj_set_style_text_color(lblEffort, LVC(0xAEB4C6), 0);
  lv_obj_set_style_text_font(lblEffort, &lv_font_montserrat_14, 0);
  lv_obj_set_style_bg_color(lblEffort, LVC(0x1A1D29), 0);
  lv_obj_set_style_bg_opa(lblEffort, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(lblEffort, 6, 0);
  lv_obj_set_style_pad_ver(lblEffort, 2, 0);
  lv_obj_set_style_radius(lblEffort, 5, 0);
  lv_obj_align(lblEffort, LV_ALIGN_TOP_RIGHT, -70, 8);

  lblLive = lv_label_create(parent);
  lv_label_set_text(lblLive, LV_SYMBOL_WIFI " LIVE");
  lv_obj_set_style_text_color(lblLive, LVC(COL_LIVE), 0);
  lv_obj_set_style_text_font(lblLive, &lv_font_montserrat_14, 0);
  lv_obj_align(lblLive, LV_ALIGN_TOP_RIGHT, -8, 9);

  // gauges -- default lv_arc is a 270° meter with the gap at the bottom
  const int R = 44;
  int gx[2] = { 120, 520 };
  lv_obj_t **arcs[2] = { &arcFive, &arcSeven };
  for (int i = 0; i < 2; i++) {
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, R * 2, R * 2);
    lv_obj_set_pos(a, gx[i] - R, 97 - R);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, LVC(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, LVC(COL_GOOD), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    *arcs[i] = a;
  }

  struct { lv_obj_t **lab, **pct, **rst; int cx; const char *text; } G[2] = {
    { &lblFiveLab, &lblFivePct, &lblFiveRst, 120, "5-HOUR" },
    { &lblSevenLab, &lblSevenPct, &lblSevenRst, 520, "WEEK" },
  };
  for (int i = 0; i < 2; i++) {
    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab, G[i].text);
    lv_obj_set_style_text_color(lab, LVC(COL_DIM), 0);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
    lv_obj_align(lab, LV_ALIGN_TOP_MID, G[i].cx - 320, 34);
    *G[i].lab = lab;

    lv_obj_t *pct = lv_label_create(parent);
    lv_label_set_text(pct, "--");
    lv_obj_set_style_text_color(pct, LVC(COL_GOOD), 0);
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_28, 0);
    lv_obj_align(pct, LV_ALIGN_CENTER, G[i].cx - 320, 11);
    *G[i].pct = pct;

    lv_obj_t *rst = lv_label_create(parent);
    lv_label_set_text(rst, "");
    lv_obj_set_style_text_color(rst, LVC(0x9AA0B2), 0);
    lv_obj_set_style_text_font(rst, &lv_font_montserrat_14, 0);
    lv_obj_align(rst, LV_ALIGN_TOP_MID, G[i].cx - 320, 145);
    *G[i].rst = rst;
  }

  // mascot canvas (120×120 buffer, RGB565), centred
  mascotBuf = (lv_color_t *)heap_caps_malloc(120 * 120 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  canvasMascot = lv_canvas_create(parent);
  lv_canvas_set_buffer(canvasMascot, mascotBuf, 120, 120, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(canvasMascot, 320 - 60, 97 - 60);
  lv_obj_add_flag(canvasMascot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvasMascot, mascot_cb, LV_EVENT_CLICKED, NULL);

  // provider pills
  for (int i = 0; i < PV_COUNT; i++) {
    lv_obj_t *p = lv_btn_create(parent);
    lv_obj_set_size(p, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_radius(p, 12, 0);
    lv_obj_set_style_shadow_width(p, 0, 0);
    lv_obj_set_style_pad_hor(p, 10, 0);
    lv_obj_add_event_cb(p, pill_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, PROVIDER_NAME[i]);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    // lay the three out centred along the bottom
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, (i - 1) * 92, -6);
    pill[i] = p;
  }
}

/* ---------------- screen ② Mac Monitor ---------------- */
static void screen_monitor_build(lv_obj_t *parent) {
  build_topbar(parent, "MAC \xE2\x80\xA2 MONITOR");

  struct { lv_obj_t **arc, **val, **cap; int cx; const char *lab; } G[3] = {
    { &arcCpu,  &mValCpu,  &mCapCpu,  110, "CPU"  },
    { &arcRam,  &mValRam,  &mCapRam,  320, "RAM"  },
    { &arcDisk, &mValDisk, &mCapDisk, 530, "DISK" },
  };
  const int cy = 84, R = 36;
  for (int i = 0; i < 3; i++) {
    *G[i].arc = make_gauge(parent, G[i].cx, cy, R);

    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab, G[i].lab);
    lv_obj_set_style_text_color(lab, LVC(COL_DIM), 0);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lab, 120);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(lab, G[i].cx - 60, 30);

    lv_obj_t *val = lv_label_create(parent);
    lv_label_set_text(val, "--");
    lv_obj_set_style_text_color(val, LVC(COL_DIM), 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
    lv_obj_set_width(val, 120);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(val, G[i].cx - 60, cy - 12);
    *G[i].val = val;

    lv_obj_t *cap = lv_label_create(parent);
    lv_label_set_text(cap, "--");
    lv_obj_set_style_text_color(cap, LVC(0x9AA0B2), 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_14, 0);
    lv_obj_set_width(cap, 150);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(cap, G[i].cx - 75, cy + 40);
    *G[i].cap = cap;
  }

  // net / battery / temp ticker along the bottom
  mTicker = lv_label_create(parent);
  lv_label_set_text(mTicker, "waiting for bridge...");
  lv_obj_set_style_text_color(mTicker, LVC(0xAEB4C6), 0);
  lv_obj_set_style_text_font(mTicker, &lv_font_montserrat_14, 0);
  lv_obj_align(mTicker, LV_ALIGN_BOTTOM_MID, 0, -6);
}

/* ---------------- screen ③ Mac Remote ---------------- */
static void screen_remote_build(lv_obj_t *parent) {
  build_topbar(parent, "MAC \xE2\x80\xA2 REMOTE");

  // shortcut row: 4 tiles (icon + name), enqueue open_url/open_app/shortcut
  struct { const char *icon, *name; int act; } S[4] = {
    { LV_SYMBOL_VIDEO, "YouTube", ACT_YT     },
    { LV_SYMBOL_AUDIO, "Music",   ACT_MUSIC  },
    { LV_SYMBOL_GPS,   "Safari",  ACT_SAFARI },
    { LV_SYMBOL_BELL,  "Focus",   ACT_FOCUS  },
  };
  const int tW = 144, tY = 31, tH = 56, gap = 10;
  for (int i = 0; i < 4; i++) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, tW, tH);
    lv_obj_set_pos(b, 16 + i * (tW + gap), tY);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, LVC(0x12141D), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, LVC(0x23273A), 0);
    lv_obj_add_event_cb(b, remote_cb, LV_EVENT_CLICKED, (void *)(intptr_t)S[i].act);

    // one centered "icon  name" label — stacking two labels in a short button
    // overlapped them, and a single line is easier to read at a glance.
    lv_obj_t *nm = lv_label_create(b);
    char lbl[40];
    snprintf(lbl, sizeof(lbl), "%s  %s", S[i].icon, S[i].name);
    lv_label_set_text(nm, lbl);
    lv_obj_set_style_text_color(nm, LVC(COL_INK), 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
    lv_obj_center(nm);
  }

  // control row: media (⏮⏯⏭) · volume (−/mute/+) · system (lock/sleep).
  // Symbol glyphs where clear; "LOCK"/"SLEEP" as text (montserrat has no padlock).
  const int cY = 100, bH = 36;
  struct { const char *glyph; int act; int x; int w; bool primary; } C[8] = {
    { LV_SYMBOL_PREV,       ACT_PREV,       30, 36, false },
    { LV_SYMBOL_PLAY,       ACT_PLAYPAUSE,  79, 36, true  },
    { LV_SYMBOL_NEXT,       ACT_NEXT,      128, 36, false },
    { LV_SYMBOL_VOLUME_MID, ACT_VOL_DN,    218, 36, false },
    { LV_SYMBOL_MUTE,       ACT_VOL_MUTE,  267, 36, false },
    { LV_SYMBOL_VOLUME_MAX, ACT_VOL_UP,    316, 36, false },
    { "LOCK",               ACT_LOCK,      420, 84, false },
    { "SLEEP",              ACT_SLEEP,     514, 84, false },
  };
  for (int i = 0; i < 8; i++) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, C[i].w, bH);
    lv_obj_set_pos(b, C[i].x, cY);
    lv_obj_set_style_radius(b, 9, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, LVC(C[i].primary ? COL_GOOD : 0x181B26), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, LVC(C[i].primary ? COL_GOOD : 0x262A3A), 0);
    lv_obj_add_event_cb(b, remote_cb, LV_EVENT_CLICKED, (void *)(intptr_t)C[i].act);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, C[i].glyph);
    lv_obj_set_style_text_color(l, LVC(C[i].primary ? 0x062015 : COL_INK), 0);
    // Word labels ("LOCK"/"SLEEP") use the small font; LV_SYMBOL glyphs (high
    // bytes, negative as signed char) use the larger one.
    bool word = (C[i].glyph[0] >= 'A' && C[i].glyph[0] <= 'Z');
    lv_obj_set_style_text_font(l, word ? &lv_font_montserrat_14 : &lv_font_montserrat_20, 0);
    lv_obj_center(l);
  }

  // hidden toast (shown ~1.5s on tap; auto-hidden by render_cb)
  lblToast = lv_label_create(parent);
  lv_label_set_text(lblToast, "");
  lv_obj_set_style_text_color(lblToast, LVC(0x7DFFC4), 0);
  lv_obj_set_style_text_font(lblToast, &lv_font_montserrat_14, 0);
  lv_obj_set_style_bg_color(lblToast, LVC(0x0B1F17), 0);
  lv_obj_set_style_bg_opa(lblToast, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(lblToast, 1, 0);
  lv_obj_set_style_border_color(lblToast, LVC(0x1F6F4F), 0);
  lv_obj_set_style_pad_hor(lblToast, 10, 0);
  lv_obj_set_style_pad_ver(lblToast, 3, 0);
  lv_obj_set_style_radius(lblToast, 8, 0);
  lv_obj_align(lblToast, LV_ALIGN_BOTTOM_MID, 0, -4);
  lv_obj_add_flag(lblToast, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- page dots (overlaid on the screen, above the tiles) ---------------- */
static void build_dots(lv_obj_t *scr) {
  dotsBox = lv_obj_create(scr);
  lv_obj_remove_style_all(dotsBox);
  lv_obj_set_size(dotsBox, 3 * 6 + 2 * 8, 8);       // 3 dots + 8px gaps
  lv_obj_clear_flag(dotsBox, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(dotsBox, LV_ALIGN_BOTTOM_MID, 0, -3);
  for (int i = 0; i < 3; i++) {
    dot[i] = lv_obj_create(dotsBox);
    lv_obj_remove_style_all(dot[i]);
    lv_obj_set_size(dot[i], 6, 6);
    lv_obj_set_pos(dot[i], i * 14, 1);
    lv_obj_set_style_radius(dot[i], 3, 0);
    lv_obj_set_style_bg_color(dot[i], LVC(i == 0 ? COL_INK : 0x4A4F60), 0);
    lv_obj_set_style_bg_opa(dot[i], LV_OPA_COVER, 0);
    lv_obj_clear_flag(dot[i], LV_OBJ_FLAG_SCROLLABLE);
  }
  g_dots_until = millis() + 1500;                    // flash once on boot
}

/* ---------------- build UI (call under the LVGL lock) ---------------- */
static void ui_build() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, LVC(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  g_tv = lv_tileview_create(scr);
  lv_obj_set_size(g_tv, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_opa(g_tv, LV_OPA_TRANSP, 0);
  lv_obj_set_scrollbar_mode(g_tv, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_pad_all(g_tv, 0, 0);          // no padding: children keep the mockup's coords
  lv_obj_add_event_cb(g_tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // three horizontal tiles (col 0/1/2, row 0), swipe left/right between them
  tile[0] = lv_tileview_add_tile(g_tv, 0, 0, LV_DIR_HOR);
  tile[1] = lv_tileview_add_tile(g_tv, 1, 0, LV_DIR_HOR);
  tile[2] = lv_tileview_add_tile(g_tv, 2, 0, LV_DIR_HOR);
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_bg_color(tile[i], LVC(COL_BG), 0);
    lv_obj_set_style_bg_opa(tile[i], LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tile[i], LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(tile[i], 0, 0);     // children position from (0,0), per the mockup
  }

  screen_usage_build(tile[0]);      // ① unchanged (same coords, same events)
  screen_monitor_build(tile[1]);    // ② Mac Monitor
  screen_remote_build(tile[2]);     // ③ Mac Remote
  build_dots(scr);                  // page indicator, above the tiles
}

/* ---------------- per-frame update (LVGL timer -> runs under the lock) ---------------- */
static void render_cb(lv_timer_t *t) {
  (void)t;
  animT += RENDER_INTERVAL_MS;

  UsageState st;
  portENTER_CRITICAL(&g_mux);
  st = g_state;
  bool have = g_have;
  portEXIT_CRITICAL(&g_mux);

  ProviderState *pr = &st.p[g_provider];
  int tFive  = have ? pr->five.util  : -1;
  int tSeven = have ? pr->seven.util : -1;

  // ease gauges toward targets
  float dstFive  = tFive  < 0 ? 0 : tFive;
  float dstSeven = tSeven < 0 ? 0 : tSeven;
  curFive  += (dstFive  - curFive)  * 0.28f;
  curSeven += (dstSeven - curSeven) * 0.28f;

  lv_arc_set_value(arcFive,  (int)lroundf(curFive));
  lv_arc_set_value(arcSeven, (int)lroundf(curSeven));
  lv_obj_set_style_arc_color(arcFive,  LVC(util_color(tFive)),  LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arcSeven, LVC(util_color(tSeven)), LV_PART_INDICATOR);

  char buf[24];
  if (tFive  >= 0) { snprintf(buf, sizeof(buf), "%d%%", tFive);  lv_label_set_text(lblFivePct, buf); }
  else               lv_label_set_text(lblFivePct, "--");
  if (tSeven >= 0) { snprintf(buf, sizeof(buf), "%d%%", tSeven); lv_label_set_text(lblSevenPct, buf); }
  else               lv_label_set_text(lblSevenPct, "--");
  lv_obj_set_style_text_color(lblFivePct,  LVC(util_color(tFive)),  0);
  lv_obj_set_style_text_color(lblSevenPct, LVC(util_color(tSeven)), 0);

  // model + effort
  if (have && pr->model[0])      lv_label_set_text(lblModel, pr->model);
  else if (have && !pr->linked)  lv_label_set_text(lblModel, "not linked");
  else if (have)                 lv_label_set_text(lblModel, "no live data");
  else                           lv_label_set_text(lblModel, "connecting...");
  lv_obj_align(lblModel, LV_ALIGN_TOP_MID, 0, 6);
  // effort chip stays in the top-right corner; hidden when the provider has none
  if (have && pr->effort[0]) {
    lv_label_set_text(lblEffort, pr->effort);
    lv_obj_clear_flag(lblEffort, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(lblEffort, LV_OBJ_FLAG_HIDDEN);
  }

  // countdowns (decrement locally from poll time)
  long elapsed = have ? (long)((millis() - st.stamp_ms) / 1000) : 0;
  char r5[16], r7[16];
  fmt_reset(tFive  >= 0 && pr->five.reset_in  >= 0 ? pr->five.reset_in  - elapsed : -1, r5, sizeof(r5));
  fmt_reset(tSeven >= 0 && pr->seven.reset_in >= 0 ? pr->seven.reset_in - elapsed : -1, r7, sizeof(r7));
  char rr[24];
  snprintf(rr, sizeof(rr), LV_SYMBOL_REFRESH " %s", r5); lv_label_set_text(lblFiveRst, rr);
  snprintf(rr, sizeof(rr), LV_SYMBOL_REFRESH " %s", r7); lv_label_set_text(lblSevenRst, rr);

  // LIVE indicator
  lv_obj_set_style_text_color(lblLive, LVC(have && st.ok ? COL_LIVE : 0x6B7180), 0);

  // pills
  for (int i = 0; i < PV_COUNT; i++) style_pill(i, i == g_provider);

  // mascot
  int worst = tFive > tSeven ? tFive : tSeven;
  if (worst < 0) worst = 0;
  int mood = mascot_mood_for(worst);
  mascot_render((uint16_t *)mascotBuf, 120, 120, g_mascot,
                PROVIDER_COLOR[g_provider], mood, animT, mascot_to565(COL_BG));
#if LV_COLOR_16_SWAP
  uint16_t *mb = (uint16_t *)mascotBuf;
  for (int i = 0; i < 120 * 120; i++) { uint16_t v = mb[i]; mb[i] = (uint16_t)((v >> 8) | (v << 8)); }
#endif
  lv_obj_invalidate(canvasMascot);

  /* ---- screen ② Mac Monitor (updates even when hidden; cheap) ---- */
  SystemState *sy = &st.sys;
  int mc = sy->ok ? sy->cpu       : -1;
  int mr = sy->ok ? sy->mem_util  : -1;
  int md = sy->ok ? sy->disk_util : -1;
  curCpu  += ((mc < 0 ? 0 : mc) - curCpu)  * 0.28f;
  curRam  += ((mr < 0 ? 0 : mr) - curRam)  * 0.28f;
  curDisk += ((md < 0 ? 0 : md) - curDisk) * 0.28f;
  lv_arc_set_value(arcCpu,  (int)lroundf(curCpu));
  lv_arc_set_value(arcRam,  (int)lroundf(curRam));
  lv_arc_set_value(arcDisk, (int)lroundf(curDisk));
  lv_obj_set_style_arc_color(arcCpu,  LVC(util_color(mc)), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arcRam,  LVC(util_color(mr)), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arcDisk, LVC(util_color(md)), LV_PART_INDICATOR);

  struct { lv_obj_t *val; int u; } MV[3] = { { mValCpu, mc }, { mValRam, mr }, { mValDisk, md } };
  for (int i = 0; i < 3; i++) {
    if (MV[i].u >= 0) { snprintf(buf, sizeof(buf), "%d%%", MV[i].u); lv_label_set_text(MV[i].val, buf); }
    else                lv_label_set_text(MV[i].val, "--");
    lv_obj_set_style_text_color(MV[i].val, LVC(util_color(MV[i].u)), 0);
  }

  // captions: CPU -> busiest process · RAM/DISK -> used / total (no %f: newlib-nano)
  char cbuf[48];
  if (sy->ok && sy->top_n > 0) {
    char pn[20]; short_name(sy->top[0].name, pn, sizeof(pn));
    snprintf(cbuf, sizeof(cbuf), "%s %d%%", pn, sy->top[0].cpu);
    lv_label_set_text(mCapCpu, cbuf);
  } else lv_label_set_text(mCapCpu, "--");

  if (sy->ok && sy->mem_total_gb > 0) {
    int uw = (int)sy->mem_used_gb, uf = (int)((sy->mem_used_gb - uw) * 10 + 0.5f);
    snprintf(cbuf, sizeof(cbuf), "%d.%d / %d GB", uw, uf, (int)(sy->mem_total_gb + 0.5f));
    lv_label_set_text(mCapRam, cbuf);
  } else lv_label_set_text(mCapRam, "--");

  if (sy->ok && sy->disk_total_gb > 0) {
    snprintf(cbuf, sizeof(cbuf), "%d / %d GB", sy->disk_used_gb, sy->disk_total_gb);
    lv_label_set_text(mCapDisk, cbuf);
  } else lv_label_set_text(mCapDisk, "--");

  // ticker: net down/up · battery · temp (each shown only when present)
  if (sy->ok) {
    char tk[96]; int n = 0;
    n += snprintf(tk + n, sizeof(tk) - n, LV_SYMBOL_DOWN " %d  " LV_SYMBOL_UP " %d KB/s",
                  sy->net_down_kbps, sy->net_up_kbps);
    if (sy->batt   >= 0) n += snprintf(tk + n, sizeof(tk) - n, "   " LV_SYMBOL_BATTERY_FULL " %d%%", sy->batt);
    if (sy->temp_c >= 0) n += snprintf(tk + n, sizeof(tk) - n, "   %dC", sy->temp_c);
    lv_label_set_text(mTicker, tk);
  } else {
    lv_label_set_text(mTicker, "waiting for bridge...");
  }

  /* ---- transient page dots + remote toast ---- */
  if (dotsBox) {
    if (millis() < g_dots_until) lv_obj_clear_flag(dotsBox, LV_OBJ_FLAG_HIDDEN);
    else                         lv_obj_add_flag(dotsBox, LV_OBJ_FLAG_HIDDEN);
  }
  if (lblToast) {
    if (g_toast_until && millis() < g_toast_until) lv_obj_clear_flag(lblToast, LV_OBJ_FLAG_HIDDEN);
    else                                           lv_obj_add_flag(lblToast, LV_OBJ_FLAG_HIDDEN);
  }
}

static void ui_setup_screen() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clean(scr);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, LVC(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_t *brand = lv_label_create(scr);
  lv_label_set_text(brand, "AI\xE2\x80\xA2USAGE  \xE2\x80\x94  SETUP");
  lv_obj_set_style_text_color(brand, LVC(COL_CLAY), 0);
  lv_obj_set_style_text_font(brand, &lv_font_montserrat_20, 0);
  lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_t *t = lv_label_create(scr);
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(t, SCREEN_W - 40);
  lv_label_set_text(t,
    "1   Join Wi-Fi  " LV_SYMBOL_WIFI "  AI-Usage-Bar-Setup\n"
    "2   Open  192.168.4.1  (if no popup)\n"
    "3   Pick your 2.4GHz Wi-Fi + password\n"
    "4   Enter Mac IP + port, then Save");
  lv_obj_set_style_text_color(t, LVC(COL_INK), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 14);
}

/* ---------------- networking task (Arduino core) ---------------- */
static unsigned long g_lastPoll = 0;
static unsigned long g_last_usb_ms = 0;   // last USB frame (0 = none)

void setup() {
  Serial.setRxBufferSize(2048);   // HWCDC default RX is 256B; USB frames are ~1KB
  Serial.begin(115200);
  Serial.println("\n=== AI-USAGE-BAR (3-screen) boot (" __DATE__ " " __TIME__ ") ===");
  i2c_master_Init();
  lvgl_port_init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  if (aiusage_lvgl_lock(-1)) { ui_setup_screen(); aiusage_lvgl_unlock(); }

  net_begin();   // captive portal on first run (SSID: AI-Usage-Bar-Setup)

  if (aiusage_lvgl_lock(-1)) {
    lv_obj_clean(lv_scr_act());
    ui_build();
    lv_timer_create(render_cb, RENDER_INTERVAL_MS, NULL);
    aiusage_lvgl_unlock();
  }
}

void loop() {
  // USB-serial transport (preferred when frames arrive): the Mac pushes /usage
  // frames over the USB cable; they drive the display even with no Wi-Fi (office).
  UsageState uframe;
  if (net_usb_read(&uframe)) {
    portENTER_CRITICAL(&g_mux);
    g_state = uframe; g_have = true;
    portEXIT_CRITICAL(&g_mux);
    g_last_usb_ms = millis();
    Serial.printf("[usb] frame ok 5h=%d wk=%d\n",
                  uframe.p[PV_CLAUDE].five.util, uframe.p[PV_CLAUDE].seven.util);
  }
  bool usbFresh = g_last_usb_ms != 0 && (millis() - g_last_usb_ms < 30000);

  if (!usbFresh && (g_lastPoll == 0 || millis() - g_lastPoll > POLL_INTERVAL_MS)) {
    g_lastPoll = millis();
    UsageState tmp;
    bool ok = net_fetch(&tmp);
    portENTER_CRITICAL(&g_mux);
    if (ok) { g_state = tmp; g_have = true; }
    else    { g_state.ok = false; strlcpy(g_state.err, tmp.err, sizeof(g_state.err)); }
    portEXIT_CRITICAL(&g_mux);
    if (!ok) Serial.printf("[net] fetch failed: %s\n", tmp.err);
  }

  // Drain the remote action queue OUTSIDE any LVGL lock. A tap in an LVGL
  // callback only sets g_action; the blocking HTTP POST happens here so the
  // render task never stalls on the network.
  if (g_action >= 0) {
    int a = g_action; g_action = -1;
    if (!net_action(ACTION_BODY[a])) Serial.printf("[action] %d failed\n", a);
  }

  // Long-press the AI-USAGE brand -> reopen the setup portal (outside LVGL lock).
  if (g_reprovision) {
    g_reprovision = false;
    Serial.println("[net] reprovision: opening setup portal (join AI-Usage-Bar-Setup)");
    net_portal();
  }

  delay(50);
}
