// Table Console glow renderer (v0.26.0; hangar gates v0.27.0) -- see table_render.h.
// Ported from the user's Clutch prop (clutch_core.ino): quadrant radius/angle
// table mirrored four ways, per-frame 1-D tables, a bloom tone map. Frames go
// out in BSP_STRIP_ROWS strips through the board layer's DMA buffers (the
// same ones LVGL uses -- LVGL is idle on these screens), rendered while the
// previous strip is still being sent.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "board_select.h"
#include "table_render.h"
#include "board_bsp.h"
#include "sf_health.h"   // 3.1-dev.32: the frame checks in with the watchdog strip by strip

TrScene tr_scene;

static constexpr int W = 466, H = 466, CX = 233, CY = 233, RMAX = 233;
static constexpr int RQ = 4;                    // radius in quarter pixels: thin lines stay smooth
static constexpr int RBINS = RMAX * RQ + 4;
static constexpr int ABINS = 2048;              // angle steps, 0 = 12 o'clock, clockwise (v0.27.2: 2048 -- sub-pixel at the rim, and the tables fit internal RAM)
static constexpr float AK = ABINS / 4096.0f;   // scale for bin counts tuned at 4096
static constexpr int ONE = 256;                 // intensity 1.0 (8.8 fixed point)
static constexpr int IMAX = 4 * ONE;
static constexpr int QN = RMAX + 1;

// Where things sit (px from the center).
static constexpr float CORE_R = 82;     // the clock face's edge
static constexpr float PHASE_R = 98;    // phase ring
static constexpr float DOOR_IN = 190, DOOR_OUT = 190;  // 3.1-dev.24: ONE gate arc at r 190 (the boards); was two doors at 134 / 182
static constexpr float RING_R = 158;    // the ring the gates sit in (symbols on it)
static constexpr float BAR_R = 204;     // standings bars
static constexpr float RIM_R = 222;     // round segments

enum Band : uint8_t { B_NONE, B_PHASE, B_DOOR, B_RING, B_BAR, B_RIM, BAND_COUNT };

static bool ready = false, running = false;
static bool angInternal = false;
static uint32_t *quad;
static uint32_t *quadRow[QN];
static int16_t quadLen[QN];
static uint8_t *bandOf;
static uint16_t *radTab, *profTab, *toneMap, *rowI;
static uint8_t *angTab[BAND_COUNT];
static uint8_t *segIdx, *segMask;
static int segTotal = -1;
static constexpr int SPR = 120;                 // glow-ring sprite
static uint8_t *ringSpr;
static const uint8_t *ovl = nullptr;
static int ovlStride = W;
static int16_t ovX0[H], ovX1[H];                // per row: first / last overlay column with ink (x0 > x1 = none)
static float colR = 0, colG = 0, colB = 0;
static bool colSet = false;
static float doorPos[4] = { 0.1f, 0.1f, 0.1f, 0.1f };  // 0 = open .. 1 = shut, eased toward each seat's state
// 2.0-dev.11 (user picked "B + C" from board P take 2): a locked gate glows in
// its owner's color -- a tone map per seat, used where gateAng / gateR say a
// pixel is inside a locked gate (doors, ring, symbol). In PSRAM: the table's
// internal RAM is tight.
static uint16_t *toneF[4];
static uint32_t toneFFor[4] = { 0, 0, 0, 0 };
static uint8_t *gateAng, *gateR;
static uint8_t rowM[W];
static float barNow[4] = {};
// 3.1-dev.23 HYPERSPACE between phases (canvas board "Hyperspace between phases"; user: "I like the
// movement animation"): stars stretch into streaks racing out from the middle, then a white flash.
// 512 thin angle lanes, about a quarter lit; per pixel it is one compare per side.
static constexpr int JLANES = 512, JUMP_MS = 1100;
static uint16_t jHead[JLANES], jLen[JLANES], jGain[JLANES];
static uint32_t jumpAt = 0;
static bool jumpOn = false;
static int lastPhase = -1;
static uint32_t sweepAt = 0;                    // the phase-change sweep started (0 = none)

struct Seg { float x1, y1, x2, y2, hw, gain; };
static Seg segs[48];
static int segCount = 0;
static uint16_t spotGain[8];

// 2.0-dev.16: where the glow's lookup tables live. Internal RAM is fastest but
// the table's Wi-Fi needs it: with 4 phones in busy play it ran to 0 KB free
// (RAM test 2026-11-03) and the tables are ~18 KB of it.
// 0 = all internal (as before), 1 = angle masks (10 KB) in PSRAM, 2 = all in PSRAM.
#ifndef TR_TABLES_PSRAM
#define TR_TABLES_PSRAM 2
#endif
static void *allocFast(size_t n) {
  void *p = TR_TABLES_PSRAM >= 2 ? nullptr : heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

static inline float lineGlow(float d, float hw, float sigma, float amp) {
  const float line = constrain(hw + 0.5f - fabsf(d), 0.0f, 1.0f);
  return line + amp * expf(-(d * d) / (2 * sigma * sigma));
}
static inline uint16_t toFixed(float v) { return (uint16_t)constrain(v * ONE, 0.0f, (float)IMAX); }
static inline float approachf(float c, float t, float r) { return c + (t - c) * r; }
static inline float binToDeg(int ai) { return ai * 360.0f / ABINS - 90.0f; }  // ABINS -> screen degrees
static inline int degToBin(float deg) {
  int a = (int)lroundf((deg + 90.0f) / 360.0f * ABINS);
  return a & (ABINS - 1);
}
static inline float angDiff(float a, float b) {  // a - b, wrapped to -180..180
  float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
  return d;
}
static inline void addSeg(float x1, float y1, float x2, float y2, float hw, float gain) {
  if (segCount < 48) segs[segCount++] = { x1, y1, x2, y2, hw, gain };
}
static inline void polar(float r, float deg, float &x, float &y) {
  const float a = deg * 0.0174533f;
  x = CX + r * cosf(a);
  y = CY + r * sinf(a);
}

static bool begin() {
  if (ready) return true;
  int n = 0;
  for (int qy = 0; qy < QN; qy++) {
    const float dy = qy + 0.5f;
    quadLen[qy] = (int16_t)min(CX, (int)max(0.0f, sqrtf((float)RMAX * RMAX - dy * dy) + 0.5f));
    n += quadLen[qy];
  }
  quad = (uint32_t *)heap_caps_malloc(n * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
  bandOf = (uint8_t *)allocFast(RBINS);
  radTab = (uint16_t *)allocFast(RBINS * 2);
  profTab = (uint16_t *)allocFast(RBINS * 2);
  toneMap = (uint16_t *)allocFast((IMAX + 1) * 2);
  rowI = (uint16_t *)allocFast(W * 2);
  bool ok = quad && bandOf && radTab && profTab && toneMap && rowI;
  // v0.27.2: the angle masks are read for every ring pixel -- internal RAM when
  // there is room (keeping 24 KB for Wi-Fi), else PSRAM.
  const size_t angBytes = (size_t)ABINS * (BAND_COUNT - 1);
  uint8_t *angBlock = TR_TABLES_PSRAM == 0 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > angBytes + 24 * 1024
                        ? (uint8_t *)heap_caps_malloc(angBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : nullptr;
  angInternal = angBlock != nullptr;
  if (!angBlock) angBlock = (uint8_t *)heap_caps_malloc(angBytes, MALLOC_CAP_SPIRAM);
  ok = ok && angBlock;
  for (int b = 1; b < BAND_COUNT && angBlock; b++) angTab[b] = angBlock + (b - 1) * ABINS;
  for (int i = 0; i < 4; i++) { toneF[i] = (uint16_t *)heap_caps_malloc((IMAX + 1) * 2, MALLOC_CAP_SPIRAM); ok = ok && toneF[i]; }
  gateAng = (uint8_t *)heap_caps_calloc(ABINS, 1, MALLOC_CAP_SPIRAM);
  gateR = (uint8_t *)heap_caps_calloc(RBINS, 1, MALLOC_CAP_SPIRAM);
  ok = ok && gateAng && gateR;
  segIdx = (uint8_t *)heap_caps_malloc(ABINS, MALLOC_CAP_SPIRAM);
  segMask = (uint8_t *)heap_caps_malloc(ABINS, MALLOC_CAP_SPIRAM);
  ringSpr = (uint8_t *)heap_caps_malloc(SPR * SPR, MALLOC_CAP_SPIRAM);
  if (!ok || !segIdx || !segMask || !ringSpr) {
    Serial.println("[TR] out of memory -- glow renderer off");
    return false;
  }
  uint32_t *pp = quad;
  for (int qy = 0; qy < QN; qy++) {
    quadRow[qy] = pp;
    const float dy = qy + 0.5f;
    for (int qx = 0; qx < quadLen[qy]; qx++) {
      const float dx = qx + 0.5f;
      const int ri = min(RBINS - 1, (int)(sqrtf(dx * dx + dy * dy) * RQ + 0.5f));
      const int aq = min(ABINS / 4 - 1, (int)(atan2f(dy, dx) / (2 * (float)M_PI) * ABINS));
      *pp++ = (uint32_t)ri | ((uint32_t)aq << 16);
    }
  }
  for (int ri = 0; ri < RBINS; ri++) {
    const float r = (float)ri / RQ;
    uint8_t b = B_NONE;
    if (fabsf(r - PHASE_R) <= 10) b = B_PHASE;
    else if (fabsf(r - DOOR_IN) <= 10 || fabsf(r - DOOR_OUT) <= 10) b = B_DOOR;
    else if (fabsf(r - RING_R) <= 10) b = B_RING;
    else if (fabsf(r - BAR_R) <= 8) b = B_BAR;
    else if (fabsf(r - RIM_R) <= 8) b = B_RIM;
    bandOf[ri] = b;
    gateR[ri] = (r >= DOOR_IN - 12 && r <= DOOR_OUT + 12) ? 1 : 0;
  }
  for (int y = 0; y < SPR; y++)
    for (int x = 0; x < SPR; x++) {
      const float dx = x - SPR / 2 + 0.5f, dy = y - SPR / 2 + 0.5f, d = sqrtf(dx * dx + dy * dy) - 47;
      ringSpr[y * SPR + x] = (uint8_t)min(255.0f, 255 * lineGlow(d, 1.2f, 4.5f, 0.55f) / 1.6f);
    }
  ready = true;
  Serial.printf("[TR] glow renderer ready (%d px table, internal heap %u KB free, angle masks in %s)\n", n,
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), angInternal ? "internal RAM" : "PSRAM");
  return true;
}

static void buildSegments(int total) {
  if (total == segTotal || total <= 0) return;
  segTotal = total;
  const float segW = (float)ABINS / total, gap = 30 * AK, soft = 6 * AK;
  for (int ai = 0; ai < ABINS; ai++) {
    const float pos = fmodf((float)ai, segW);  // round 1 starts at 12 o'clock
    segIdx[ai] = (uint8_t)min(total - 1, (int)(ai / segW));
    const float edge = min(pos, segW - pos) - gap / 2;
    segMask[ai] = (uint8_t)(constrain(edge / soft, 0.0f, 1.0f) * 255);
  }
}

// A comet's tail on an angular mask: brightest at `headDeg`, fading behind it.
static void cometOn(uint8_t *a, float headDeg, int tail, float bright) {
  tail = max(1, (int)(tail * AK));
  const int head = degToBin(headDeg);
  for (int d = 0; d < tail; d++) {
    const float f = 1 - (float)d / tail;
    uint8_t &p = a[(head - d) & (ABINS - 1)];
    p = (uint8_t)max((float)p, 255 * bright * f * f);
  }
}

// ---------- per-frame tables ----------
static void animate(uint32_t now) {
  TrScene &s = tr_scene;
  const float t = now / 1000.0f, mt = (now - s.modeStart) / 1000.0f;
  const float intro = constrain(mt / 0.9f, 0.0f, 1.0f);           // rings draw themselves on
  const float breathe = 0.5f + 0.5f * sinf(t * 1.4f);
  const bool gates = s.mode == TR_CLOCK || s.mode == TR_FIRST;

  const float tr = ((s.color >> 16) & 0xFF), tg = ((s.color >> 8) & 0xFF), tb = (s.color & 0xFF);
  if (!colSet) { colR = tr; colG = tg; colB = tb; colSet = true; }
  colR = approachf(colR, tr, 0.10f);
  colG = approachf(colG, tg, 0.10f);
  colB = approachf(colB, tb, 0.10f);

  // Phase change: a sweep of light round the face (user: "I love the idea of the sweep").
  if (s.mode == TR_CLOCK && s.phase != lastPhase) {
    if (lastPhase >= 0) sweepAt = jumpAt = now ? now : 1;
    lastPhase = s.phase;
  }
  if (s.mode != TR_CLOCK) lastPhase = -1;
  const float sw = sweepAt ? (now - sweepAt) / 1600.0f : 1.0f;
  if (sw >= 1.0f) sweepAt = 0;
  const float sweepHead = 270 + 360 * min(sw, 1.0f);

  float halo = 0.12f + 0.03f * breathe;
  if (s.mode == TR_ALERT) halo = 0.26f + 0.10f * sinf(t * 5.0f);
  else if (s.mode == TR_VICTORY) halo = 0.24f + 0.06f * breathe;
  halo *= intro;
  const float jp = jumpAt ? (now - jumpAt) / (float)JUMP_MS : 1.0f;
  jumpOn = jp < 1.0f;
  if (!jumpOn) jumpAt = 0;
  float flash = 0;
  if (jumpOn) {
    const float e = jp * jp;  // accelerating
    flash = jp > 0.72f ? sinf((jp - 0.72f) / 0.28f * 3.14159f) : 0;
    const float fade = jp < 0.8f ? 1.0f : (1.0f - jp) / 0.2f;
    for (int l = 0; l < JLANES; l++) {
      uint32_t h = (uint32_t)l * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
      if ((h & 3) != 0) { jGain[l] = 0; continue; }
      const float sp = 0.55f + 0.45f * ((h >> 8) & 255) / 255.0f, head = 24 + 250 * e * sp;
      const float len = fminf(6 + 150 * jp * sp, head - 16);
      jHead[l] = (uint16_t)(head * RQ); jLen[l] = (uint16_t)(fmaxf(len, 1.0f) * RQ);
      jGain[l] = (uint16_t)(ONE * (0.6f + 0.9f * sp) * fade);
    }
  }
  const float rip = fmodf(t, 1.6f) / 1.6f;
  for (int ri = 0; ri < RBINS; ri++) {
    const float r = (float)ri / RQ;
    float v = halo * expf(-(r * r) / (2 * 120.0f * 120.0f));
    if (flash > 0) v += flash * 1.8f * expf(-(r * r) / (2 * 150.0f * 150.0f));  // the jump's white flash
    // 3.1-dev.24: no core ring (the boards have none)
    if (s.ripple) v += (1 - rip) * 0.9f * lineGlow(r - (CORE_R + 3 + 14 * rip), 0.8f, 4, 0.5f);
    radTab[ri] = toFixed(v);
    float p = 0;
    switch (bandOf[ri]) {
      case B_PHASE: p = lineGlow(r - PHASE_R, 2.8f, 5, 0.45f); break;
      case B_DOOR:  p = 2 * lineGlow(r - DOOR_IN, 3.0f, 4, 0.5f); break;  // 3.1-dev.24: one 7 px arc  // 2.0-dev.11: masks are half-scale (room for the locked bloom)
      case B_RING:  p = lineGlow(r - RING_R, 0.9f, 3.5f, 0.4f); break;
      case B_BAR:   p = lineGlow(r - BAR_R, 2.4f, 4.5f, 0.5f); break;
      case B_RIM:   p = lineGlow(r - RIM_R, 1.9f, 4, 0.45f); break;
      default: break;
    }
    profTab[ri] = toFixed(p);
  }

  // Phase ring: three thirds from 12 o'clock, the current one lit, drawn on clockwise.
  {
    uint8_t *a = angTab[B_PHASE];
    const float third = ABINS / 3.0f;
    const int reveal = (int)(intro * ABINS);
    const float lit = sweepAt ? min(sw * 1.4f, 1.0f) : 1.0f;  // the new third fills behind the sweep
    for (int ai = 0; ai < ABINS; ai++) {
      float v = 0;
      if (s.phase >= 0 && !s.noPhaseRing && ai <= reveal) {
        const int i = min(2, (int)(ai / third));
        const float pos = ai - i * third, edge = min(pos, third - pos) - 22 * AK;
        v = (i == s.phase ? (0.78f + 0.22f * breathe) * lit : 0.09f) * constrain(edge / (8 * AK), 0.0f, 1.0f);
      }
      a[ai] = (uint8_t)(v * 255);
    }
    if (sweepAt) cometOn(a, sweepHead, 800, 1.0f);
  }

  // Rim: one segment per round.
  buildSegments(s.rimTotal);
  {
    uint8_t *a = angTab[B_RIM];
    const int reveal = (int)(intro * ABINS);
    const float pulse = 0.5f + 0.5f * sinf(t * 5.0f);
    for (int ai = 0; ai < ABINS; ai++) {
      float v;
      if (s.mode == TR_ALERT) {  // light chasing round the rim
        float f = fmodf(ai * 6.0f / ABINS - t * 2.4f + 600.0f, 1.0f);
        v = 0.10f + 0.90f * f * f * f * f;
      } else if (s.mode == TR_VICTORY) {
        v = (0.45f + 0.55f * (0.5f + 0.5f * sinf(t * 3.0f - segIdx[ai] * 0.8f))) * segMask[ai] / 255.0f;
      } else if (s.mode == TR_FIRST) {
        v = 0.14f * segMask[ai] / 255.0f;
      } else {
        v = s.rimFinal ? 0.45f + 0.40f * pulse : 0.62f + 0.10f * breathe;  // 3.1-dev.24: one solid rim (the boards)
      }
      a[ai] = ai <= reveal ? (uint8_t)(v * 255) : 0;
    }
    if (sweepAt) cometOn(a, sweepHead, 560, 0.9f);
  }

  // Gates: doors ease toward their state; the ring runs between the gates.
  uint8_t *door = angTab[B_DOOR], *ring = angTab[B_RING], *bar = angTab[B_BAR];
  memset(door, 0, ABINS);
  memset(bar, 0, ABINS);
  segCount = 0;
  // Lock-in (2.0-dev.11, user's pick "B + C" -- the shapes stay, only brightness
  // and color change): open gates go quiet, a locked gate's doors bloom
  // white-hot in its owner's color; locking in flashes them brighter still.
  // (dev.7's seal + sparks are gone -- they added lines.)
  static uint8_t lastState[4] = { 255, 255, 255, 255 };
  static uint32_t lockAt[4] = { 0, 0, 0, 0 };
  float lockFlash[4] = { 0, 0, 0, 0 };
  for (int i = 0; i < 4; i++) {
    const uint8_t stNow = i < s.seatCount ? s.seat[i].state : 255;
    if (s.mode == TR_CLOCK && mt > 1.0f && stNow == TR_SEAT_SHUT && lastState[i] != TR_SEAT_SHUT && lastState[i] != 255) lockAt[i] = now ? now : 1;
    lastState[i] = stNow;
    if (lockAt[i]) {
      const float f = (now - lockAt[i]) / 900.0f;
      if (f >= 1) lockAt[i] = 0;
      else lockFlash[i] = 1 - f;
    }
  }
  if (gates) {
    const float landed = s.mode == TR_FIRST && mt > 4.0f;
    for (int i = 0; i < s.seatCount && i < 4; i++) {
      const TrSeat &st = s.seat[i];
      float target = st.state == TR_SEAT_SHUT ? 1.0f : st.state == TR_SEAT_WAITING ? 0.25f * (0.5f + 0.5f * sinf(t * 2.6f)) : 0.1f;
      if (st.first && landed) target = 1.0f;  // who goes first: that gate slams shut
      doorPos[i] = approachf(doorPos[i], target, 0.18f);
      barNow[i] = approachf(barNow[i], st.barFrac / 255.0f, 0.06f);
    }
    // v0.27.0: only the angles near each gate are touched (looping every bin
    // against every gate cost ~35 ms a frame).
    memset(ring, 0, ABINS);  // 3.1-dev.24: no ring between the gates (the comet still runs on it)
    constexpr float BIN_DEG = 360.0f / ABINS;
    for (int i = 0; i < s.seatCount && i < 4; i++) {
      const TrSeat &st = s.seat[i];
      const int c = degToBin(st.angle);
      const float bh = st.barHalf * barNow[i];
      const bool bars = st.barHalf > 0 && (st.state != TR_SEAT_EMPTY || st.away) && s.mode == TR_CLOCK;
      const int nb = (int)((max((float)st.half + 2, bars ? bh + 2 : 0.0f)) / BIN_DEG) + 1;
      const float open = (1 - doorPos[i]) * (st.half - 6) * 0.55f;
      // door brightness relative to before dev.11: locked 1.8x (+ a flash), open 0.55x
      float lv = (st.state == TR_SEAT_SHUT && s.mode == TR_CLOCK) ? 0.90f + 0.10f * lockFlash[i] : 0.5f;
      if (s.mode != TR_CLOCK) lv = 0.5f;  // other modes look as they always did
      else if (st.state != TR_SEAT_SHUT) lv = 0.275f;
      if (st.dim) lv *= 0.40f;
      if (st.fight) lv *= 0.65f + 0.35f * (0.5f + 0.5f * sinf(t * 6.3f));
      const float barLv = (st.top ? 1.0f : 0.55f) * (st.dim ? 0.45f : 1.0f);
      for (int k = -nb; k <= nb; k++) {
        const int ai = (c + k) & (ABINS - 1);
        const float d = fabsf(k * BIN_DEG);
        if (d < st.half + 1.5f) ring[ai] = (uint8_t)(ring[ai] * constrain((d - st.half) / 1.5f, 0.0f, 1.0f));  // the ring stops at the gate
        if (d <= st.half) {
          float dv;
          // 3.1-dev.24 (the boards): open = a dashed dim arc, locked = solid and glowing; locking in
          // fills the dashes in as the gate "closes" (doorPos 0 -> 1), then flashes.
          const float edge = constrain(st.half - d + 0.4f, 0.0f, 1.0f);
          if (st.state == TR_SEAT_EMPTY) dv = ((int)d % 6) < 3 ? 0.10f : 0.0f;  // an empty seat: dashed, dark
          else {
            const float dash = fmodf(d, 2.7f) < 1.0f ? (st.state == TR_SEAT_WAITING ? 0.22f + 0.22f * (0.5f + 0.5f * sinf(t * 2.6f)) : 0.24f) : 0.0f;
            const float solid = (s.mode == TR_CLOCK ? 0.90f + 0.10f * lockFlash[i] : 0.6f) * (st.dim ? 0.4f : 1.0f);
            dv = (dash * (1 - doorPos[i]) + solid * doorPos[i]) * edge;
          }
          (void)open; (void)lv;
          door[ai] = max(door[ai], (uint8_t)(dv * 255));
        }
        if (bars && d <= bh + 1.0f) bar[ai] = max(bar[ai], (uint8_t)(barLv * constrain(bh + 1.0f - d, 0.0f, 1.0f) * 255));
      }
    }
    // 2.0-dev.11: which angles belong to a locked gate that glows in its owner's color.
    memset(gateAng, 0, ABINS);
    if (s.mode == TR_CLOCK) {
      for (int i = 0; i < s.seatCount && i < 4; i++) {
        const TrSeat &st = s.seat[i];
        if (st.state != TR_SEAT_SHUT || !st.color) continue;
        const int c = degToBin(st.angle), nb = (int)((st.half + 1.0f) / BIN_DEG);
        for (int k = -nb; k <= nb; k++) gateAng[(c + k) & (ABINS - 1)] = (uint8_t)(i + 1);
        if (toneFFor[i] != st.color) {
          toneFFor[i] = st.color;
          const float fr = (st.color >> 16) & 0xFF, fg = (st.color >> 8) & 0xFF, fb = st.color & 0xFF;
          for (int j = 0; j <= IMAX; j++) {
            const float I = (float)j / ONE, base = min(I, 1.0f), w = constrain((I - 1.0f) / 1.6f, 0.0f, 0.85f);
            const uint16_t cc = ((uint16_t)(fr * base + (255 - fr * base) * w) & 0xF8) << 8 | ((uint16_t)(fg * base + (255 - fg * base) * w) & 0xFC) << 3 |
                                (uint16_t)(fb * base + (255 - fb * base) * w) >> 3;
            toneF[i][j] = (cc >> 8) | (cc << 8);
          }
        }
      }
    }
    // (v0.27.2: the gate walls and the 2-player divider are drawn once into the overlay.)
    if (sweepAt) cometOn(ring, sweepHead, 450, 0.9f);
    if (s.mode == TR_FIRST && mt < 4.0f) {  // who goes first: a comet runs the gate ring, easing onto its gate
      const float p = constrain(mt / 4.0f, 0.0f, 1.0f), e = 1 - (1 - p) * (1 - p) * (1 - p);
      cometOn(ring, s.cometAngle - 1080 * (1 - e), 520, 1.0f);
    }
  } else {
    memset(ring, 0, ABINS);
  }

  for (int i = 0; i < s.spotCount && i < 8; i++) {
    float g = 0;
    switch (s.spot[i].kind) {
      case TR_SPOT_BREATHE: g = 0.15f + 0.85f * (0.5f + 0.5f * sinf(t * 2.6f)); break;
      case TR_SPOT_PULSE:   g = 0.30f + 0.70f * (0.5f + 0.5f * sinf(t * 6.3f)); break;
      default:              g = mt > 4.0f ? 0.6f + 0.4f * (0.5f + 0.5f * sinf(t * 6.0f)) : 0; break;
    }
    spotGain[i] = (uint16_t)(g * 360);
  }

  // Blaster bolts, fireworks.
  for (int b = 0; b < s.boltCount && b < 2; b++) {
    const TrBolt &bo = s.bolt[b];
    const float dx = bo.x2 - bo.x1, dy = bo.y2 - bo.y1, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) continue;
    const float ux = dx / len, uy = dy / len;
    for (int k = 0; k < 3; k++) {
      const float d = fmodf(now * 0.22f + k * len / 3, len), d2 = fminf(d + 18, len);
      addSeg(bo.x1 + ux * d, bo.y1 + uy * d, bo.x1 + ux * d2, bo.y1 + uy * d2, 1.6f, 2.2f);
    }
  }
  if (s.fireworks) {
    static const int FW[4][2] = { { 150, 150 }, { 316, 150 }, { 316, 316 }, { 150, 316 } };
    for (int k = 0; k < 4; k++) {
      const float f = ((now + k * 450) % 1800) / 1800.0f;
      for (int j = 0; j < 8; j++) {
        const float a = j * 0.7854f, r0 = 6 + 30 * f, r1 = 12 + 40 * f;
        addSeg(FW[k][0] + r0 * cosf(a), FW[k][1] + r0 * sinf(a), FW[k][0] + r1 * cosf(a), FW[k][1] + r1 * sinf(a), 1.2f, 2.0f * (1 - f));
      }
    }
  }

  // Tone map: the mood color up to 1.0, then blooming toward white.
  // 2.0-dev.16: rebuilt only when the (whole-number) color changes -- it was
  // 1025 float entries every frame while the color sat still.
  static int lastR = -1, lastG = -1, lastB = -1;
  const int qR = (int)(colR + 0.5f), qG = (int)(colG + 0.5f), qB = (int)(colB + 0.5f);
  if (qR == lastR && qG == lastG && qB == lastB) return;
  lastR = qR; lastG = qG; lastB = qB;
  for (int i = 0; i <= IMAX; i++) {
    const float I = (float)i / ONE, base = min(I, 1.0f), w = constrain((I - 1.0f) / 1.6f, 0.0f, 0.85f);
    const float r = qR * base + (255 - qR * base) * w;
    const float g = qG * base + (255 - qG * base) * w;
    const float b = qB * base + (255 - qB * base) * w;
    const uint16_t c = ((uint16_t)r & 0xF8) << 8 | ((uint16_t)g & 0xFC) << 3 | (uint16_t)b >> 3;
    toneMap[i] = (c >> 8) | (c << 8);  // the panel takes big-endian pixels
  }
}

// ---------- one row ----------
static void renderRow(uint16_t *dst, int y) {
  const TrScene &s = tr_scene;
  const bool top = y < CY;
  const int qy = top ? CY - 1 - y : y - CY;
  const int n = quadLen[qy];
  const uint32_t *q = quadRow[qy];
  const int rBase = ABINS / 4, lBase = 3 * ABINS / 4, sgn = top ? -1 : 1;
  uint16_t *right = rowI + CX, *left = rowI + CX - 1;
  for (int k = 0; k < n; k++) {
    const uint32_t pv = q[k];
    const uint32_t ri = pv & 0xFFFF;
    const int aq = (int)(pv >> 16);
    uint32_t Ir = radTab[ri], Il = Ir;
    const uint8_t b = bandOf[ri];
    if (b) {
      const uint32_t pr = profTab[ri];
      const uint8_t *at = angTab[b];
      Ir += (pr * at[(rBase + sgn * aq) & (ABINS - 1)]) >> 8;
      Il += (pr * at[(lBase - sgn * aq) & (ABINS - 1)]) >> 8;
    }
    if (jumpOn) {
      const int lr = ((rBase + sgn * aq) & (ABINS - 1)) >> 2, ll = ((lBase - sgn * aq) & (ABINS - 1)) >> 2;
      if (jGain[lr] && (uint32_t)((int)jHead[lr] - (int)ri) < jLen[lr]) Ir += jGain[lr];
      if (jGain[ll] && (uint32_t)((int)jHead[ll] - (int)ri) < jLen[ll]) Il += jGain[ll];
    }
    right[k] = (uint16_t)Ir;
    left[-k] = (uint16_t)Il;
    if (gateR[ri]) {  // 2.0-dev.11: inside a locked gate's span -> its owner's tone map
      rowM[CX + k] = gateAng[(rBase + sgn * aq) & (ABINS - 1)];
      rowM[CX - 1 - k] = gateAng[(lBase - sgn * aq) & (ABINS - 1)];
    } else {
      rowM[CX + k] = rowM[CX - 1 - k] = 0;
    }
  }
  const int x0 = CX - n, x1 = CX + n;  // inside the circle: [x0, x1)

  // The pixel planet replaces the glow inside its disc.
  const int pc = s.planetCell ? s.planetCell : 5, ph = 14 * pc;
  if (s.planet >= 0 && s.planetPx && y >= CY - ph && y < CY + ph) {
    static const uint16_t PL[4] = { (uint16_t)(0.16f * ONE), (uint16_t)(0.40f * ONE), (uint16_t)(0.72f * ONE),
                                    (uint16_t)(1.15f * ONE) };
    const uint8_t *prow = s.planetPx + ((y - (CY - ph)) / pc) * 28;
    for (int x = CX - ph; x < CX + ph; x++) {
      const uint8_t c = prow[(x - (CX - ph)) / pc];
      if (c < 4) rowI[x] = PL[c];
    }
  }
  for (int i = 0; i < s.spotCount && i < 8; i++) {
    const int sy = y - (s.spot[i].y - SPR / 2);
    if (sy < 0 || sy >= SPR || !spotGain[i]) continue;
    const uint8_t *sr = ringSpr + sy * SPR;
    const int sx0 = s.spot[i].x - SPR / 2;
    const uint32_t g = spotGain[i];
    for (int x = max(sx0, x0); x < min(sx0 + SPR, x1); x++) rowI[x] += (uint16_t)((sr[x - sx0] * g) >> 8);
  }
  // Sharp parts: symbols, numbers, words, dice, ships.
  if (ovl && ovX0[y] <= ovX1[y]) {
    const uint8_t *orow = ovl + (size_t)y * ovlStride;
    for (int x = ovX0[y]; x <= ovX1[y]; x++) {
      const uint8_t v = orow[x];
      if (v) rowI[x] += (uint16_t)((v * 9) >> 2);  // 255 -> 2.24 (white-hot)
    }
  }
  // Line work: gate walls, dividers, bolts, fireworks -- anti-aliased with a glow.
  for (int i = 0; i < segCount; i++) {
    const Seg &g = segs[i];
    if (g.gain <= 0.01f) continue;
    const float reach = g.hw + 4;
    if (y < fminf(g.y1, g.y2) - reach || y > fmaxf(g.y1, g.y2) + reach) continue;
    const float dx = g.x2 - g.x1, dy = g.y2 - g.y1, l2 = dx * dx + dy * dy;
    const int xa = max(x0, (int)(fminf(g.x1, g.x2) - reach)), xb = min(x1 - 1, (int)(fmaxf(g.x1, g.x2) + reach));
    for (int x = xa; x <= xb; x++) {
      float u = l2 > 0 ? ((x - g.x1) * dx + (y - g.y1) * dy) / l2 : 0;
      u = constrain(u, 0.0f, 1.0f);
      const float px = g.x1 + u * dx - x, py = g.y1 + u * dy - y, d = sqrtf(px * px + py * py);
      if (d > reach) continue;
      rowI[x] += (uint16_t)(g.gain * lineGlow(d, g.hw, 1.8f, 0.4f) * ONE);
    }
  }
  memset(dst, 0, x0 * 2);
  for (int x = x0; x < x1; x++) {
    const uint16_t I = rowI[x] > IMAX ? IMAX : rowI[x];
    const uint8_t m = rowM[x];
    dst[x] = m ? toneF[m - 1][I] : toneMap[I];
  }
  memset(dst + x1, 0, (W - x1) * 2);
}

static uint32_t frameHook() {
  static uint32_t statAt = 0, frames = 0, busyUs = 0;
  const uint32_t now = millis();
  const uint32_t t0 = micros();
  if (tr_scene.preFrame) tr_scene.preFrame();
  animate(now);
  int k = 0;
  for (int y0 = 0; y0 < H; y0 += BSP_STRIP_ROWS) {
    const int rows = min(BSP_STRIP_ROWS, H - y0);
    bsp_frame_wait(k);
    healthFeed();  // 3.1-dev.32 (playtest on a busy Wi-Fi 7 router: the Wi-Fi driver held this core long enough that
                   // ONE frame passed the 8 s watchdog -> restart). A slow frame is now just slow.
    uint16_t *dst = bsp_strip_buffer(k);
    for (int r = 0; r < rows; r++) renderRow(dst + r * W, y0 + r);
    bsp_frame_strip(k, y0, rows);
    k ^= 1;
  }
  bsp_frame_wait(0);
  bsp_frame_wait(1);
  const uint32_t us = micros() - t0;
  busyUs += us;
  frames++;
  if (now - statAt >= 5000) {
    if (statAt) Serial.printf("[TR] 5s: %.1f fps, %u ms per frame\n", frames * 1000.0f / (now - statAt),
                              (unsigned)(frames ? busyUs / frames / 1000 : 0));
    statAt = now;
    frames = busyUs = 0;
  }
  const uint32_t ms = us / 1000;
  return ms >= 50 ? 15 : 65 - ms;  // 3.1-dev.32: ~15 fps and always >= 15 ms for Wi-Fi and the other tasks (was ~25 fps / 4 ms)
}

void trStart() {
  if (running || bsp_screen_off) return;  // screen off: no frames to draw (and the CPU goes to the phones)
  if (!begin()) return;
  colSet = false;
  lastPhase = -1;
  sweepAt = 0;
  for (int i = 0; i < 4; i++) { doorPos[i] = 0.1f; barNow[i] = 0; }
  running = true;
  bsp_set_frame_hook(frameHook);
}

void trStop() {
  if (!running) return;
  running = false;
  bsp_set_frame_hook(nullptr);
}

bool trActive() { return running; }

void trOverlayChanged(const uint8_t *l8, int stride) {
  ovl = l8;
  ovlStride = stride;
  for (int y = 0; y < H; y++) {
    int a = W, b = -1;
    if (l8) {
      const uint8_t *r = l8 + (size_t)y * stride;
      for (int x = 0; x < W; x++) if (r[x]) { if (a == W) a = x; b = x; }
    }
    ovX0[y] = (int16_t)a;
    ovX1[y] = (int16_t)b;
  }
}
