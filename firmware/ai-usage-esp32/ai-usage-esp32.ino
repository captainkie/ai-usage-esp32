/*
 * AI Usage Bar — ESP32 edition
 * Waveshare ESP32-S3-Touch-LCD-3.49 (640×172, AXS15231B QSPI + capacitive touch)
 *
 * Shows your Claude Code 5-hour / weekly limits as twin arc gauges with a live
 * reset countdown, the current model + effort, touch-switchable providers, and
 * a pixel companion (cat / robot / husky / owl / T-800 / runner / dino) whose
 * colour follows the provider and whose mood follows the busiest gauge.
 *
 * Data comes from the Mac "bridge" (../bridge) over your LAN — your token never
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

static float curFive = 0, curSeven = 0;   // eased gauge values
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

/* ---------------- build UI (call under the LVGL lock) ---------------- */
static void ui_build() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scr, LVC(COL_BG), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // top strip
  lblBrand = lv_label_create(scr);
  lv_label_set_text(lblBrand, "AI\xC2\xB7USAGE");
  lv_obj_set_style_text_color(lblBrand, LVC(COL_CLAY), 0);
  lv_obj_set_style_text_font(lblBrand, &lv_font_montserrat_14, 0);
  lv_obj_set_pos(lblBrand, 12, 9);

  lblModel = lv_label_create(scr);
  lv_label_set_text(lblModel, "—");
  lv_obj_set_style_text_color(lblModel, LVC(COL_INK), 0);
  lv_obj_set_style_text_font(lblModel, &lv_font_montserrat_20, 0);
  lv_obj_align(lblModel, LV_ALIGN_TOP_MID, 0, 6);

  lblEffort = lv_label_create(scr);
  lv_label_set_text(lblEffort, "");
  lv_obj_set_style_text_color(lblEffort, LVC(0xAEB4C6), 0);
  lv_obj_set_style_text_font(lblEffort, &lv_font_montserrat_14, 0);
  lv_obj_set_style_bg_color(lblEffort, LVC(0x1A1D29), 0);
  lv_obj_set_style_bg_opa(lblEffort, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_hor(lblEffort, 6, 0);
  lv_obj_set_style_pad_ver(lblEffort, 2, 0);
  lv_obj_set_style_radius(lblEffort, 5, 0);
  lv_obj_align(lblEffort, LV_ALIGN_TOP_RIGHT, -70, 8);

  lblLive = lv_label_create(scr);
  lv_label_set_text(lblLive, LV_SYMBOL_WIFI " LIVE");
  lv_obj_set_style_text_color(lblLive, LVC(COL_LIVE), 0);
  lv_obj_set_style_text_font(lblLive, &lv_font_montserrat_14, 0);
  lv_obj_align(lblLive, LV_ALIGN_TOP_RIGHT, -8, 9);

  // gauges — default lv_arc is a 270° meter with the gap at the bottom
  const int R = 44;
  int gx[2] = { 120, 520 };
  lv_obj_t **arcs[2] = { &arcFive, &arcSeven };
  for (int i = 0; i < 2; i++) {
    lv_obj_t *a = lv_arc_create(scr);
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
    lv_obj_t *lab = lv_label_create(scr);
    lv_label_set_text(lab, G[i].text);
    lv_obj_set_style_text_color(lab, LVC(COL_DIM), 0);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
    lv_obj_align(lab, LV_ALIGN_TOP_MID, G[i].cx - 320, 34);
    *G[i].lab = lab;

    lv_obj_t *pct = lv_label_create(scr);
    lv_label_set_text(pct, "—");
    lv_obj_set_style_text_color(pct, LVC(COL_GOOD), 0);
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_40, 0);
    lv_obj_align(pct, LV_ALIGN_LEFT_MID, G[i].cx - 30, 0);
    *G[i].pct = pct;

    lv_obj_t *rst = lv_label_create(scr);
    lv_label_set_text(rst, "");
    lv_obj_set_style_text_color(rst, LVC(0x9AA0B2), 0);
    lv_obj_set_style_text_font(rst, &lv_font_montserrat_14, 0);
    lv_obj_align(rst, LV_ALIGN_TOP_MID, G[i].cx - 320, 145);
    *G[i].rst = rst;
  }

  // mascot canvas (120×120 buffer, RGB565), centred
  mascotBuf = (lv_color_t *)heap_caps_malloc(120 * 120 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  canvasMascot = lv_canvas_create(scr);
  lv_canvas_set_buffer(canvasMascot, mascotBuf, 120, 120, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(canvasMascot, 320 - 60, 97 - 60);
  lv_obj_add_flag(canvasMascot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(canvasMascot, mascot_cb, LV_EVENT_CLICKED, NULL);

  // provider pills
  for (int i = 0; i < PV_COUNT; i++) {
    lv_obj_t *p = lv_btn_create(scr);
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
  else               lv_label_set_text(lblFivePct, "—");
  if (tSeven >= 0) { snprintf(buf, sizeof(buf), "%d%%", tSeven); lv_label_set_text(lblSevenPct, buf); }
  else               lv_label_set_text(lblSevenPct, "—");
  lv_obj_set_style_text_color(lblFivePct,  LVC(util_color(tFive)),  0);
  lv_obj_set_style_text_color(lblSevenPct, LVC(util_color(tSeven)), 0);

  // model + effort
  if (have && pr->model[0])      lv_label_set_text(lblModel, pr->model);
  else if (have && !pr->linked)  lv_label_set_text(lblModel, "not linked");
  else if (have)                 lv_label_set_text(lblModel, "no live data");
  else                           lv_label_set_text(lblModel, "connecting…");
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
}

/* ---------------- networking task (Arduino core) ---------------- */
static unsigned long g_lastPoll = 0;

void setup() {
  Serial.begin(115200);
  i2c_master_Init();
  lvgl_port_init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

  net_begin();   // captive portal on first run (SSID: AI-Usage-Bar-Setup)

  if (aiusage_lvgl_lock(-1)) {
    ui_build();
    lv_timer_create(render_cb, RENDER_INTERVAL_MS, NULL);
    aiusage_lvgl_unlock();
  }
}

void loop() {
  if (g_lastPoll == 0 || millis() - g_lastPoll > POLL_INTERVAL_MS) {
    g_lastPoll = millis();
    UsageState tmp;
    bool ok = net_fetch(&tmp);
    portENTER_CRITICAL(&g_mux);
    if (ok) { g_state = tmp; g_have = true; }
    else    { g_state.ok = false; strlcpy(g_state.err, tmp.err, sizeof(g_state.err)); }
    portEXIT_CRITICAL(&g_mux);
    if (!ok) Serial.printf("[net] fetch failed: %s\n", tmp.err);
  }
  delay(50);
}
