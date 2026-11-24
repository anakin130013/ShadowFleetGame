// 3.1-dev.25 THE TABLE'S SCREENS -- see table_scenes.h. Layouts are the canvas boards
// (https://claude.ai/artifact/7fLBrkn6uC5bPzA7ESFgWW) on the 466 px face, center (233, 233):
// rim 222, gates 190, symbols 158, numbers ~120, the phase ~98, the middle. Text is a size up from
// the boards everywhere (user: "the mockups almost exactly ... with bigger text all around").
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>
#include "table_scenes.h"
#include "table_pixfont.h"
#include "ui_prims.h"
#include "commander_art.h"
#include "sf_qrcodegen.h"
#include "transmission_bank.h"

LV_FONT_DECLARE(sf_font_montserrat_28);
LV_FONT_DECLARE(sf_font_montserrat_36);
LV_FONT_DECLARE(sf_font_montserrat_12);

// ---------------------------------------------------------------------------
// Where things sit, colors
// ---------------------------------------------------------------------------
// The map direction the screen's TOP faces (board B17: the plug points south, the top faces east).
static const int TABLE_UP_MAP_DEG = 0;
// Each faction's hangar, map degrees (B17: in pairs beside the nebulae, spread 35 deg off them).
static const int TABLE_GATE_SPREAD = 35;
static const int FACTION_MAP_DEG[NUM_FACTIONS] = { -TABLE_GATE_SPREAD, TABLE_GATE_SPREAD, 180 + TABLE_GATE_SPREAD, 180 - TABLE_GATE_SPREAD };
static const int TABLE_RING_R = 158;  // the symbols
static const uint32_t TABLE_PHASE_COLOR[3] = { 0xD6EEFF, 0x469EFF, 0xFF4438 };  // Prepare white-blue, Movement blue, Resolution red
static const uint32_t TABLE_FACTION_MOOD[4] = { 0xFFC933, 0xC9D2DC, 0xA77BFF, 0xE0475B };  // Rebels gold, Empire steel, Pykes purple, Dawn crimson
static const uint32_t TABLE_NEWS = 0x5FD4FF, TABLE_GOLD = 0xFFD166, TABLE_BATTLE = 0xFF6B5A;
static const char *const TABLE_ERA_NAME[4] = { "THE REBELLION RISES", "IMPERIAL ORDER", "THE SPICE AGE", "THE DAWN GRIPS THE GALAXY" };
static const int OV_MOOD = 120, OV_WHITE = 255, OV_DIM = 70;  // overlay levels: the mood color itself / white-hot / dim

int tableAngle(int faction) {
  if (faction < 0 || faction >= NUM_FACTIONS) return 90;
  int a = (TABLE_UP_MAP_DEG + 270 - FACTION_MAP_DEG[faction]) % 360;
  return a < 0 ? a + 360 : a;
}
lv_point_t tablePolar(float r, float deg) {
  const float a = deg * 0.0174533f;
  lv_point_t p = { (int32_t)lroundf(233 + r * cosf(a)), (int32_t)lroundf(233 + r * sinf(a)) };
  return p;
}
static uint32_t tableGray(int v) { return (uint32_t)v * 0x010101u; }
static float tableAngDiff(float a, float b) { return fmodf(a - b + 540.0f, 360.0f) - 180.0f; }
static void tableArc(lv_layer_t *layer, int cx, int cy, int r, int w, float a0, float a1, uint32_t color, lv_opa_t opa) {
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.center.x = cx; d.center.y = cy;
  d.radius = r; d.width = w;
  d.start_angle = a0; d.end_angle = a1;
  d.color = lv_color_hex(color);
  d.opa = opa;
  lv_draw_arc(layer, &d);
}

// ---------------------------------------------------------------------------
// The overlay + deferred words / numbers
// ---------------------------------------------------------------------------
static uint8_t *ovl = nullptr;            // 466 rows x ovl_stride, L8, PSRAM
static int ovl_stride = 466;
static lv_obj_t *ovl_canvas = nullptr;
static uint8_t *scr = nullptr;            // scratch: one line of words, drawn upright
static int scr_stride = 512;
static lv_obj_t *scr_canvas = nullptr;
static const int SCR_W = 512, SCR_H = 64;

static bool ovlEnsure() {
  if (ovl_canvas && scr_canvas) return true;
  ovl_stride = (int)lv_draw_buf_width_to_stride(466, LV_COLOR_FORMAT_L8);
  scr_stride = (int)lv_draw_buf_width_to_stride(SCR_W, LV_COLOR_FORMAT_L8);
  if (!ovl) ovl = (uint8_t *)heap_caps_malloc((size_t)ovl_stride * 466, MALLOC_CAP_SPIRAM);
  if (!scr) scr = (uint8_t *)heap_caps_malloc((size_t)scr_stride * SCR_H, MALLOC_CAP_SPIRAM);
  if (!ovl || !scr) return false;
  ovl_canvas = lv_canvas_create(lv_layer_sys());  // survives screen changes; never shown
  lv_obj_add_flag(ovl_canvas, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_buffer(ovl_canvas, ovl, 466, 466, LV_COLOR_FORMAT_L8);
  scr_canvas = lv_canvas_create(lv_layer_sys());
  lv_obj_add_flag(scr_canvas, LV_OBJ_FLAG_HIDDEN);
  lv_canvas_set_buffer(scr_canvas, scr, SCR_W, SCR_H, LV_COLOR_FORMAT_L8);
  return true;
}

// Words: drawn upright into the scratch buffer, then placed. STRAIGHT = centered on (x, y) (the middle
// of the capitals), turned `deg` clockwise. ARC = bent round the middle, baseline at radius x, centered
// at screen angle y, tops outward (reads left to right from outside).
enum { TX_STRAIGHT, TX_ARC };
struct TxReq { char s[48]; const lv_font_t *f; int8_t space; uint8_t level, kind; float x, y, deg; };
static TxReq txq[40];
static int txn = 0;
// Pixel numerals (5x7), centered on (cx, cy), turned deg.
struct PixReq { char s[16]; float cx, cy, deg; uint8_t cell, level; };
static PixReq pixq[24];
static int pixn = 0;

static int textW(const char *s, const lv_font_t *f, int space) {
  int w = 0;
  for (const char *q = s; *q; q++) w += lv_font_get_glyph_width(f, (uint32_t)*q, (uint32_t)q[1]) + space;
  return w > 0 ? w - space : 0;
}
static int capH(const lv_font_t *f) { return (f->line_height - f->base_line) * 3 / 4; }
// The biggest font (36 / 28 / 20 / 14), starting at `top`, that keeps `s` within maxW px.
static const lv_font_t *fit(const char *s, int maxW, int space, const lv_font_t *top) {
  const lv_font_t *const F[4] = { &sf_font_montserrat_36, &sf_font_montserrat_28, LV_FONT_DEFAULT, &lv_font_montserrat_14 };
  int k = 0;
  while (k < 3 && F[k] != top) k++;
  for (; k < 3; k++) if (textW(s, F[k], space) <= maxW) return F[k];
  return F[3];
}
static void txStraight(const char *s, const lv_font_t *f, int level, float cx, float cy, float deg, int space = 0) {
  if (!s || !s[0] || txn >= 40) return;
  TxReq &q = txq[txn++];
  snprintf(q.s, sizeof(q.s), "%s", s);
  q.f = f; q.space = (int8_t)space; q.level = (uint8_t)constrain(level, 0, 255); q.kind = TX_STRAIGHT; q.x = cx; q.y = cy; q.deg = deg;
}
static void txArc(const char *s, const lv_font_t *f, int level, float r, float mid, int space) {
  if (!s || !s[0] || txn >= 40) return;
  TxReq &q = txq[txn++];
  snprintf(q.s, sizeof(q.s), "%s", s);
  q.f = f; q.space = (int8_t)space; q.level = (uint8_t)constrain(level, 0, 255); q.kind = TX_ARC; q.x = r; q.y = mid; q.deg = 0;
}
// Words for the player at screen angle `a`, centered `r` px out, tops toward the middle.
static void txFacing(const char *s, const lv_font_t *f, int level, float r, float a) {
  const lv_point_t p = tablePolar(r, a);
  txStraight(s, f, level, p.x, p.y, a - 90);
}
// The same words twice: right way up below the middle, upside down above it.
static void txBoth(const char *s, const lv_font_t *f, int level, float dy) {
  txStraight(s, f, level, 233, 233 + dy, 0);
  txStraight(s, f, level, 233, 233 - dy, 180);
}
// Words round the rim, top and bottom (mid 270 / 90) or turned by `mid`; long ones step down a size.
static void txRim(const char *s, int level, float mid) {
  if (!s || !s[0]) return;
  const lv_font_t *f = fit(s, 330, 3, &sf_font_montserrat_28);
  const float r = f == &sf_font_montserrat_28 ? 183 : 189;  // 3.1-dev.27: ~20 px clear of the rim (was 190 / 195)
  txArc(s, f, level, r, mid, 3);
  txArc(s, f, level, r, mid + 180, 3);
}
static int pixW(const char *s, int cell) { const int n = (int)strlen(s); return n ? (n * 6 - 1) * cell : 0; }
static void pix(const char *s, float cx, float cy, int cell, float deg, int level) {
  if (!s[0] || pixn >= 24) return;
  PixReq &q = pixq[pixn++];
  snprintf(q.s, sizeof(q.s), "%s", s);
  q.cx = cx; q.cy = cy; q.deg = deg; q.cell = (uint8_t)cell; q.level = (uint8_t)constrain(level, 0, 255);
}

static inline float sampleScr(float u, float v, int w, int h) {
  if (u < 0 || v < 0 || u >= w - 1 || v >= h - 1) return 0;
  const int x = (int)u, y = (int)v;
  const float fx = u - x, fy = v - y;
  const uint8_t *p = scr + (size_t)y * scr_stride + x;
  return (p[0] * (1 - fx) + p[1] * fx) * (1 - fy) + (p[scr_stride] * (1 - fx) + p[scr_stride + 1] * fx) * fy;
}
static inline void plot(uint8_t *dst, int stride, int x, int y, float v, int level) {
  if (v < 2) return;
  const uint8_t o = (uint8_t)fminf(255.0f, v * level / 255.0f);
  uint8_t &d = dst[(size_t)y * stride + x];
  if (o > d) d = o;
}
// Draws the queued words and numbers into `dst` (466 x 466, L8).
static uint8_t *txCache = nullptr;   // the placed words of the last redraw (466 x ovl_stride, PSRAM)
static uint32_t txCacheKey = 0;
uint32_t table_ms_words = 0;         // timing for the slow-redraw log
static uint32_t txKey() {
  uint32_t h = 2166136261u;
  auto mix = [&](const void *p, size_t n) { const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u; };
  for (int i = 0; i < txn; i++) {
    const TxReq &q = txq[i];
    mix(q.s, strlen(q.s)); mix(&q.f, sizeof(q.f)); mix(&q.space, 1); mix(&q.level, 1); mix(&q.kind, 1);
    mix(&q.x, 4); mix(&q.y, 4); mix(&q.deg, 4);
  }
  return h ^ (uint32_t)txn;
}
static void flushQueued(uint8_t *dst, int stride) {
  const uint32_t t0 = millis();
  if (!txCache) txCache = (uint8_t *)heap_caps_malloc((size_t)stride * 466, MALLOC_CAP_SPIRAM);
  const uint32_t key = txKey();
  uint8_t *const out = dst;
  const bool reuse = txCache && txn > 0 && key == txCacheKey;
  if (txCache && !reuse) { memset(txCache, 0, (size_t)stride * 466); dst = txCache; }
  for (int i = 0; i < (reuse ? 0 : txn); i++) {
    const TxReq &q = txq[i];
    const int w = textW(q.s, q.f, q.space) + 4, lh = q.f->line_height;
    if (w > SCR_W - 4 || lh > SCR_H - 4) continue;
    lv_canvas_fill_bg(scr_canvas, lv_color_black(), LV_OPA_COVER);
    lv_layer_t sl;
    lv_canvas_init_layer(scr_canvas, &sl);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = q.s;
    d.font = q.f;
    d.color = lv_color_white();
    d.letter_space = q.space;
    lv_area_t a = { 2, 2, (int32_t)(2 + w + 8), (int32_t)(2 + lh) };
    lv_draw_label(&sl, &d, &a);
    lv_canvas_finish_layer(scr_canvas, &sl);
    const float base = 2.0f + (lh - q.f->base_line);   // the baseline row in the scratch buffer
    const int tw = w - 4, v0 = 0, v1 = lh + 3;          // scratch rows that can hold ink
    const float uc = 2 + tw / 2.0f;                      // the words' middle column
    // 3.1-dev.25: placed by walking the SOURCE (half-pixel steps, value sampled smoothly, written to the
    // nearest screen pixel) -- scanning every screen pixel round a turned / bent word with trig per pixel
    // took up to ~300 ms a redraw on the console. Upright and upside-down words are a straight copy.
    if (q.kind == TX_STRAIGHT && (fabsf(q.deg) < 0.01f || fabsf(q.deg - 180) < 0.01f)) {
      const bool flip = q.deg > 90;
      const float vc = base - capH(q.f) / 2.0f;
      const int ox = (int)lroundf(q.x - (flip ? -1 : 1) * uc), oy = (int)lroundf(q.y - (flip ? -1 : 1) * vc);
      for (int v = v0; v <= v1; v++)
        for (int u = 0; u < w + 4 && u < SCR_W; u++) {
          const uint8_t s = scr[(size_t)v * scr_stride + u];
          if (!s) continue;
          const int x = flip ? ox - u : ox + u, y = flip ? oy - v : oy + v;
          if (x >= 0 && x < 466 && y >= 0 && y < 466) plot(dst, stride, x, y, s, q.level);
        }
    } else if (q.kind == TX_STRAIGHT) {
      const float vc = base - capH(q.f) / 2.0f, a = q.deg * 0.0174533f, ca = cosf(a), sa = sinf(a);
      for (float v = v0; v <= v1; v += 0.5f)
        for (float u = 0; u < w + 2; u += 0.5f) {
          const float s = sampleScr(u, v, w + 4, lh + 4);
          if (s < 2) continue;
          const float lx = u - uc, ly = v - vc;
          const int x = (int)lroundf(q.x + lx * ca - ly * sa), y = (int)lroundf(q.y + lx * sa + ly * ca);
          if (x >= 0 && x < 466 && y >= 0 && y < 466) plot(dst, stride, x, y, s, q.level);
        }
    } else {
      // bent round the middle: source column u -> angle mid + (u - uc) / r, source row v -> radius r + (base - v)
      const float r = q.x, step = 0.5f / r, cs = cosf(step), sn = sinf(step);
      for (float v = v0; v <= v1; v += 0.5f) {
        const float rho = r + (base - v);
        if (rho < 1) continue;
        const float th0 = (q.y * 0.0174533f) + (0 - uc) / r;
        float ex = cosf(th0), ey = sinf(th0);  // the unit vector, turned a little each step (no trig per sample)
        for (float u = 0; u < w + 2; u += 0.5f) {
          const float s = sampleScr(u, v, w + 4, lh + 4);
          if (s >= 2) {
            const int x = (int)lroundf(233 + rho * ex), y = (int)lroundf(233 + rho * ey);
            if (x >= 0 && x < 466 && y >= 0 && y < 466) plot(dst, stride, x, y, s, q.level);
          }
          const float nx = ex * cs - ey * sn;
          ey = ex * sn + ey * cs;
          ex = nx;
        }
      }
    }
  }
  if (txCache) {  // merge the words into the overlay
    txCacheKey = key;
    for (size_t k = 0, n = (size_t)stride * 466; k < n; k++) if (txCache[k] > out[k]) out[k] = txCache[k];
    dst = out;
  }
  txn = 0;
  table_ms_words = millis() - t0;
  for (int r = 0; r < pixn; r++) {
    const PixReq &q = pixq[r];
    const int n = (int)strlen(q.s);
    const uint8_t *glyph[16];
    for (int i = 0; i < n; i++) { const char *hit = strchr(PIX_CHARS, q.s[i]); glyph[i] = hit ? PIX_ROWS[hit - PIX_CHARS] : nullptr; }
    const float cell = q.cell, w = (n * 6 - 1) * cell, h = 7 * cell, g = cell > 3 ? cell - 1 : cell;
    const float a = q.deg * 0.0174533f, ca = cosf(a), sa = sinf(a);
    const float hx = 0.5f * (fabsf(w * ca) + fabsf(h * sa)) + 2, hy = 0.5f * (fabsf(w * sa) + fabsf(h * ca)) + 2;
    const int x0 = max(0, (int)(q.cx - hx)), x1 = min(465, (int)(q.cx + hx)), y0 = max(0, (int)(q.cy - hy)), y1 = min(465, (int)(q.cy + hy));
    for (int y = y0; y <= y1; y++)
      for (int x = x0; x <= x1; x++) {
        int hits = 0;
        for (int k = 0; k < 4; k++) {
          const float px = x + 0.25f + 0.5f * (k & 1) - q.cx, py = y + 0.25f + 0.5f * (k >> 1) - q.cy;
          const float lx = px * ca + py * sa + w / 2, ly = -px * sa + py * ca + h / 2;
          if (lx < 0 || ly < 0 || lx >= w || ly >= h) continue;
          const int ci = (int)(lx / cell), cj = (int)(ly / cell), ch = ci / 6, col = ci % 6;
          if (col == 5 || ch >= n || !glyph[ch]) continue;
          if (lx - ci * cell >= g || ly - cj * cell >= g) continue;
          if (glyph[ch][cj] & (0x10 >> col)) hits++;
        }
        if (hits) plot(dst, stride, x, y, 255.0f * hits / 4, q.level);
      }
  }
  pixn = 0;
}

// ---------------------------------------------------------------------------
// Shapes in the overlay
// ---------------------------------------------------------------------------
static void ovCircle(lv_layer_t *L, int r, int w, int level) { tableArc(L, 233, 233, r, w, 0, 360, tableGray(level), LV_OPA_COVER); }
static void ovDashLine(lv_layer_t *L, float a, float r0, float r1, int level) {
  for (float r = r0; r < r1; r += 8) {
    const lv_point_t p = tablePolar(r, a), q = tablePolar(fminf(r + 2.5f, r1), a);
    uiLine(L, p.x, p.y, q.x, q.y, lv_color_hex(tableGray(level)), 1);
  }
}
static void ovSymbol(lv_layer_t *L, int f, float cx, float cy, int r, int level) {
  if (f < 0 || f >= NUM_FACTIONS) return;
  ui_insignia_gray = level;
  uiDrawInsignia(L, f, (int)lroundf(cx), (int)lroundf(cy), r, 0x000000);
  ui_insignia_gray = -1;
}
// A die face (size px, `face` 1-6) centered on (cx, cy), turned to face the player at screen angle `deg`.
static void ovDie(lv_layer_t *L, int cx, int cy, int size, int face, float deg, int level) {
  static const int8_t PIPS[7][6][2] = { {}, { { 0, 0 } }, { { -1, -1 }, { 1, 1 } }, { { -1, -1 }, { 0, 0 }, { 1, 1 } },
    { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } }, { { -1, -1 }, { 1, -1 }, { 0, 0 }, { -1, 1 }, { 1, 1 } },
    { { -1, -1 }, { 1, -1 }, { -1, 0 }, { 1, 0 }, { -1, 1 }, { 1, 1 } } };
  face = constrain(face, 1, 6);
  const float a = (deg - 90) * 0.0174533f, ca = cosf(a), sa = sinf(a), h = size / 2.0f, q = size * 0.25f;
  auto at = [&](float x, float y, int &ox, int &oy) { ox = (int)lroundf(cx + x * ca - y * sa); oy = (int)lroundf(cy + x * sa + y * ca); };
  const lv_color_t c = lv_color_hex(tableGray(level));
  int px[4], py[4];
  at(-h, -h, px[0], py[0]); at(h, -h, px[1], py[1]); at(h, h, px[2], py[2]); at(-h, h, px[3], py[3]);
  for (int k = 0; k < 4; k++) uiLine(L, px[k], py[k], px[(k + 1) & 3], py[(k + 1) & 3], c, 3);
  for (int k = 0; k < face; k++) {
    int x, y;
    at(PIPS[face][k][0] * q, PIPS[face][k][1] * q, x, y);
    uiDisc(L, x, y, max(3, size / 11), c);
  }
}

// ---------------------------------------------------------------------------
// Seats
// ---------------------------------------------------------------------------
// Seat angle per faction for this game (-1 = no seat). Fills tr_scene's gates.
static void tableSeats(const GameState &gs, int seatAngle[NUM_FACTIONS]) {
  TrScene &s = tr_scene;
  int slotOf[NUM_FACTIONS], n = 0;
  for (int f = 0; f < NUM_FACTIONS; f++) { slotOf[f] = -1; seatAngle[f] = -1; }
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (gs.players[i].active && gs.players[i].chosenFaction >= 0) { slotOf[gs.players[i].chosenFaction] = i; n++; }
  s.halves = n == 2;
  s.seatCount = 0;
  int f1 = -1, f2 = -1;
  if (n == 2) for (int f = 0; f < NUM_FACTIONS; f++) if (slotOf[f] >= 0) { if (f1 < 0) f1 = f; else f2 = f; }
  for (int f = 0; f < NUM_FACTIONS; f++) {
    if (n == 2 && slotOf[f] < 0) continue;
    int a = tableAngle(f);
    if (n == 2) {  // push the two seats apart until they sit across from each other
      const int a1 = tableAngle(f1), a2 = tableAngle(f2);
      int diff = ((a2 - a1) % 360 + 360) % 360;
      if (diff > 180) diff -= 360;
      const int push = (180 - abs(diff)) / 2, dir = diff > 0 ? -1 : 1;
      a = f == f1 ? a1 + dir * push : a2 - dir * push;
      a = ((a % 360) + 360) % 360;
    }
    TrSeat &st = s.seat[s.seatCount++];
    st = {};
    st.angle = (int16_t)a;
    st.half = n == 2 ? 20 : 14;  // the boards' gate arcs at r 190
    st.barHalf = 0;
    const int slot = slotOf[f];
    if (slot < 0) { st.state = TR_SEAT_EMPTY; continue; }
    seatAngle[f] = a;
    const Player &p = gs.players[slot];
    st.first = slot == gs.firstPlayer;
    st.state = p.lockedIn ? TR_SEAT_SHUT : TR_SEAT_OPEN;
    st.color = 0;  // the boards: locked gates in the mood color
    if (p.away) { st.state = TR_SEAT_EMPTY; st.away = true; st.dim = true; }
  }
  // "Who are we waiting for?": once half the table has locked in, the open gates breathe.
  int active = 0, locked = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (gs.players[i].active) { active++; if (gs.players[i].lockedIn) locked++; }
  if (active > 1 && locked * 2 >= active && locked < active)
    for (int i = 0; i < s.seatCount; i++) if (s.seat[i].state == TR_SEAT_OPEN) s.seat[i].state = TR_SEAT_WAITING;
}
// Which pair of directions (top/bottom or the two sides) is farthest from every given angle.
static float quietMid(const int ang[NUM_FACTIONS]) {
  float best = 270, bestD = -1;
  const float CAND[2] = { 270, 0 };
  for (int c = 0; c < 2; c++) {
    float dmin = 999;
    for (int f = 0; f < NUM_FACTIONS; f++)
      if (ang[f] >= 0)
        for (int k = 0; k < 2; k++) dmin = fminf(dmin, fabsf(tableAngDiff(ang[f], CAND[c] + 180 * k)));
    if (dmin > bestD + 0.5f) { bestD = dmin; best = CAND[c]; }
  }
  return best;
}

// ---------------------------------------------------------------------------
// The screens
// ---------------------------------------------------------------------------
// A BIG MOMENT (boards "Galaxy News", "A grudge strike lands", "A new Era begins", "The game is won"):
// the rim line, the middle (the caller), then for both sides a lighter line, the big word and a pixel
// number (or a last line of words).
static void moment(const char *rim, const char *line1, const char *big, const char *line3, bool pix3) {
  txRim(rim, OV_MOOD + 20, 270);
  for (int side = 0; side < 2; side++) {
    const float deg = side ? 180 : 0, sg = side ? -1.0f : 1.0f;
    if (line1 && line1[0]) txStraight(line1, fit(line1, 300, 2, LV_FONT_DEFAULT), 190, 233, 233 + sg * 70, deg, 2);
    if (big && big[0]) txStraight(big, fit(big, 290, 1, &sf_font_montserrat_36), OV_WHITE, 233, 233 + sg * 103, deg, 1);
    if (line3 && line3[0]) {
      if (pix3) pix(line3, 233, 233 + sg * 140, pixW(line3, 4) <= 200 ? 4 : 3, deg, 210);
      else txStraight(line3, fit(line3, 196, 1, LV_FONT_DEFAULT), 175, 233, 233 + sg * 138, deg, 1);
    }
  }
}

static void sceneBattle(lv_layer_t *L, const GameState &gs, const TableView &v, const int seatAngle[NUM_FACTIONS]) {
  // BATTLE (boards "Table battle for a world" / "between players"): only the fight. Along each
  // fighter's line, from the middle out: its die (r 86), ships left in pixels (r 134), SHIPS /
  // GARRISON (r 166), its symbol or the world's name (r 196). A dashed line joins them.
  TrScene &s = tr_scene;
  s.seatCount = 0;
  const int af = gs.players[gs.battleAttackerSlot].chosenFaction;
  const float aa = af >= 0 && seatAngle[af] >= 0 ? seatAngle[af] : 35;
  const bool pvp = gs.battleKind == BATTLE_PVP && gs.battleDefenderSlot >= 0;
  const int df = pvp ? gs.players[gs.battleDefenderSlot].chosenFaction : -1;
  const float da = pvp && df >= 0 && seatAngle[df] >= 0 ? seatAngle[df] : aa + 180;
  const bool tumble = v.tumble || gs.battleExchange == 0;
  const int atkFace = tumble ? (int)random(1, 7) : gs.lastAtkRoll - gs.battleAtkMod;
  const int defFace = tumble ? (int)random(1, 7) : gs.lastDefRoll - gs.battleDefMod;
  const bool atkWon = !tumble && gs.lastAtkRoll > gs.lastDefRoll, defWon = !tumble && gs.lastDefRoll > gs.lastAtkRoll;
  int atkShips = 0, defShips = 0;
  for (int k = 0; k < BATTLE_SHIPS; k++) { if (gs.atkAlive[k]) atkShips++; if (gs.defAlive[k]) defShips++; }
  atkShips += gs.battleEscortAlive + gs.battleAtkMercs;
  defShips = pvp ? defShips + gs.battleDefMercs : gs.defRemaining + gs.battleDefMercs + gs.battleDefExtra;  // (the militia is already in defRemaining)
  char num[8];
  for (int side = 0; side < 2; side++) {
    const float a = side ? da : aa;
    const bool won = side ? defWon : atkWon, lost = side ? atkWon : defWon;
    ovDashLine(L, a, pvp ? 32 : 52, 57, 60);   // the boards' dashed line, in the gaps: middle -> die -> number
    ovDashLine(L, a, 112, 118, 60);
    const lv_point_t dp = tablePolar(84, a);
    ovDie(L, dp.x, dp.y, 54, side ? defFace : atkFace, a, won ? 255 : lost ? 95 : 190);
    snprintf(num, sizeof(num), "%d", side ? defShips : atkShips);
    const lv_point_t np = tablePolar(132, a);
    pix(num, np.x, np.y, 5, a - 90, OV_WHITE);
    txFacing((side && !pvp) ? "DEFENDERS" : "SHIPS", &lv_font_montserrat_14, OV_MOOD + 50, 163, a);
    if (side == 0 || pvp) {
      const lv_point_t bp = tablePolar(196, a);
      ovSymbol(L, side ? df : af, bp.x, bp.y, 19, won || tumble ? 255 : 160);
    } else if (gs.battleTargetPlanet >= 0 && gs.battleTargetPlanet < NUM_PLANETS) {
      txFacing(PLANETS[gs.battleTargetPlanet].name, fit(PLANETS[gs.battleTargetPlanet].name, 120, 1, LV_FONT_DEFAULT), OV_WHITE, 192, a);
    }
  }
  if (pvp) { ovCircle(L, 30, 2, OV_MOOD); pix("VS", 233, 233, 3, 0, 200); }
  else ovCircle(L, 50, 2, OV_MOOD + 20);
  char head[40];
  if (pvp) snprintf(head, sizeof(head), "%s VS %s", snapshotCallName(gs, af), snapshotCallName(gs, df));
  else snprintf(head, sizeof(head), "BATTLE FOR %s", s.planet >= 0 ? PLANETS[s.planet].name : "A WORLD");
  int fighters[NUM_FACTIONS] = { (int)aa, (int)da, -1, -1 };
  txRim(head, OV_MOOD + 20, quietMid(fighters));
}

static void sceneClock(lv_layer_t *L, const GameState &gs, const TableView &v) {
  TrScene &s = tr_scene;
  const int era = snapshotEra(gs);
  s.color = gs.battleActive ? TABLE_BATTLE : era >= 0 ? TABLE_FACTION_MOOD[era] : TABLE_PHASE_COLOR[gs.phase % 3];
  s.phase = gs.phase % 3;  // the renderer jumps to hyperspace when it changes
  s.noPhaseRing = true;
  s.planet = (gs.battleActive && gs.battleKind == BATTLE_PLANET) ? gs.battleTargetPlanet : -1;
  s.planetCell = 3;  // 84 px (board: r 44)
  int seatAngle[NUM_FACTIONS];
  tableSeats(gs, seatAngle);
  if (gs.battleActive && gs.battleAttackerSlot >= 0) { sceneBattle(L, gs, v, seatAngle); return; }

  // CLOCK (boards "Table clock" / "Clock in an Era"): a gate arc per player at the rim (the renderer:
  // dashed while open, solid + glowing once locked in), their symbol at r 158 (bright once locked),
  // their VP in pixels at r 120 facing them, the phase twice at r 98, the round big in the middle.
  // An Era: everything in its color, its name round the rim, the phase white.
  const bool hidden = false;  // 3.1-dev.88: the Shadow Age (hidden standings) is gone
  for (int f = 0; f < NUM_FACTIONS; f++) {
    if (seatAngle[f] < 0) continue;
    int slot = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) if (gs.players[i].active && gs.players[i].chosenFaction == f) slot = i;
    const bool away = slot >= 0 && gs.players[slot].away, locked = slot >= 0 && gs.players[slot].lockedIn;
    const lv_point_t c = tablePolar(TABLE_RING_R, seatAngle[f]);
    ovSymbol(L, f, c.x, c.y, s.halves ? 19 : 17, away ? OV_DIM : locked ? 255 : 130);
    if (slot < 0) continue;
    char vp[8];
    if (hidden) snprintf(vp, sizeof(vp), "?");
    else snprintf(vp, sizeof(vp), "%d", gs.players[slot].vp);
    const lv_point_t np = tablePolar(120, seatAngle[f]);
    pix(vp, np.x, np.y, 4, seatAngle[f] - 90, away ? 90 : 235);
  }
  char rn[6], lim[6];
  snprintf(rn, sizeof(rn), "%d", gs.round);
  snprintf(lim, sizeof(lim), "/%d", gs.roundLimit);
  const int rc = gs.round >= 10 ? 7 : 9, w1 = pixW(rn, rc), w2 = pixW(lim, 3), x0 = 233 - (w1 + 8 + w2) / 2;
  pix(rn, x0 + w1 / 2.0f, 233, rc, 0, OV_WHITE);
  pix(lim, x0 + w1 + 8 + w2 / 2.0f, 233 + (7 * rc - 21) / 2.0f, 3, 0, 120);
  const float mid = quietMid(seatAngle);
  txArc(PHASE_NAMES[gs.phase % 3], LV_FONT_DEFAULT, era >= 0 ? OV_WHITE : 175, 88, mid, 3);
  txArc(PHASE_NAMES[gs.phase % 3], LV_FONT_DEFAULT, era >= 0 ? OV_WHITE : 175, 88, mid + 180, 3);
  if (era >= 0) {
    char left[24];
    const int n = gs.eraUntil - gs.round + 1;
    snprintf(left, sizeof(left), "%d ROUND%s LEFT", n, n == 1 ? "" : "S");
    txArc(TABLE_ERA_NAME[era], fit(TABLE_ERA_NAME[era], 230, 3, LV_FONT_DEFAULT), OV_MOOD + 40, 189, mid, 3);
    txArc(left, LV_FONT_DEFAULT, OV_MOOD + 40, 189, mid + 180, 3);
  }
}

// WHO GOES FIRST: the gates open, a comet runs round and eases onto the first player's gate.
static void sceneFirst(lv_layer_t *L, const GameState &gs) {
  TrScene &s = tr_scene;
  s.color = TABLE_PHASE_COLOR[1];
  int seatAngle[NUM_FACTIONS];
  tableSeats(gs, seatAngle);
  for (int i = 0; i < s.seatCount; i++) if (s.seat[i].state != TR_SEAT_EMPTY) s.seat[i].state = TR_SEAT_OPEN;
  for (int i = 0; i < s.seatCount; i++) if (s.seat[i].first) s.cometAngle = s.seat[i].angle;
  for (int f = 0; f < NUM_FACTIONS; f++) {
    if (seatAngle[f] < 0) continue;
    const lv_point_t c = tablePolar(TABLE_RING_R, seatAngle[f]);
    ovSymbol(L, f, c.x, c.y, s.halves ? 19 : 17, 200);
  }
  txBoth("WHO GOES", &sf_font_montserrat_28, OV_WHITE, 22);
  txBoth("FIRST?", &sf_font_montserrat_28, OV_WHITE, 58);
}

// Everything the table announces, in the board layout, in the acting faction's color.
static void sceneAlert(lv_layer_t *L, const GameState &gs) {
  TrScene &s = tr_scene;
  const int m = gs.publicMoment, actor = gs.publicActor, victim = gs.publicVictim, planet = gs.publicPlanet;
  const bool hasActor = actor >= 0 && actor < NUM_FACTIONS;
  const char *name = snapshotCallName(gs, actor);
  const char *where = (planet >= 0 && planet < NUM_PLANETS) ? PLANETS[planet].name : "";
  char rim[48] = "", l1[40] = "", big[32] = "", l3[48] = "";
  bool pix3 = false, showPlanet = false, showSymbol = hasActor, showRound = false, ripples = false;
  int symbolRing = 0;
  s.color = hasActor ? TABLE_FACTION_MOOD[actor] : TABLE_NEWS;
  if (m == PUBLIC_ASHEN_SUNK) {
    s.color = 0xFF8C42;
    snprintf(rim, sizeof(rim), "THE ASHEN FLEET");
    snprintf(big, sizeof(big), "DESTROYED");
    snprintf(l3, sizeof(l3), "%s STRUCK LAST", hasActor ? name : "THE GALAXY");
    showSymbol = false; showRound = true;
  } else if (m >= PUBLIC_NEWS_BASE) {
    const int id = m - PUBLIC_NEWS_BASE;
    snapshotNewsText(gs, l1, sizeof(l1), big, sizeof(big), l3, sizeof(l3));
    snprintf(rim, sizeof(rim), "GALAXY NEWS - ROUND %d", gs.round);
    if (id == NEWS_STRIKE) {  // board "A grudge strike lands"
      static const char *const KIND[4] = { "UPRISING ON", "IMPERIAL STRIKE ON", "SHAKEDOWN ON", "ASSASSINS ON" };
      static const int COST[4] = { GRUDGE_UPRISING, GRUDGE_EMPIRE_HIT, GRUDGE_SHAKEDOWN_CR, GRUDGE_ASSASSIN_HIT };
      static const char *const UNIT[4] = { "DEFENDERS", "DEFENDERS", "CR", "SHIPS" };
      const int f = hasActor ? actor : 0;
      snprintf(rim, sizeof(rim), "THE %s STRIKE", FACTION_SHORT[f]);
      snprintf(l1, sizeof(l1), "%s", KIND[f]);
      snprintf(l3, sizeof(l3), "-%d %s", COST[f], UNIT[f]);
      pix3 = true; ripples = true;
    } else if (id == NEWS_ERA && hasActor) {  // board "A new Era begins"
      static const char *const L1[4] = { "THE REBELLION", "IMPERIAL", "THE", "THE DAWN" };
      static const char *const BIG[4] = { "RISES", "ORDER", "SPICE AGE", "GRIPS ALL" };
      const int champ = gs.eraBy >= 0 && gs.eraBy < MAX_PLAYERS ? gs.players[gs.eraBy].chosenFaction : -1;
      snprintf(rim, sizeof(rim), "A NEW ERA - CHAMPION: %s", snapshotCallName(gs, champ));
      snprintf(l1, sizeof(l1), "%s", L1[actor]);
      snprintf(big, sizeof(big), "%s", BIG[actor]);
      const int rounds = gs.eraUntil - gs.round + 1;
      snprintf(l3, sizeof(l3), "%d ROUND%s", rounds, rounds == 1 ? "" : "S");
      pix3 = true; symbolRing = 40;
    } else if (id == NEWS_OFFENSIVE_LANDS) {
      ripples = true;
    }
    showPlanet = planet >= 0 && planet < NUM_PLANETS && id != NEWS_STRIKE && id != NEWS_ERA;
  } else if (m >= PUBLIC_EVENT_BASE) {
    s.color = TABLE_BATTLE;
    snprintf(rim, sizeof(rim), "GALAXY EVENT");
    snprintf(big, sizeof(big), "%s", eventName(m - PUBLIC_EVENT_BASE));
    snprintf(l3, sizeof(l3), "SEE YOUR PHONE");
    showSymbol = false; showRound = true;
  } else {
    switch (m) {
      case TXB_FIRSTBLOOD: snprintf(rim, sizeof(rim), "FIRST BLOOD"); snprintf(big, sizeof(big), "%s", name); snprintf(l3, sizeof(l3), "TOOK %s", where); showPlanet = true; break;
      case TXB_SEAT:       snprintf(rim, sizeof(rim), "A HOME WORLD FALLS"); snprintf(big, sizeof(big), "%s", name); snprintf(l3, sizeof(l3), "TOOK %s", where); showPlanet = true; break;
      case TXB_CORE:       snprintf(rim, sizeof(rim), "THE CORE FALLS"); snprintf(big, sizeof(big), "%s", name); snprintf(l3, sizeof(l3), "TOOK %s", where); showPlanet = true; break;
      case TXB_BETRAYAL:   snprintf(rim, sizeof(rim), "BETRAYAL"); snprintf(big, sizeof(big), "%s", name);
                           snprintf(l3, sizeof(l3), "TURNED ON THE %s", (victim >= 0 && victim < 4) ? FACTION_SHORT[victim] : "GALAXY"); showPlanet = true; break;
      case TXB_FLEET:      snprintf(rim, sizeof(rim), "FLEET DESTROYED"); snprintf(big, sizeof(big), "%s", name);
                           snprintf(l3, sizeof(l3), "SANK %s'S FLEET", snapshotCallName(gs, victim)); break;
      default:             s.color = TABLE_BATTLE; snprintf(rim, sizeof(rim), "FINAL ROUND");
                           snprintf(big, sizeof(big), "LAST ROUND"); snprintf(l3, sizeof(l3), "IT ALL COMES DOWN TO THIS");
                           showSymbol = false; showRound = true; break;
    }
  }
  if (ripples) { ovCircle(L, 60, 3, 200); ovCircle(L, 88, 2, 115); ovCircle(L, 118, 2, 55); }
  if (showPlanet && planet >= 0 && planet < NUM_PLANETS) { s.planet = planet; s.planetCell = 3; ovCircle(L, 50, 2, OV_MOOD + 20); }
  else if (showSymbol) {
    if (symbolRing) ovCircle(L, symbolRing, 2, OV_MOOD + 40);
    ovSymbol(L, actor, 233, 233, symbolRing ? 26 : 32, OV_WHITE);
  } else if (showRound) { char rn[6]; snprintf(rn, sizeof(rn), "%d", gs.round); pix(rn, 233, 233, 6, 0, OV_WHITE); }
  moment(rim, l1, big, l3, pix3);
}

// VICTORY (board "The game is won"): gold, the winner's symbol in a ring, "KESS WINS", the VP in pixels.
static void sceneVictory(lv_layer_t *L, const GameState &gs) {
  TrScene &s = tr_scene;
  s.color = TABLE_GOLD;
  s.rimCurrent = gs.roundLimit;
  const int w = snapshotWinnerSlot(gs);
  const int f = w >= 0 ? gs.players[w].chosenFaction : -1;
  if (f >= 0) {
    char rim[48], big[32], vp[12];
    snprintf(rim, sizeof(rim), "THE %s TAKE THE GALAXY", FACTION_SHORT[f]);
    snprintf(big, sizeof(big), "%s WINS", snapshotCallName(gs, f));
    snprintf(vp, sizeof(vp), "%d VP", gs.players[w].vp);
    ovCircle(L, 42, 3, OV_MOOD + 60);
    ovSymbol(L, f, 233, 233, 26, OV_WHITE);
    s.fireworks = true;
    moment(rim, "", big, vp, true);
  } else {
    moment("NOBODY RULES THE GALAXY", "", "A DRAW", "", false);
  }
}

static int mode_shown = -1;
void tableComposeReset() { mode_shown = -1; }

int tableCompose(const GameState &gs, const TableView &v) {
  const int mode = gs.stage == STAGE_GAME_OVER ? TR_VICTORY : v.first ? TR_FIRST : v.alert ? TR_ALERT : TR_CLOCK;
  TrScene &s = tr_scene;
  if (mode != mode_shown) { s.modeStart = millis(); mode_shown = mode; }
  s.mode = (uint8_t)mode;
  s.rimTotal = gs.roundLimit;
  s.rimCurrent = gs.round;
  s.rimFinal = gs.round >= gs.roundLimit;
  s.phase = -1;
  s.noPhaseRing = false;
  s.ripple = s.fireworks = false;
  s.halves = false;
  s.seatCount = 0;
  s.planetCell = 5;
  s.spotCount = s.boltCount = 0;
  s.planet = -1;
  if (!ovlEnsure()) return -1;
  lv_canvas_fill_bg(ovl_canvas, lv_color_black(), LV_OPA_COVER);
  lv_layer_t L;
  lv_canvas_init_layer(ovl_canvas, &L);
  txn = pixn = 0;
  if (mode == TR_CLOCK) sceneClock(&L, gs, v);
  else if (mode == TR_ALERT) sceneAlert(&L, gs);
  else if (mode == TR_FIRST) sceneFirst(&L, gs);
  else sceneVictory(&L, gs);
  lv_canvas_finish_layer(ovl_canvas, &L);
  flushQueued(ovl, ovl_stride);
  trOverlayChanged(ovl, ovl_stride);
  return mode;
}

// ---------------------------------------------------------------------------
// THE LOBBY (board "Lobby"): the code in the middle, SCAN TO JOIN round the rim for both sides, each
// faction's symbol at its hangar (r 168) -- lit in its color once taken, a dashed ring while open --
// and the character's name inside it (r 124) facing that seat, OPEN while free. The words are one
// A8 image (the same word pipeline as the glow screens); symbols / rings are drawn live.
// ---------------------------------------------------------------------------
static const int LOBBY_SEAT_R = 140, LOBBY_NAME_R = 190, LOBBY_FACE = 64;  // names OUTSIDE the faces (the code needs its room)
// 3.1-dev.27 (user drew where the faces should go): each seat turns 28 deg from its hangar toward the
// nearest side -- the free space beside the code, clear of SCAN TO JOIN at the top and bottom.
static int lobbySeatAngle(int f) {
  const int a = tableAngle(f);
  const float r = a * 0.0174533f;
  const int toward = (sinf(r) < 0) == (cosf(r) > 0) ? 28 : -28;  // top-right / bottom-left turn clockwise
  return ((a + toward) % 360 + 360) % 360;
}
static lv_image_dsc_t lobby_words_dsc;
static int lobby_slot[4], lobby_angle[4];
static void lobbyDrawCb(lv_event_t *e) {
  lv_layer_t *L = lv_event_get_layer(e);
  tableArc(L, 233, 233, 222, 8, 0, 360, 0x5FD4FF, LV_OPA_20);
  tableArc(L, 233, 233, 222, 2, 0, 360, 0x5FD4FF, LV_OPA_COVER);
  for (int f = 0; f < NUM_FACTIONS; f++) {  // open seats: a dashed ring (taken seats are face objects, see tableLobbyBuild)
    if (lobby_slot[f] >= 0) continue;
    const lv_point_t c = tablePolar(LOBBY_SEAT_R, lobby_angle[f]);
    for (int k = 0; k < 16; k++) tableArc(L, c.x, c.y, 28, 2, k * 22.5f, k * 22.5f + 11, 0x3A4A5C, LV_OPA_COVER);
  }
}
// The character's face (the phones' 24x28 art at 2x), baked once per character into PSRAM.
static lv_image_dsc_t face_dsc[NUM_COMMANDERS];
static uint8_t *face_px[NUM_COMMANDERS];
static const lv_image_dsc_t *faceImage(int id) {
  if (id < 0 || id >= NUM_COMMANDERS) return nullptr;
  if (face_px[id]) return &face_dsc[id];
  const CommanderArt &a = COMMANDER_ART[id];
  const int s = 2, w = COMMANDER_ART_W * s, h = COMMANDER_ART_H * s;
  uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)w * h * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) return nullptr;
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const uint8_t i = a.px[(y / s) * COMMANDER_ART_W + x / s];
      const uint32_t rgb = i ? a.pal[i] : 0;
      uint8_t *p = buf + ((size_t)y * w + x) * 4;  // ARGB8888 = B,G,R,A
      p[0] = rgb & 0xFF; p[1] = (rgb >> 8) & 0xFF; p[2] = (rgb >> 16) & 0xFF; p[3] = i ? 0xFF : 0;
    }
  lv_image_dsc_t &d = face_dsc[id];
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_ARGB8888;
  d.header.w = w; d.header.h = h;
  d.header.stride = w * 4;
  d.data_size = (uint32_t)w * h * 4;
  d.data = buf;
  face_px[id] = buf;
  return &d;
}
static void faceSymbolCb(lv_event_t *e) {  // a taken seat with no character yet: the faction's symbol in the circle
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const int f = (int)(intptr_t)lv_event_get_user_data(e);
  uiDrawInsignia(lv_event_get_layer(e), f, (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, 18, (FACTION_RING_COLOR[f] >> 3) & 0x1F1F1F);
}
static lv_image_dsc_t qr_dsc[2];
static uint8_t *qr_px[2];
static const lv_image_dsc_t *qrImage(int slot, const char *text) {
  if (slot < 0 || slot > 1) return nullptr;
  if (qr_px[slot]) return &qr_dsc[slot];
  static uint8_t qr[sfqr_BUFFER_LEN_FOR_VERSION(8)], tmp[sfqr_BUFFER_LEN_FOR_VERSION(8)];
  if (!sfqr_encodeText(text, tmp, qr, sfqr_Ecc_MEDIUM, 1, 8, sfqr_Mask_AUTO, true)) return nullptr;
  const int n = sfqr_getSize(qr), quiet = 2, s = 4, w = (n + 2 * quiet) * s;
  const size_t bytes = (size_t)w * w * 4;
  uint8_t *buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t *)malloc(bytes);
  if (!buf) return nullptr;
  for (int y = 0; y < w; y++)
    for (int x = 0; x < w; x++) {
      const int mx = x / s - quiet, my = y / s - quiet;
      const bool dark = mx >= 0 && my >= 0 && mx < n && my < n && sfqr_getModule(qr, mx, my);
      uint8_t *p = buf + ((size_t)y * w + x) * 4;  // ARGB8888 = B,G,R,A
      const uint8_t v = dark ? 0x05 : 0xF4;
      p[0] = v; p[1] = v; p[2] = v; p[3] = 0xFF;
    }
  lv_image_dsc_t &d = qr_dsc[slot];
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_ARGB8888;
  d.header.w = w; d.header.h = w;
  d.header.stride = w * 4;
  d.data_size = bytes;
  d.data = buf;
  qr_px[slot] = buf;
  return &d;
}

void tableLobbyBuild(lv_obj_t *root, const GameState &gs, const TableLobbyEnv &env) {
  for (int f = 0; f < NUM_FACTIONS; f++) {
    lobby_angle[f] = lobbySeatAngle(f);
    lobby_slot[f] = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) if (gs.players[i].active && gs.players[i].chosenFaction == f) lobby_slot[f] = i;
  }
  lv_obj_t *shapes = lv_obj_create(root);
  lv_obj_remove_style_all(shapes);  // style first (Convention 13)
  lv_obj_clear_flag(shapes, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(shapes, 466, 466);
  lv_obj_set_pos(shapes, 0, 0);
  lv_obj_add_event_cb(shapes, lobbyDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
  // The words, into the overlay buffer (the glow renderer is off in the Lobby), shown as an A8 image.
  if (ovlEnsure()) {
    memset(ovl, 0, (size_t)ovl_stride * 466);
    txn = pixn = 0;
    if (env.homeWifi) txRim("SCAN TO JOIN", 235, 270);
    else {
      txArc("SCAN: TABLE WI-FI", LV_FONT_DEFAULT, 235, 189, 270, 3);
      txArc("THEN 192.168.4.1", LV_FONT_DEFAULT, 235, 189, 90, 3);
    }
    for (int f = 0; f < NUM_FACTIONS; f++) {
      if (lobby_slot[f] < 0) { txFacing("OPEN", &lv_font_montserrat_14, 110, LOBBY_NAME_R, lobby_angle[f]); continue; }
      const Player &p = gs.players[lobby_slot[f]];
      if (p.commander < 0 || p.commander >= NUM_COMMANDERS) { txFacing("CHOOSING...", &lv_font_montserrat_14, 150, LOBBY_NAME_R, lobby_angle[f]); continue; }
      txFacing(COMMANDER_CALL[p.commander], fit(COMMANDER_CALL[p.commander], 100, 1, LV_FONT_DEFAULT), 240, LOBBY_NAME_R, lobby_angle[f]);
    }
    flushQueued(ovl, ovl_stride);
    memset(&lobby_words_dsc, 0, sizeof(lobby_words_dsc));
    lobby_words_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    lobby_words_dsc.header.cf = LV_COLOR_FORMAT_A8;
    lobby_words_dsc.header.w = 466;
    lobby_words_dsc.header.h = 466;
    lobby_words_dsc.header.stride = ovl_stride;
    lobby_words_dsc.data_size = (uint32_t)ovl_stride * 466;
    lobby_words_dsc.data = ovl;
    lv_obj_t *words = lv_image_create(root);
    lv_image_set_src(words, &lobby_words_dsc);
    lv_obj_set_style_image_recolor(words, lv_color_hex(0xE8EEF5), 0);
    lv_obj_set_style_image_recolor_opa(words, LV_OPA_COVER, 0);
    lv_obj_set_pos(words, 0, 0);
  }
  const lv_image_dsc_t *qd = qrImage(env.homeWifi ? 0 : 1, env.homeWifi ? env.gameUrl : "WIFI:T:WPA;S:ShadowFleet;P:starwars1;;");
  const int qh = qd ? (int)qd->header.w / 2 : 60;
  lv_obj_t *img = lv_image_create(root);
  if (qd) lv_image_set_src(img, qd);
  lv_obj_set_style_radius(img, 10, 0);
  lv_obj_set_style_clip_corner(img, true, 0);
  lv_obj_set_pos(img, 233 - qh, 233 - qh);
  for (int f = 0; f < NUM_FACTIONS; f++) {  // taken seats: the face in a circle ringed in the faction's color
    if (lobby_slot[f] < 0) continue;
    const Player &p = gs.players[lobby_slot[f]];
    const lv_point_t c = tablePolar(LOBBY_SEAT_R, lobby_angle[f]);
    const uint32_t ring = FACTION_RING_COLOR[f];
    lv_obj_t *b = lv_obj_create(root);
    lv_obj_remove_style_all(b);  // style first (Convention 13)
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(b, true, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(ring), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex((ring >> 3) & 0x1F1F1F), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, LOBBY_FACE, LOBBY_FACE);
    lv_obj_set_pos(b, c.x - LOBBY_FACE / 2, c.y - LOBBY_FACE / 2);
    const lv_image_dsc_t *fd = p.commander >= 0 ? faceImage(p.commander) : nullptr;
    if (fd) {
      lv_obj_t *im = lv_image_create(b);
      lv_image_set_src(im, fd);
      lv_obj_align(im, LV_ALIGN_BOTTOM_MID, 0, 2);
    } else {
      lv_obj_add_event_cb(b, faceSymbolCb, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)f);
    }
  }
  // The Wi-Fi switch, small, under the code: 116 x 34 at (175, 356) -- corner ~162 px from the middle.
  if ((env.homeWifi || env.homeSaved) && env.wifiSwitchCb) {  // (the console passes nullptr and draws its own button)
    const uint32_t col = env.homeWifi ? 0x8FA3B8 : 0x5FD4FF;
    lv_obj_t *b = lv_obj_create(root);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(col), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0C1118), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_size(b, 116, 34);
    lv_obj_set_pos(b, 175, 356);
    if (env.wifiSwitchCb) lv_obj_add_event_cb(b, env.wifiSwitchCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, env.homeWifi ? "TABLE WI-FI" : "HOME WI-FI");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_center(l);
    if (!env.homeWifi && env.homeFailed) {
      char nf[150];
      char nf2[150];
      snprintf(nf, sizeof(nf), "Couldn't reach %s", env.homeSsid ? env.homeSsid : "");
      if (env.failWhy && env.failWhy[0]) { snprintf(nf2, sizeof(nf2), "%s:\n%s", nf, env.failWhy); snprintf(nf, sizeof(nf), "%s", nf2); }
      lv_obj_t *t = lv_label_create(root);
      lv_label_set_text(t, nf);
      lv_obj_set_width(t, 300);
      lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_font(t, &sf_font_montserrat_12, 0);
      lv_obj_set_style_text_color(t, lv_color_hex(0xFF9B70), 0);
      lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 388);
    }
  }
}
