// Host harness: render the ported C mascot engine to a PPM so we can eyeball
// that firmware/ai-usage-esp32/mascot.c matches design/mockup.html.
//   cc -O2 mascot_ctest.c ../../firmware/ai-usage-esp32/mascot.c -lm -o /tmp/mctest && /tmp/mctest > /tmp/mascots.ppm
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/ai-usage-esp32/mascot.h"

#define TILE 120
#define GAP  8
#define CELL (TILE + GAP)

static void put565(uint8_t *rgb, int W, int x, int y, uint16_t c) {
  uint8_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  uint8_t *px = rgb + (y * W + x) * 3;
  px[0] = (r << 3) | (r >> 2); px[1] = (g << 2) | (g >> 4); px[2] = (b << 3) | (b >> 2);
}

int main(void) {
  const uint32_t bg = 0x0c0e16;
  const uint16_t bg565 = mascot_to565(bg);

  struct { int kind; uint32_t fur; } comp[] = {
    { MASC_CAT, 0xe08040 }, { MASC_ROBOT, 0xe08040 }, { MASC_HUSKY, 0x8892a6 },
    { MASC_OWL, 0xb07a3a }, { MASC_TERM, 0x7f8694 }, { MASC_RUNNER, 0x12b886 },
    { MASC_DINO, 0x4285f4 },
  };
  int nComp = 7;
  int moods[5] = { MOOD_CHILL, MOOD_FOCUS, MOOD_SWEAT, MOOD_STRESS, MOOD_FRIED };

  int cols = nComp;                 // row 0: companions ; row 1: cat moods
  int W = cols * CELL, H = 2 * CELL;
  uint8_t *img = malloc((size_t)W * H * 3);
  for (int i = 0; i < W * H; i++) { img[i * 3] = 0x14; img[i * 3 + 1] = 0x16; img[i * 3 + 2] = 0x1e; }

  uint16_t tile[TILE * TILE];

  // row 0 — all companions at FOCUS
  for (int i = 0; i < nComp; i++) {
    mascot_render(tile, TILE, TILE, comp[i].kind, comp[i].fur, MOOD_FOCUS, 60, bg565);
    int ox = i * CELL, oy = 0;
    for (int y = 0; y < TILE; y++) for (int x = 0; x < TILE; x++) put565(img, W, ox + x, oy + y, tile[y * TILE + x]);
  }
  // row 1 — cat across the 5 moods
  for (int i = 0; i < 5; i++) {
    mascot_render(tile, TILE, TILE, MASC_CAT, 0xe08040, moods[i], 60, bg565);
    int ox = i * CELL, oy = CELL;
    for (int y = 0; y < TILE; y++) for (int x = 0; x < TILE; x++) put565(img, W, ox + x, oy + y, tile[y * TILE + x]);
  }

  printf("P6\n%d %d\n255\n", W, H);
  fwrite(img, 1, (size_t)W * H * 3, stdout);
  free(img);
  return 0;
}
