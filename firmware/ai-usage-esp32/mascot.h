// Pixel-companion engine for the AI Usage Bar ESP32 display.
//
// Pure C (RGB565 out) so it (a) drops into the Arduino sketch and (b) can be
// compiled + rendered on a host to verify it matches design/mockup.html.
//
// Ported 1:1 from the mockup's JS drawing engine (24x23 grid, 5px cells,
// outline pass + fills). Fur colour follows the active provider; expression
// follows the busiest gauge.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { MASC_CAT = 0, MASC_ROBOT, MASC_HUSKY, MASC_OWL, MASC_TERM, MASC_RUNNER, MASC_DINO, MASC_COUNT };
enum { MOOD_CHILL = 0, MOOD_FOCUS, MOOD_SWEAT, MOOD_STRESS, MOOD_FRIED };

// Grid is 24x23 cells of 5px -> art fits in a 120x120 canvas.
#define MASCOT_CANVAS 120

// util (0..100) of the busiest window -> mood.
int mascot_mood_for(int util);

// Render companion `kind` into a bw*bh RGB565 buffer (row-major).
//   furRGB : 0xRRGGBB shell/fur colour (per provider)
//   mood   : MOOD_*
//   t      : millis-ish time for idle animation (blink/bob/stride/steam)
//   bg565  : background fill (the dark screen behind the mascot)
void mascot_render(uint16_t *buf, int bw, int bh, int kind,
                   uint32_t furRGB, int mood, uint32_t t, uint16_t bg565);

// 0xRRGGBB -> RGB565 (handy for callers)
uint16_t mascot_to565(uint32_t rgb);

#ifdef __cplusplus
}
#endif
