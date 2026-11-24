// 3.1-dev.25: see ui_prims.h (moved from the .ino unchanged).
#include <math.h>
#include "ui_prims.h"

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

void uiLine(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t c, int w) {
  if (!layerSees(layer, imin(x1, x2) - w, imin(y1, y2) - w, imax(x1, x2) + w, imax(y1, y2) + w)) return;
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.p1.x = x1; d.p1.y = y1;
  d.p2.x = x2; d.p2.y = y2;
  d.color = c;
  d.width = w;
  d.round_start = 1;
  d.round_end = 1;
  lv_draw_line(layer, &d);
}

void uiDisc(lv_layer_t *layer, int cx, int cy, int r, lv_color_t c) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = c;
  d.bg_opa = LV_OPA_COVER;
  d.radius = LV_RADIUS_CIRCLE;
  lv_area_t a = { (int32_t)(cx - r), (int32_t)(cy - r), (int32_t)(cx + r), (int32_t)(cy + r) };
  lv_draw_rect(layer, &d, &a);
}

int ui_insignia_gray = -1;
void uiDrawInsignia(lv_layer_t *layer, int faction, int cx, int cy, int r, uint32_t bg) {
  if (faction < 0 || faction > 3) return;
  lv_color_t c = ui_insignia_gray >= 0 ? lv_color_make(ui_insignia_gray, ui_insignia_gray, ui_insignia_gray)
                                     : lv_color_hex(FACTION_RING_COLOR[faction]);
  const int w = 2;
  if (faction == 0) {  // Rebel sunburst
    uiDisc(layer, cx, cy, r * 36 / 100, c);
    for (int k = 0; k < 8; k++) {
      float a = k * 0.7854f;
      int x1 = cx + (int)(cosf(a) * r * 0.58f), y1 = cy + (int)(sinf(a) * r * 0.58f);
      int x2 = cx + (int)(cosf(a) * r * 0.95f), y2 = cy + (int)(sinf(a) * r * 0.95f);
      uiLine(layer, x1, y1, x2, y2, c, w);
    }
  } else if (faction == 1) {  // Imperial hexagon
    int px[6], py[6];
    for (int k = 0; k < 6; k++) {
      float a = -1.5708f + k * 1.0472f;
      px[k] = cx + (int)(cosf(a) * r * 0.9f);
      py[k] = cy + (int)(sinf(a) * r * 0.9f);
    }
    for (int k = 0; k < 6; k++) uiLine(layer, px[k], py[k], px[(k + 1) % 6], py[(k + 1) % 6], c, w);
    uiDisc(layer, cx, cy, r * 34 / 100, c);
  } else if (faction == 2) {  // Pyke diamond
    int t = r * 92 / 100;
    uiLine(layer, cx, cy - t, cx + t, cy, c, w);
    uiLine(layer, cx + t, cy, cx, cy + t, c, w);
    uiLine(layer, cx, cy + t, cx - t, cy, c, w);
    uiLine(layer, cx - t, cy, cx, cy - t, c, w);
    uiDisc(layer, cx, cy, r * 30 / 100, c);
  } else {  // Crimson Dawn crescent: a disc, bitten by a background disc
    uiDisc(layer, cx, cy, r * 82 / 100, c);
    uiDisc(layer, cx + r * 38 / 100, cy - r * 8 / 100, r * 66 / 100, lv_color_hex(bg));
  }
}
