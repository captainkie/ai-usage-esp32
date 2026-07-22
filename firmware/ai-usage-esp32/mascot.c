// Pixel-companion engine — see mascot.h. Ported 1:1 from design/mockup.html.
#include "mascot.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define GW 24
#define GH 23
#define U  5
#define OYc 2
#define MASC_OY 3
#define EMPTY (-1)

static int32_t G[GH][GW];
static int g_blink = 0, g_frame = 0;

/* ------------------------- colour helpers ------------------------- */
uint16_t mascot_to565(uint32_t rgb) {
  uint8_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static uint32_t mix(uint32_t a, uint32_t b, float t) {
  int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
  int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
  int r = (int)lroundf(ar + (br - ar) * t);
  int g = (int)lroundf(ag + (bg - ag) * t);
  int c = (int)lroundf(ab + (bb - ab) * t);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)c;
}

typedef struct { uint32_t fur, dark, deep, cream, creamSh, pink, inEar; } Pal;
static Pal palette(uint32_t fur) {
  Pal p;
  p.fur = fur;
  p.dark = mix(fur, 0x241119, 0.50f);
  p.deep = mix(fur, 0x120810, 0.82f);
  p.cream = 0xf6ecda; p.creamSh = 0xe0cfb2;
  p.pink = 0xff9fb2;  p.inEar = 0xf0a6bb;
  return p;
}

/* ------------------------- grid stamping ------------------------- */
static void newG(void) { for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) G[y][x] = EMPTY; }
static void gs(int x, int y, uint32_t c) { if (x >= 0 && x < GW && y >= 0 && y < GH) G[y][x] = (int32_t)c; }
static void gclr(int x, int y) { if (x >= 0 && x < GW && y >= 0 && y < GH) G[y][x] = EMPTY; }
static void gr(int x, int y, int w, int h, uint32_t c) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) gs(x + i, y + j, c);
}
static void gtri(int cxp, int ay, int by, int hb, uint32_t c) {
  int hgt = by - ay; if (hgt <= 0) return;
  for (int y = ay; y <= by; y++) {
    int hw = (int)lroundf((float)hb * (y - ay) / hgt);
    for (int x = cxp - hw; x <= cxp + hw; x++) gs(x, y, c);
  }
}

/* ------------------------- shared eyes ------------------------- */
static void faceEyes(int mood, int c, int ey, uint32_t glow) {
  int L = c - 3, R = c + 2; uint32_t dk = 0x160b11;
  int closed = g_blink > 0 && mood != MOOD_CHILL && mood != MOOD_FRIED;
  if (closed) { gr(L, ey, 2, 1, glow); gr(R, ey, 2, 1, glow); return; }
  if (mood == MOOD_FRIED) {
    gs(L, ey - 1, 0xff5d6e); gs(L + 2, ey - 1, 0xff5d6e); gs(L + 1, ey, 0xff5d6e); gs(L, ey + 1, 0xff5d6e); gs(L + 2, ey + 1, 0xff5d6e);
    gs(R - 1, ey - 1, 0xff5d6e); gs(R + 1, ey - 1, 0xff5d6e); gs(R, ey, 0xff5d6e); gs(R - 1, ey + 1, 0xff5d6e); gs(R + 1, ey + 1, 0xff5d6e);
  } else if (mood == MOOD_STRESS) {
    gs(L, ey - 1, glow); gs(L + 1, ey, glow); gs(R, ey, glow); gs(R + 1, ey - 1, glow);
  } else if (mood == MOOD_CHILL) {
    gs(L, ey, glow); gs(L + 1, ey - 1, glow); gs(L + 2, ey, glow);
    gs(R - 1, ey, glow); gs(R, ey - 1, glow); gs(R + 1, ey, glow);
  } else {
    gr(L, ey - 1, 2, 2, dk); gs(L, ey - 1, glow);
    gr(R, ey - 1, 2, 2, dk); gs(R + 1, ey - 1, glow);
  }
}

/* ------------------------- CAT ------------------------- */
static void catFace(Pal p, int mood, int c) {
  int L = c - 3, R = c + 2, ey = 9; uint32_t dk = 0x160b11;
  int closed = g_blink > 0 && mood != MOOD_CHILL && mood != MOOD_FRIED;
  if (mood == MOOD_CHILL) {
    gr(c - 6, 8, 13, 2, 0x14171e); gs(c - 6, 10, 0x14171e); gs(c + 6, 10, 0x14171e);
    gs(c - 5, 8, 0x3b4553); gs(c + 1, 8, 0x3b4553);
    gs(c - 1, 12, 0x6b4436); gs(c, 12, 0x6b4436); gs(c + 1, 12, 0x6b4436); gs(c + 2, 11, 0x6b4436);
  } else if (mood == MOOD_FRIED) {
    gs(L, ey - 1, 0xff5d6e); gs(L + 2, ey - 1, 0xff5d6e); gs(L + 1, ey, 0xff5d6e); gs(L, ey + 1, 0xff5d6e); gs(L + 2, ey + 1, 0xff5d6e);
    gs(R - 1, ey - 1, 0xff5d6e); gs(R + 1, ey - 1, 0xff5d6e); gs(R, ey, 0xff5d6e); gs(R - 1, ey + 1, 0xff5d6e); gs(R + 1, ey + 1, 0xff5d6e);
    gr(c - 2, 12, 5, 2, 0x2a0f10); gr(c - 1, 12, 3, 1, 0xff8a8a);
  } else if (closed) {
    gr(L, ey, 2, 1, 0x241119); gr(R, ey, 2, 1, 0x241119);
  } else if (mood == MOOD_FOCUS) {
    gr(L, ey - 1, 2, 2, dk); gs(L, ey - 1, 0xfff7ea);
    gr(R, ey - 1, 2, 2, dk); gs(R + 1, ey - 1, 0xfff7ea);
    gs(c, 12, 0x6b4436);
  } else if (mood == MOOD_SWEAT) {
    gr(L, ey - 1, 2, 2, dk); gr(R, ey - 1, 2, 2, dk);
    gr(c - 1, 12, 3, 2, 0x3a1e18);
  } else { /* stress */
    gs(L, ey - 1, dk); gs(L + 1, ey, dk); gs(R, ey, dk); gs(R + 1, ey - 1, dk);
    int xs[5] = { c - 2, c - 1, c, c + 1, c + 2 };
    for (int i = 0; i < 5; i++) gs(xs[i], 12, (i % 2) ? 0x8a5140 : 0x3a1e18);
  }
  if (mood != MOOD_CHILL) { gs(c - 1, 11, p.pink); gs(c, 11, p.pink); }
}
static Pal buildCat(uint32_t fur, int mood) {
  newG(); Pal p = palette(fur); int c = 12;
  int tail[7][2] = {{19,16},{20,15},{20,14},{19,13},{18,13},{20,16},{20,17}};
  for (int i = 0; i < 7; i++) gs(tail[i][0], tail[i][1], p.dark);
  for (int y = 14; y <= 21; y++) { int hw = (int)lroundf(4 + (y - 14) / 7.0f * 4); gr(c - hw, y, hw * 2 + 1, 1, p.fur); }
  for (int y = 16; y <= 21; y++) { int hw = (int)lroundf(2 + (y - 16) / 5.0f * 2); gr(c - hw, y, hw * 2 + 1, 1, p.cream); }
  int st[3] = {15,17,19}; for (int i = 0; i < 3; i++) { gs(c - 4, st[i], p.dark); gs(c + 4, st[i], p.dark); }
  gr(c - 6, 20, 3, 2, p.cream); gr(c + 4, 20, 3, 2, p.cream);
  gtri(c - 5, 2, 7, 3, p.fur); gtri(c + 5, 2, 7, 3, p.fur);
  gtri(c - 5, 4, 7, 1, p.inEar); gtri(c + 5, 4, 7, 1, p.inEar);
  gr(c - 7, 6, 15, 8, p.fur); gclr(c - 7, 6); gclr(c + 7, 6); gclr(c - 7, 13); gclr(c + 7, 13);
  gr(c - 4, 11, 9, 3, p.cream);
  int m[8][2] = {{c-3,7},{c-2,7},{c,7},{c+2,7},{c+3,7},{c-1,8},{c+1,8},{c,8}};
  for (int i = 0; i < 8; i++) gs(m[i][0], m[i][1], p.dark);
  for (int y = 7; y <= 12; y++) { gs(c - 7, y, p.dark); gs(c + 7, y, p.dark); }
  catFace(p, mood, c);
  return p;
}

/* ------------------------- ROBOT ------------------------- */
static void robotEyes(int mood, int c, uint32_t glow) {
  int ey = 7;
  int closed = g_blink > 0 && mood != MOOD_CHILL && mood != MOOD_FRIED;
  if (closed) { gr(c - 4, ey, 2, 1, glow); gr(c + 2, ey, 2, 1, glow); return; }
  if (mood == MOOD_CHILL) {
    gs(c - 4, ey, glow); gs(c - 3, ey - 1, glow); gs(c - 2, ey, glow);
    gs(c + 2, ey, glow); gs(c + 3, ey - 1, glow); gs(c + 4, ey, glow);
  } else if (mood == MOOD_FRIED) {
    gs(c - 4, ey - 1, 0xff5d6e); gs(c - 2, ey - 1, 0xff5d6e); gs(c - 3, ey, 0xff5d6e); gs(c - 4, ey + 1, 0xff5d6e); gs(c - 2, ey + 1, 0xff5d6e);
    gs(c + 2, ey - 1, 0xff5d6e); gs(c + 4, ey - 1, 0xff5d6e); gs(c + 3, ey, 0xff5d6e); gs(c + 2, ey + 1, 0xff5d6e); gs(c + 4, ey + 1, 0xff5d6e);
  } else if (mood == MOOD_STRESS) {
    gs(c - 4, ey - 1, glow); gs(c - 3, ey, glow); gs(c + 3, ey, glow); gs(c + 4, ey - 1, glow);
  } else if (mood == MOOD_SWEAT) {
    gr(c - 4, ey, 2, 1, glow); gr(c + 2, ey, 2, 1, glow);
  } else {
    gr(c - 4, ey - 1, 2, 2, glow); gr(c + 2, ey - 1, 2, 2, glow);
  }
}
static Pal buildRobot(uint32_t fur, int mood) {
  newG(); int c = 12; Pal p = palette(fur);
  uint32_t metal = 0xcdd5e2, metalSh = 0x868ea1, visor = 0x0d1119;
  uint32_t glow = mix(fur, 0xffffff, 0.5f);
  gs(c, 0, mood == MOOD_CHILL ? 0x7dffd0 : glow); gs(c, 1, metalSh);
  gr(c - 6, 3, 13, 8, p.fur);
  gclr(c - 6, 3); gclr(c + 6, 3); gclr(c - 6, 10); gclr(c + 6, 10);
  gr(c - 4, 3, 9, 1, mix(fur, 0xffffff, 0.26f));
  for (int y = 4; y <= 9; y++) { gs(c - 6, y, p.dark); gs(c + 6, y, p.dark); }
  gr(c - 8, 6, 1, 3, metalSh); gr(c + 8, 6, 1, 3, metalSh);
  gs(c - 8, 7, metal); gs(c + 8, 7, metal);
  gr(c - 5, 5, 11, 4, visor); gr(c - 6, 6, 13, 2, visor);
  robotEyes(mood, c, glow);
  gr(c - 2, 11, 5, 1, metalSh);
  gr(c - 4, 12, 9, 6, p.fur);
  gclr(c - 4, 12); gclr(c + 4, 12); gclr(c - 4, 17); gclr(c + 4, 17);
  gr(c - 4, 12, 9, 1, mix(fur, 0xffffff, 0.2f));
  for (int y = 13; y <= 16; y++) { gs(c - 4, y, p.dark); gs(c + 4, y, p.dark); }
  uint32_t led = mood == MOOD_FRIED ? 0xff5d6e : mood == MOOD_STRESS ? 0xffb020 : mood == MOOD_SWEAT ? 0xffd24b : mood == MOOD_FOCUS ? 0x7fd7ff : 0x37e0a1;
  gr(c - 1, 14, 3, 3, visor); gs(c, 15, led); gs(c - 1, 15, mix(led, 0x000000, 0.35f)); gs(c + 1, 15, mix(led, 0x000000, 0.35f));
  gr(c - 6, 13, 2, 3, metalSh); gr(c + 5, 13, 2, 3, metalSh); gs(c - 6, 16, metal); gs(c + 6, 16, metal);
  gr(c - 3, 18, 3, 2, metalSh); gr(c + 1, 18, 3, 2, metalSh);
  return p;
}

/* ------------------------- HUSKY ------------------------- */
static Pal buildHusky(uint32_t fur, int mood) {
  newG(); Pal p = palette(fur); int c = 12; uint32_t eyeC = 0x5ec8ff;
  int tl[5][2] = {{18,12},{19,11},{20,11},{20,12},{19,13}};
  for (int i = 0; i < 5; i++) gs(tl[i][0], tl[i][1], p.fur);
  gs(19, 11, p.cream); gs(20, 11, p.cream);
  for (int y = 13; y <= 21; y++) { int hw = (int)lroundf(4 + (y - 13) / 8.0f * 4); gr(c - hw, y, hw * 2 + 1, 1, p.fur); }
  for (int y = 15; y <= 21; y++) { int hw = (int)lroundf(2 + (y - 15) / 6.0f * 2); gr(c - hw, y, hw * 2 + 1, 1, p.cream); }
  gr(c - 6, 20, 3, 2, p.cream); gr(c + 4, 20, 3, 2, p.cream);
  gtri(c - 6, 1, 7, 2, p.fur); gtri(c + 6, 1, 7, 2, p.fur);
  gtri(c - 6, 4, 7, 1, p.pink); gtri(c + 6, 4, 7, 1, p.pink);
  gtri(c - 6, 1, 4, 1, p.dark); gtri(c + 6, 1, 4, 1, p.dark);
  gr(c - 6, 5, 13, 7, p.fur); gclr(c - 6, 5); gclr(c + 6, 5);
  gr(c - 2, 5, 5, 4, p.cream);
  gr(c - 4, 9, 9, 3, p.cream);
  gr(c - 6, 6, 2, 2, p.dark); gr(c + 5, 6, 2, 2, p.dark);
  faceEyes(mood, c, 7, eyeC);
  gs(c, 10, 0x20140f); gs(c - 1, 10, 0x20140f);
  if (mood == MOOD_CHILL) { gr(c - 1, 12, 3, 1, 0xff8fa3); gs(c, 13, 0xff6f88); }
  return p;
}

/* ------------------------- OWL ------------------------- */
static void owlEyes(int mood, int c, int ex, int ey) {
  int closed = g_blink > 0 && mood != MOOD_FRIED;
  int signs[2] = { -1, 1 };
  for (int k = 0; k < 2; k++) {
    int e = c + signs[k] * ex;
    if (closed) { gr(e - 1, ey, 2, 1, 0x3a2a10); continue; }
    if (mood == MOOD_FRIED) { gs(e - 1, ey - 1, 0xff5d6e); gs(e + 1, ey - 1, 0xff5d6e); gs(e, ey, 0xff5d6e); gs(e - 1, ey + 1, 0xff5d6e); gs(e + 1, ey + 1, 0xff5d6e); continue; }
    if (mood == MOOD_STRESS) { gr(e - 1, ey - 1, 3, 3, 0x20140a); gs(e, ey, 0xffd24b); continue; }
    gr(e - 1, ey - 1, 3, 3, 0x20140a); gs(e, ey, 0x5ec8ff); gs(e - 1, ey - 1, 0xffffff);
    if (mood == MOOD_CHILL) gr(e - 1, ey - 1, 3, 1, 0xf6ecda);
  }
}
static Pal buildOwl(uint32_t fur, int mood) {
  newG(); Pal p = palette(fur); int c = 12;
  for (int y = 6; y <= 21; y++) { float dy = (y - 13.5f) / 8.0f; int hw = (int)lroundf(8 * sqrtf(fmaxf(0, 1 - dy * dy))); if (hw > 0) gr(c - hw, y, hw * 2 + 1, 1, p.fur); }
  for (int y = 11; y <= 20; y++) { float dy = (y - 15.0f) / 6.0f; int hw = (int)lroundf(4 * sqrtf(fmaxf(0, 1 - dy * dy))); if (hw > 0) gr(c - hw, y, hw * 2 + 1, 1, p.cream); }
  gtri(c - 6, 3, 6, 1, p.fur); gtri(c + 6, 3, 6, 1, p.fur);
  for (int y = 9; y <= 18; y++) { gs(c - 8, y, p.dark); gs(c + 8, y, p.dark); }
  gs(c - 7, 18, p.dark); gs(c + 7, 18, p.dark);
  int ex = 4, ey = 9, signs[2] = { -1, 1 };
  for (int k = 0; k < 2; k++) { int e = c + signs[k] * ex; gr(e - 2, ey - 2, 4, 4, 0xf6ecda); gclr(e - 2, ey - 2); gclr(e + 1, ey - 2); gclr(e - 2, ey + 1); gclr(e + 1, ey + 1); }
  owlEyes(mood, c, ex, ey);
  gs(c, 11, 0xf4a53a); gs(c - 1, 12, 0xe8902a); gs(c, 12, 0xe8902a); gs(c + 1, 12, 0xf4a53a); gs(c, 13, 0xd97e20);
  gs(c - 2, 21, 0xf4a53a); gs(c - 1, 21, 0xf4a53a); gs(c + 1, 21, 0xf4a53a); gs(c + 2, 21, 0xf4a53a);
  return p;
}

/* ------------------------- TERMINATOR ------------------------- */
static Pal buildTerm(uint32_t fur, int mood, uint32_t t) {
  newG(); int c = 12;
  uint32_t metal = mix(fur, 0xc8ccd6, 0.60f), sh = mix(fur, 0x565b67, 0.5f), deepm = mix(fur, 0x2a2d35, 0.55f);
  Pal p = { metal, sh, 0x171922, metal, sh, 0xff5d6e, sh };
  for (int y = 3; y <= 10; y++) { float dy = (y - 7.0f) / 5.0f; int hw = (int)lroundf(7 * sqrtf(fmaxf(0, 1 - dy * dy))); if (hw > 0) gr(c - hw, y, hw * 2 + 1, 1, metal); }
  gr(c - 6, 8, 13, 1, sh);
  gr(c - 5, 7, 3, 2, 0x0a0b0f); gr(c + 3, 7, 3, 2, 0x0a0b0f);
  uint32_t fl = ((t / 120) % 5 == 0) ? 0xff9a9a : 0xff2a2a;
  gs(c - 4, 7, fl); gs(c - 4, 8, 0x8a0000);
  gs(c + 4, 7, mood == MOOD_FRIED ? 0x360000 : 0xff2a2a); gs(c + 4, 8, 0x7a0000);
  gs(c - 6, 10, sh); gs(c + 6, 10, sh);
  gr(c - 5, 11, 11, 1, metal);
  gr(c - 5, 12, 11, 2, 0x0a0b0f);
  for (int x = c - 5; x <= c + 5; x += 2) gs(x, 12, metal);
  for (int x = c - 4; x <= c + 4; x += 2) gs(x, 13, metal);
  gr(c - 4, 14, 9, 1, metal);
  gr(c - 1, 15, 3, 2, sh); gs(c, 16, metal);
  gr(c - 5, 17, 11, 2, deepm); gr(c - 6, 18, 13, 1, metal); gr(c - 6, 19, 13, 1, sh);
  return p;
}

/* ------------------------- RUNNER (Arnold / T-800 in leather + shades) ------- */
static Pal buildRunner(uint32_t fur, int mood, uint32_t t) {
  newG(); int c = 12; Pal p = palette(fur);
  uint32_t skin = 0xe6ac82, hair = 0x211a13;
  uint32_t leather = mix(fur, 0x241d2b, 0.42f);          // provider-tinted black leather
  uint32_t lapel = fur;                                   // provider-colour trim
  uint32_t gun = 0x484c56, gunDk = 0x191b22, wood = 0x6b3f22;
  uint32_t pants = 0x242736, boot = 0x14141b;
  int stride = ((t / 160) % 2 == 0);
  int y0 = 2;

  // hair
  gr(c - 2, y0, 5, 2, hair); gr(c - 2, y0, 5, 1, mix(hair, 0x000000, 0.4f));
  // head + jaw
  gr(c - 2, y0 + 2, 5, 3, skin); gclr(c - 2, y0 + 2);
  // sunglasses (always — the cool shades)
  gr(c - 2, y0 + 3, 5, 1, 0x0d0f15); gs(c - 1, y0 + 3, 0x2f3644);
  // stern mouth
  gs(c, y0 + 4, mood == MOOD_FRIED ? 0xff5d6e : 0x6a4636);
  // leather jacket
  gr(c - 3, y0 + 5, 6, 5, leather);
  gr(c - 3, y0 + 5, 6, 1, mix(leather, 0xffffff, 0.16f));   // collar sheen
  gs(c - 3, y0 + 6, lapel); gs(c + 2, y0 + 6, lapel);        // provider-colour lapels
  gs(c, y0 + 6, mix(leather, 0x000000, 0.5f)); gs(c, y0 + 7, mix(leather, 0x000000, 0.5f)); // zipper
  // shotgun held forward
  int gy = y0 + 7;
  gr(c + 3, gy, 5, 1, gun); gr(c + 3, gy, 2, 1, gunDk); gs(c + 7, gy, gunDk);   // barrel
  gr(c + 3, gy + 1, 2, 1, wood);                                                 // fore-grip
  gr(c + 2, gy - 1, 2, 2, leather);                                              // front arm to gun
  gr(c - 4, y0 + 6, 2, 2, leather);                                             // back arm
  // belt
  gr(c - 3, y0 + 10, 6, 1, 0x120f16);
  // legs (striding) + boots
  int ly = y0 + 11;
  if (stride) {
    gr(c - 2, ly, 2, 3, pants); gr(c - 3, ly + 2, 2, 1, boot);
    gr(c + 1, ly, 2, 3, pants); gr(c + 1, ly + 2, 2, 1, boot);
  } else {
    gr(c - 2, ly, 2, 3, pants); gr(c - 2, ly + 2, 2, 1, boot);
    gr(c, ly, 2, 3, pants); gr(c + 2, ly + 1, 2, 2, pants); gr(c + 3, ly + 2, 2, 1, boot);
  }
  // motion streaks behind
  gs(c - 5, y0 + 6, mix(fur, 0xffffff, 0.35f)); gs(c - 6, y0 + 8, mix(fur, 0xffffff, 0.28f));
  return p;
}

/* ------------------------- DINO ------------------------- */
static Pal buildDino(uint32_t fur, int mood, uint32_t t) {
  newG(); int c = 12; Pal p = palette(fur); (void)c;
  int tail[4][2] = {{3,14},{4,14},{2,15},{5,13}};
  for (int i = 0; i < 4; i++) gs(tail[i][0], tail[i][1], p.fur);
  gr(6, 11, 8, 5, p.fur);
  gr(13, 7, 6, 5, p.fur); gr(14, 7, 5, 1, mix(fur, 0xffffff, 0.2f));
  gr(18, 10, 2, 2, p.fur);
  if (mood == MOOD_FRIED) { gs(15, 8, 0xff5d6e); gs(16, 9, 0xff5d6e); }
  else if (g_blink > 0) { gs(15, 8, 0x20140f); }
  else { gr(15, 8, 2, 1, 0x20140f); gs(15, 8, 0xffffff); }
  gr(15, 11, 5, 1, (mood == MOOD_STRESS || mood == MOOD_FRIED) ? 0x3a1010 : 0x2a1a10);
  gr(13, 12, 2, 1, p.dark);
  int bl[3] = {12,13,14}; for (int i = 0; i < 3; i++) gs(7, bl[i], mix(fur, 0xffffff, 0.18f));
  int stride = ((t / 150) % 2 == 0);
  if (stride) { gr(7, 16, 2, 3, p.fur); gr(11, 16, 2, 2, p.fur); gs(11, 18, p.dark); gs(7, 19, p.dark); }
  else { gr(8, 16, 2, 2, p.fur); gr(10, 16, 2, 3, p.fur); gs(8, 18, p.dark); gs(10, 19, p.dark); }
  return p;
}

/* ------------------------- blit helpers ------------------------- */
static void fillRect(uint16_t *buf, int bw, int bh, int x, int y, int w, int h, uint16_t col) {
  for (int j = y; j < y + h; j++) {
    if (j < 0 || j >= bh) continue;
    for (int i = x; i < x + w; i++) { if (i < 0 || i >= bw) continue; buf[j * bw + i] = col; }
  }
}
static void lineThin(uint16_t *buf, int bw, int bh, int x0, int y0, int x1, int y1, uint16_t col) {
  int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
  for (;;) {
    if (x0 >= 0 && x0 < bw && y0 >= 0 && y0 < bh) buf[y0 * bw + x0] = col;
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

/* ------------------------- public ------------------------- */
int mascot_mood_for(int u) {
  if (u >= 98) return MOOD_FRIED;
  if (u >= 90) return MOOD_STRESS;
  if (u >= 75) return MOOD_SWEAT;
  if (u >= 50) return MOOD_FOCUS;
  return MOOD_CHILL;
}

void mascot_render(uint16_t *buf, int bw, int bh, int kind, uint32_t fur, int mood, uint32_t t, uint16_t bg565) {
  g_frame++;
  if (g_frame % 44 == 0) g_blink = 4; else if (g_blink > 0) g_blink--;

  uint32_t heat = mood == MOOD_FRIED ? mix(fur, 0xff5344, 0.30f)
                : mood == MOOD_STRESS ? mix(fur, 0xff5344, 0.15f) : fur;
  Pal p;
  switch (kind) {
    case MASC_ROBOT:  p = buildRobot(heat, mood); break;
    case MASC_HUSKY:  p = buildHusky(heat, mood); break;
    case MASC_OWL:    p = buildOwl(heat, mood); break;
    case MASC_TERM:   p = buildTerm(heat, mood, t); break;
    case MASC_RUNNER: p = buildRunner(heat, mood, t); break;
    case MASC_DINO:   p = buildDino(heat, mood, t); break;
    default:          p = buildCat(heat, mood); break;
  }

  for (int i = 0; i < bw * bh; i++) buf[i] = bg565;
  int bob = (int)lroundf(sinf(t / 560.0f) * 2.0f);

  uint16_t deep = mascot_to565(p.deep);
  for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) {
    if (G[y][x] != EMPTY) continue;
    int up = y > 0 && G[y - 1][x] != EMPTY, dn = y < GH - 1 && G[y + 1][x] != EMPTY;
    int lf = x > 0 && G[y][x - 1] != EMPTY, rt = x < GW - 1 && G[y][x + 1] != EMPTY;
    if (up || dn || lf || rt) fillRect(buf, bw, bh, x * U, MASC_OY + bob + y * U, U, U, deep);
  }
  for (int y = 0; y < GH; y++) for (int x = 0; x < GW; x++) {
    if (G[y][x] == EMPTY) continue;
    fillRect(buf, bw, bh, x * U, MASC_OY + bob + y * U, U, U, mascot_to565((uint32_t)G[y][x]));
  }

  if (kind == MASC_CAT) {
    uint16_t wc = mascot_to565(0xf6ecda);
    int oy = MASC_OY + bob;
    lineThin(buf, bw, bh, (int)(5.6f * U), oy + (int)(10.3f * U), (int)(1.4f * U), oy + (int)(9.4f * U), wc);
    lineThin(buf, bw, bh, (int)(5.6f * U), oy + (int)(11.2f * U), (int)(1.4f * U), oy + (int)(11.9f * U), wc);
    lineThin(buf, bw, bh, (int)(18.4f * U), oy + (int)(10.3f * U), (int)(22.6f * U), oy + (int)(9.4f * U), wc);
    lineThin(buf, bw, bh, (int)(18.4f * U), oy + (int)(11.2f * U), (int)(22.6f * U), oy + (int)(11.9f * U), wc);
  }

  int organic = (kind == MASC_CAT || kind == MASC_HUSKY || kind == MASC_OWL || kind == MASC_ROBOT || kind == MASC_RUNNER);
  if ((mood == MOOD_SWEAT || mood == MOOD_STRESS) && organic) {
    int off = (int)((t / 240 + (mood == MOOD_STRESS ? 1 : 0)) % 4);
    int sx = (int)(18.6f * U), sy = MASC_OY + bob + (9 + off) * U;
    fillRect(buf, bw, bh, sx, sy, U, (int)(U * 1.2f), mascot_to565(0x7fd0ff));
    fillRect(buf, bw, bh, sx, sy, U, (int)(U * 0.4f), mascot_to565(0xc9ecff));
  }
  if (mood == MOOD_FRIED) {
    int s = (int)((t / 200) % 3);
    fillRect(buf, bw, bh, (8 + s) * U, MASC_OY + bob + 1 * U, (int)(U * 0.8f), (int)(U * 0.8f), mascot_to565(0xffab8a));
    fillRect(buf, bw, bh, (15 - s) * U, MASC_OY + bob + 0 * U, (int)(U * 0.8f), (int)(U * 0.8f), mascot_to565(0xffc8a8));
  }
}
