#include <Arduino.h>
#include <string.h>
#include <math.h>
#include <Preferences.h>  // 2026-11-14 SCREENOFF / SCREENON (saved on this board)
#include "board_bsp.h"
#include "board_select.h"
#include "lvgl.h"
#include "game_state.h"
#include "web_server.h"
#include "version.h"
#include "commander_art.h"
#include "negotiation_bank.h"
#include "transmission_bank.h"  // v0.24.4: TxBig names for the table alerts
#include "table_render.h"      // v0.26.0: the table's glow renderer
#include "table_scenes.h"      // 3.1-dev.25: the table's screens (also built by tools/table_harness)
#include "ui_prims.h"         // 3.1-dev.25: lines, discs, faction symbols + colors (shared with table_scenes)
#include "console_link.h"
#include <esp_mac.h>
#include "sf_qrcodegen.h"
#include "sf_audio.h"
#include "sf_health.h"   // 2.0-dev.6: crash records + watchdog

// The WebServer (handling /join, /state, and every other HTTP endpoint) runs
// on Arduino's main "loopTask", a SEPARATE FreeRTOS task from the LVGL
// display task -- its stack is independent of LVGL_TASK_STACK_SIZE in
// bsp_config.h. GameState is now ~3.3KB per copy (grew substantially once
// reputation/obligation tracking were added), and sendStateJson() in
// web_server.cpp copies a full GameState plus builds a large JSON response
// via repeated String concatenation. The Arduino core's default loopTask
// stack is only 8KB. This overrides that default -- must be at file scope
// (it expands to a full function definition overriding a weak default in
// the ESP32 core), which is why it's placed here, before setup()/loop().
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// Shadow Fleet - Prototype 02
// Target: Waveshare ESP32-S3-Touch-AMOLED-1.43 (non-C variant)
// Built on the corrected BSP (fixed QSPI/reset/enable pins, fixed touch
// controller protocol) confirmed working via 00_Display_Test.
//
// Goal of this prototype:
//   - Prove display + touch
//   - Starfield background
//   - Main command console
//   - Fleet screen
//   - Planet screen
//   - Battle screen with animated dice/explosion
//
// This sketch intentionally uses only APIs demonstrated by the supplied
// Waveshare example/BSP. Game networking and real game state come later.

static lv_obj_t *screen_root = nullptr;
static lv_obj_t *title_label = nullptr;
static lv_obj_t *battle_label = nullptr;
static lv_obj_t *battle_button = nullptr;
static lv_obj_t *withdraw_button = nullptr;
static lv_obj_t *reinforce_button = nullptr;  // v0.28.0: PvP defender brings another fleet
static lv_timer_t *star_timer = nullptr;
static lv_timer_t *battle_timer = nullptr;
static bool onHomeScreen = false;
static lv_obj_t *phase_title_label = nullptr;
static lv_obj_t *phase_status_label = nullptr;
static lv_obj_t *lock_in_button = nullptr;

// The AMOLED's own player slot. Used to be a fixed #define (always
// HOST_SLOT/0) -- now the AMOLED joins the lobby exactly like a phone does,
// picking any slot 1-4, so this is a real variable set once the local join
// flow completes (see gameStateJoinSlot()). -1 means "not joined yet."
static int MY_SLOT = -1;

// Screen tracking so background timers know what to refresh.
enum ScreenId { SCR_START, SCR_LOBBY, SCR_HOME, SCR_PLANETS, SCR_PLANET_DETAIL,
                SCR_TAKEOVER, SCR_CONFIRM_PAY, SCR_NEGOTIATE, SCR_FLEETS, SCR_BATTLE, SCR_TRANSMISSION,
                SCR_REPUTATION, SCR_STANDINGS, SCR_GAME_OVER,
                SCR_SLOT_PICKER, SCR_FACTION_PICKER, SCR_BRIEFING,
                SCR_BANNER, SCR_LOCK_CONFIRM, SCR_VICTORY, SCR_COMMANDER_PICKER, SCR_TABLE, SCR_MARKETS,
                SCR_DEALS, SCR_DEAL_VIEW, SCR_DEAL_COMPOSE, SCR_ASHEN, SCR_END_CONFIRM };
static ScreenId currentScreen = SCR_START;
static uint32_t join_shown_key = 0;  // joinScreenKey() the seat/faction picker was built from (v0.21.18)
static int cmd_pick_i = 0;  // commander picker: 0..2 within your faction (v0.21.20)
static void showSlotPicker();
static void showFactionPicker();
static void showCommanderPicker();
static void showLobby();
static void showHome();
static uint32_t joinScreenKey(const GameState &gs);
static bool transmission_is_obligation = false;

// Faction display data (name/color/glyph placeholder). Moved up here since
// it's now used by both the Reputation screen and the Faction Transmission
// screen. Real portrait art can replace the glyph rendering later -- no
// other code needs to change, just what gets drawn in the portrait area.
struct FactionVisual {
  const char *name;
  uint32_t color;
  char glyph;
};
static const FactionVisual FACTION_VISUALS[4] = {
  { "REBEL ALLIANCE", 0xFFC933, 'R' },
  { "GALACTIC EMPIRE", 0xE6EEF5, 'E' },
  { "PYKE SYNDICATE", 0x9B5FE0, 'P' },
  { "CRIMSON DAWN", 0xB0202A, 'C' },
};

struct Star {
  int16_t x;
  int16_t y;
  uint8_t r;
  uint8_t brightness;
  int8_t dx;
};

constexpr size_t STAR_COUNT = 34;
Star stars[STAR_COUNT];

// v0.23.5: objects that animate on their own (spinning planets) -- starTick
// redraws just these now that it no longer redraws the whole screen.
// clearScreen() empties the list; screens add theirs with trackAnimated().
static lv_obj_t *anim_objs[4];
static int anim_obj_count = 0;
static void untrackAnimatedCb(lv_event_t *e) {  // deleted without a screen change -> drop it
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  for (int i = 0; i < anim_obj_count; i++)
    if (anim_objs[i] == o) { anim_objs[i] = anim_objs[--anim_obj_count]; break; }
}
static void trackAnimated(lv_obj_t *o) {
  if (anim_obj_count >= (int)(sizeof(anim_objs) / sizeof(anim_objs[0]))) return;
  anim_objs[anim_obj_count++] = o;
  lv_obj_add_event_cb(o, untrackAnimatedCb, LV_EVENT_DELETE, nullptr);
}

// Simple procedural RNG, seeded from esp_random().
static int randomInt(int minValue, int maxValue) {
  if (maxValue <= minValue) return minValue;
  return minValue + (int)(esp_random() % (uint32_t)(maxValue - minValue + 1));
}

static void setScreenBackground(lv_obj_t *obj) {
  lv_obj_set_style_bg_color(obj, lv_color_hex(0x02060D), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

// ---------------------------------------------------------------------------
// PHASE C, REVISED: text scaling now comes from a genuinely bigger default
// LVGL font (Montserrat 20, set as LV_FONT_DEFAULT in lv_conf.h) instead of
// the transform_scale "zoom trick" used earlier. That trick worked fine for
// a single decorative element but caused a real freeze on Home once ~7
// widgets used it simultaneously (each transformed widget needs an extra
// offscreen-buffer rendering pass in LVGL). A real font has none of that
// per-widget cost -- it just draws bigger glyphs directly -- and since
// nothing in this file explicitly overrides its font, EVERY label
// automatically renders at the new bigger size with zero code changes
// needed here. The style_big_text/applyBigText/makeButtonPlain machinery
// from the transform-scale approach is retired; makeButton() is back to
// one plain version used everywhere, freeze risk included.
// ---------------------------------------------------------------------------

static void styleButton(lv_obj_t *btn) {
  lv_obj_set_style_radius(btn, 14, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x081725), 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x12314A), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(btn, 2, 0);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(0xD9F5FF), 0);
  lv_obj_set_style_pad_all(btn, 12, 0);
  // Extend the touch-sensitive area well beyond the visible button on all
  // sides, so near-misses (and touch-calibration drift) still register as a
  // hit. Purely for input; doesn't change how the button looks.
  lv_obj_set_ext_click_area(btn, 26);
}

static void makeButton(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_pos(btn, x, y);
  lv_obj_set_size(btn, w, h);
  styleButton(btn);
  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
}

// ===========================================================================
// UI KIT (v0.21.0 UI refresh) -- the "holo-console" look from the approved
// mockups (canvas: claude.ai/artifact/AskaS3GeVAGCP7GcjiVCDi; design notes
// in the design notes). Rebuilt screens use these; screens not yet rebuilt keep
// styleButton()/makeButton() until their turn in the rollout.
// Touch targets: nothing tappable under ~72px; primary actions ~96px+;
// farthest corner <= ~227px from center (Convention 5).
// Font: Montserrat 20/14 for now -- the mockups' Oxanium needs LVGL's
// lv_font_conv (Node.js), a tooling decision left for the user.
// ===========================================================================
LV_FONT_DECLARE(sf_font_montserrat_12);  // project-local copy, sf_font_montserrat_12.c (v0.21.2)
LV_FONT_DECLARE(sf_font_montserrat_36);  // project-local copy, sf_font_montserrat_36.c (v0.21.18)
static const uint32_t UI_BG = 0x05070C;
static const uint32_t UI_RING = 0x18263A;
static const uint32_t UI_LINE = 0x2A3B50;
static const uint32_t UI_PANEL = 0x0B1420;
static const uint32_t UI_TEXT = 0xE6F1FA;
static const uint32_t UI_DIM = 0x7C93A8;
static const uint32_t UI_FAINT = 0x56697C;
static const uint32_t UI_CYAN = 0x5FD4FF;
static const uint32_t UI_RED = 0xFF6B5A;

// Per-faction colors: the insignia/ring color, and a lighter text-safe
// variant (Crimson Dawn's deep red and the Empire's gray read poorly as
// text on the dark ground).
// (FACTION_RING_COLOR / FACTION_TEXT_COLOR / FACTION_SHORT live in ui_prims.h since 3.1-dev.25.)

// The player's accent everywhere = their own faction's color.
static uint32_t uiAccentFor(int faction) {
  return (faction >= 0 && faction < 4) ? FACTION_TEXT_COLOR[faction] : UI_CYAN;
}

// Darkened tint of a color for button fills (~12%).
static uint32_t uiTint(uint32_t c) {
  uint32_t r = ((c >> 16) & 0xFF) * 12 / 100, g = ((c >> 8) & 0xFF) * 12 / 100, b = (c & 0xFF) * 12 / 100;
  return (r << 16) | (g << 8) | b;
}

// The greyed-out look of a button you can't use yet (2.0 frame-rate work).
// It used to be lv_obj_set_style_opa(obj, 50%), but a whole-object opacity
// makes LVGL render the object into an offscreen layer and blend it back on
// every redraw. On the near-black screen, mixing each color toward the
// background looks the same and costs nothing. Children first, so a label
// mixes its own (inherited) color, not one its parent already darkened.
static void uiDimColors(lv_obj_t *o, lv_opa_t keep) {
  const uint32_t n = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < n; i++) uiDimColors(lv_obj_get_child(o, i), keep);
  const lv_color_t bg = lv_color_hex(0x02060D);
  lv_obj_set_style_border_color(o, lv_color_mix(lv_obj_get_style_border_color(o, LV_PART_MAIN), bg, keep), 0);
  lv_obj_set_style_bg_color(o, lv_color_mix(lv_obj_get_style_bg_color(o, LV_PART_MAIN), bg, keep), 0);
  lv_obj_set_style_text_color(o, lv_color_mix(lv_obj_get_style_text_color(o, LV_PART_MAIN), bg, keep), 0);
}

static void uiStyleButton(lv_obj_t *btn, uint32_t border, int borderW, uint32_t fill, int radius) {
  lv_obj_remove_style_all(btn);
  lv_obj_set_style_bg_color(btn, lv_color_hex(fill), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x16283C), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn, lv_color_hex(border), 0);
  lv_obj_set_style_border_width(btn, borderW, 0);
  lv_obj_set_style_radius(btn, radius, 0);
  lv_obj_set_style_text_color(btn, lv_color_hex(UI_TEXT), 0);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);  // scrollable-by-default swallowed taps once
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(btn, 10);
}

// 2.0-dev.7: the screen's back button, pressed by the left-edge swipe (see
// edgeSwipeHook). Every back-button style registers itself with uiMarkBack.
static lv_obj_t *ui_back_btn = nullptr;
static lv_event_cb_t ui_back_fn = nullptr;
static void uiBackForgetCb(lv_event_t *e) {
  if (lv_event_get_target(e) == ui_back_btn) ui_back_btn = nullptr;
}
// 2.0-dev.8 (user: "the swiping is amazing. lets remove the back buttons"):
// arrow back buttons are still built -- the swipe presses them -- but hidden.
// STAY / KEEP PLAYING pills are real choices and stay visible (arrow = false).
static const bool UI_BACK_BUTTONS = false;
static void uiMarkBack(lv_obj_t *btn, bool arrow = true) {
  ui_back_btn = btn;
  lv_obj_add_event_cb(btn, uiBackForgetCb, LV_EVENT_DELETE, nullptr);
  if (arrow && !UI_BACK_BUTTONS) lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

// Round button centered at (cx, cy). `emphasis` = accent border + colored
// label (the screen's main action); otherwise a quiet hairline border.
static lv_obj_t *uiCircleButton(lv_obj_t *parent, int cx, int cy, int d, const char *text,
                                uint32_t color, bool emphasis, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  uiStyleButton(btn, emphasis ? color : 0x2F4A66, 2, emphasis ? uiTint(color) : UI_PANEL, LV_RADIUS_CIRCLE);
  lv_obj_set_size(btn, d, d);
  lv_obj_set_pos(btn, cx - d / 2, cy - d / 2);
  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_hex(emphasis ? color : UI_TEXT), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  if (cb && strcmp(text, LV_SYMBOL_LEFT) == 0) uiMarkBack(btn);  // a back arrow
  return btn;
}

// Pill button with a main label (child 0) and an optional small subline
// (child 1, Montserrat 14).
static lv_obj_t *uiPill(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                        const char *sub, uint32_t color, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  uiStyleButton(btn, color, 2, uiTint(color), h / 2);
  lv_obj_set_size(btn, w, h);
  lv_obj_set_pos(btn, x, y);
  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_letter_space(l, 2, 0);
  lv_obj_t *s = lv_label_create(btn);
  lv_label_set_text(s, sub ? sub : "");
  lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s, lv_color_hex(UI_DIM), 0);
  if (sub && sub[0]) {
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -9);
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 14);
  } else {
    lv_obj_center(l);
    lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
  }
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
  return btn;
}

// 72px round "back" button with a chevron.
// 2.0-dev.7 (user: "we could get rid of every back button if we could swipe
// from the left side as back reliably"): a drag that starts within
// EDGE_SWIPE_START px of the left edge and goes EDGE_SWIPE_DIST px right
// (mostly sideways) does what the screen's back button does. Step 1: the
// buttons stay until the swipe has proven itself. The screen's back action
// is its uiBackButton, or ui_back_fn for screens with their own (the Galaxy).
static const int EDGE_SWIPE_START = 60, EDGE_SWIPE_DIST = 70;  // px in from the round glass's left edge at that height; px dragged

static lv_obj_t *uiBackButton(lv_obj_t *parent, int cx, int cy, lv_event_cb_t cb) {
  lv_obj_t *btn = lv_btn_create(parent);
  uiStyleButton(btn, UI_LINE, 1, UI_BG, LV_RADIUS_CIRCLE);
  lv_obj_set_size(btn, 72, 72);
  lv_obj_set_pos(btn, cx - 36, cy - 36);
  lv_obj_t *l = lv_label_create(btn);
  lv_label_set_text(l, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(l, lv_color_hex(UI_DIM), 0);
  lv_obj_center(l);
  if (cb) {
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    uiMarkBack(btn);  // the edge swipe presses this
  }
  return btn;
}

static lv_obj_t *edge_hint = nullptr;  // a "<" disc that follows the finger during an edge swipe
static void edgeSwipeBack(void *) {
  lv_indev_t *in = lv_indev_get_next(nullptr);
  if (in) { lv_indev_reset(in, nullptr); lv_indev_wait_release(in); }  // nothing under the finger gets the press
  if (edge_hint) lv_obj_add_flag(edge_hint, LV_OBJ_FLAG_HIDDEN);
  Serial.println("[SWIPE] back");
  if (ui_back_btn) lv_obj_send_event(ui_back_btn, LV_EVENT_CLICKED, nullptr);
  else if (ui_back_fn) ui_back_fn(nullptr);
}
static void edgeSwipeHook(int x, int y, bool down) {
  static bool wasDown = false, track = false, fired = false;
  static int sx = 0, sy = 0;
  if (!down) {
    wasDown = track = fired = false;
    if (edge_hint) lv_obj_add_flag(edge_hint, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (!wasDown) {  // a new press
    wasDown = true;
    sx = x; sy = y;
    // The glass is round: its left edge at height y is 233 - sqrt(233^2 - (y-233)^2).
    const int dyc = y - 233;
    const int edgeX = 233 - (int)sqrtf((float)max(0, 233 * 233 - dyc * dyc));
    track = x < edgeX + EDGE_SWIPE_START && (ui_back_btn || ui_back_fn);
    return;
  }
  if (!track || fired) return;
  const int dx = x - sx, dy = abs(y - sy);
  if (dy > 80 || dx < -10) { track = false; if (edge_hint) lv_obj_add_flag(edge_hint, LV_OBJ_FLAG_HIDDEN); return; }
  if (dx > 16) {
    if (!edge_hint) {
      edge_hint = lv_obj_create(lv_layer_top());
      lv_obj_remove_style_all(edge_hint);
      lv_obj_set_size(edge_hint, 48, 48);
      lv_obj_set_style_radius(edge_hint, LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(edge_hint, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(edge_hint, 2, 0);
      lv_obj_clear_flag(edge_hint, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_t *l = lv_label_create(edge_hint);
      lv_label_set_text(l, LV_SYMBOL_LEFT);
      lv_obj_center(l);
    }
    const bool ready = dx >= EDGE_SWIPE_DIST;
    const uint32_t col = ready ? UI_CYAN : UI_DIM;
    lv_obj_set_style_bg_color(edge_hint, lv_color_hex(ready ? 0x0E2A36 : UI_BG), 0);
    lv_obj_set_style_border_color(edge_hint, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(edge_hint, 0), lv_color_hex(col), 0);
    lv_obj_set_pos(edge_hint, min(x, sx + EDGE_SWIPE_DIST) - 24, sy - 24);
    lv_obj_clear_flag(edge_hint, LV_OBJ_FLAG_HIDDEN);
  }
  if (dx >= EDGE_SWIPE_DIST && dy < dx) {
    fired = true;
    lv_async_call(edgeSwipeBack, nullptr);  // not from inside the touch read
  }
}

static lv_obj_t *uiLabel(lv_obj_t *parent, const char *text, uint32_t color, const lv_font_t *font,
                         int y, int letterSpace) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_letter_space(l, letterSpace, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}

// 2.0 frame rate: LVGL queues a draw task for every lv_draw_* call, even one
// wholly outside the strip / area being redrawn, and each task costs ~0.1 ms
// on the 1.75" -- custom draws skip what this layer can't see.
// (layerSees lives in ui_prims.h since 3.1-dev.25.)

// ---- Round-progress rim: one arc segment per round around the bezel,
// past rounds dim accent, the current round bright, future rounds dark. ----
struct UiRim { int total; int current; uint32_t color; };
static UiRim ui_rim = { 8, 1, 0x5FD4FF };

// v0.23.4 diagnostics: timed wrapper (see [UI] 5s draw ms).
static void uiRimDrawCbImpl(lv_event_t *e);
static void uiRimDrawCb(lv_event_t *e) { const uint32_t t0 = micros(); uiRimDrawCbImpl(e); g_uiDrawUs[UI_T_RIM] += micros() - t0; }
// 2.0 frame rate: LVGL's arcs rebuild their circle masks in every strip
// (~7 ms a full redraw on the 1.75"). Each segment's SHAPE depends only on the
// game length, so it is drawn once into an A8 image in PSRAM (same geometry
// as the arcs: radius 230, 3 px, rounded ends) and recolored at draw time.
static const int RIM_MAX = 12;
static struct { int total; int32_t x0[RIM_MAX], y0[RIM_MAX]; lv_image_dsc_t img[RIM_MAX]; } rim_cache = { 0 };

static bool rimBuild(int n) {
  for (int i = 0; i < rim_cache.total; i++) { heap_caps_free((void *)rim_cache.img[i].data); rim_cache.img[i].data = nullptr; }
  rim_cache.total = 0;
  if (n > RIM_MAX) return false;
  const float seg = 360.0f / n, gap = 2.4f, rOut = 230.0f, rIn = 227.0f, rMid = 228.5f, cap = 1.5f;
  for (int i = 0; i < n; i++) {
    // whole degrees, exactly like the arcs (int cast of the float angles)
    const int a0 = (int)(270.0f + i * seg + gap / 2), a1 = (int)(270.0f + (i + 1) * seg - gap / 2);
    const float span = (float)(a1 - a0);
    int x0 = 466, y0 = 466, x1 = 0, y1 = 0;
    for (int a = a0; a <= a1; a++) {
      const float rad = a * (float)M_PI / 180.0f;
      const int px = 233 + (int)lroundf(cosf(rad) * rOut), py = 233 + (int)lroundf(sinf(rad) * rOut);
      x0 = min(x0, px); x1 = max(x1, px); y0 = min(y0, py); y1 = max(y1, py);
      const int qx = 233 + (int)lroundf(cosf(rad) * rIn), qy = 233 + (int)lroundf(sinf(rad) * rIn);
      x0 = min(x0, qx); x1 = max(x1, qx); y0 = min(y0, qy); y1 = max(y1, qy);
    }
    x0 = max(0, x0 - 3); y0 = max(0, y0 - 3); x1 = min(465, x1 + 3); y1 = min(465, y1 + 3);
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    uint8_t *buf = (uint8_t *)heap_caps_calloc((size_t)w * h, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { rim_cache.total = i; return false; }
    const float e0 = a0 * (float)M_PI / 180.0f, e1 = a1 * (float)M_PI / 180.0f;
    const float c0x = 233 + cosf(e0) * rMid, c0y = 233 + sinf(e0) * rMid, c1x = 233 + cosf(e1) * rMid, c1y = 233 + sinf(e1) * rMid;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const float dx = x0 + x + 0.5f - 233, dy = y0 + y + 0.5f - 233;
        const float r = sqrtf(dx * dx + dy * dy);
        if (r < rIn - 1 || r > rOut + 1) continue;
        float ang = atan2f(dy, dx) * 180.0f / (float)M_PI - a0;
        while (ang < 0) ang += 360.0f;
        while (ang >= 360.0f) ang -= 360.0f;
        float cov;
        if (ang <= span) cov = min(1.0f, 0.5f + min(r - rIn, rOut - r));
        else {  // past an end: the rounded cap
          const float px = x0 + x + 0.5f, py = y0 + y + 0.5f;
          const float d = min(hypotf(px - c0x, py - c0y), hypotf(px - c1x, py - c1y));
          cov = 0.5f + cap - d;
        }
        if (cov > 0) buf[y * w + x] = cov >= 1 ? 255 : (uint8_t)(cov * 255);
      }
    }
    lv_image_dsc_t &img = rim_cache.img[i];
    memset(&img, 0, sizeof(img));
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_A8;
    img.header.w = w;
    img.header.h = h;
    img.header.stride = w;
    img.data_size = (uint32_t)w * h;
    img.data = buf;
    rim_cache.x0[i] = x0;
    rim_cache.y0[i] = y0;
  }
  rim_cache.total = n;
  return true;
}

static void uiRimDrawCbImpl(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  int n = ui_rim.total > 0 ? ui_rim.total : 8;
  static bool rimFailed = false;
  if (rim_cache.total != n && !rimFailed) rimFailed = !rimBuild(n);
  if (rim_cache.total == n) {
    for (int i = 0; i < n; i++) {
      const lv_image_dsc_t &img = rim_cache.img[i];
      const lv_area_t at = { rim_cache.x0[i], rim_cache.y0[i], rim_cache.x0[i] + (int32_t)img.header.w - 1, rim_cache.y0[i] + (int32_t)img.header.h - 1 };
      if (!layerSees(layer, at.x1, at.y1, at.x2, at.y2)) continue;
      lv_draw_image_dsc_t d;
      lv_draw_image_dsc_init(&d);
      d.src = &img;
      const int round = i + 1;
      d.recolor = lv_color_hex(round <= ui_rim.current ? ui_rim.color : UI_RING);
      d.opa = round < ui_rim.current ? LV_OPA_40 : LV_OPA_COVER;
      lv_draw_image(layer, &d, &at);
    }
    return;
  }
  float seg = 360.0f / n;
  const float gap = 2.4f;
  for (int i = 0; i < n; i++) {
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = 233;
    d.center.y = 233;
    d.radius = 230;
    d.width = 3;  // 2.0: thinner
    int a0 = (int)(270.0f + i * seg + gap / 2);
    int a1 = (int)(270.0f + (i + 1) * seg - gap / 2);
    d.start_angle = a0 % 360;
    d.end_angle = a1 % 360;
    d.rounded = true;
    int round = i + 1;
    if (round < ui_rim.current) { d.color = lv_color_hex(ui_rim.color); d.opa = LV_OPA_40; }
    else if (round == ui_rim.current) { d.color = lv_color_hex(ui_rim.color); d.opa = LV_OPA_COVER; }
    else { d.color = lv_color_hex(UI_RING); d.opa = LV_OPA_COVER; }
    lv_draw_arc(layer, &d);
  }
}

static void uiAddRim(int total, int current, uint32_t color) {
  ui_rim = { total, current, color };
  lv_obj_t *rim = lv_obj_create(screen_root);
  lv_obj_remove_style_all(rim);
  lv_obj_set_size(rim, 466, 466);
  lv_obj_set_pos(rim, 0, 0);
  lv_obj_clear_flag(rim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(rim, uiRimDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
}

// ---- Faction insignia (original shapes, drawn live): Rebel sunburst,
// Imperial hexagon, Pyke diamond, Crimson Dawn crescent. ----

// (uiLine lives in ui_prims.cpp since 3.1-dev.25.)

// 2.0 sprites (original pixel art; tools/art/sprites_2_0.py regenerates the
// grids and a preview PNG -- the phone page has the same grids, keep them in
// step). One character per pixel: '.' = nothing, else a palette letter.
static const char *const ASHEN_SPRITE[17] = {  // THE ASHEN FLEET's flagship, seen from above, nose left
  ".................................lll....",
  ".............................llllhhh....",
  ".........................llllhdhhhhhree.",
  "....................lllllhhhhhdhhrhhreye",
  "................lellhhhhdhhhhhdhhhhhree.",
  "...........lllllhherhhhhdlllllllhhhh....",
  ".......lllrhdhhhhhdhhhhhdlhhhhhdhhhh....",
  "...llllhhhhrhhhlllllllllllhyyyhdllhhree.",
  "dddhhhllllllllllllllllllllhhhhhdllhhreye",
  "...ddddhhhhhhhhddddddddddlhhhhhdddhhree.",
  ".......ddddhdhrhhhdhhhhhdlhhhhhdhhhh....",
  "...........dddddhhdhhrhhdlddddddhhhh....",
  "................ddddhhrhdhhhhhdhhhhh....",
  "....................dddddhhhhhdhhhehree.",
  ".........................ddddhdhhhhhreye",
  ".............................ddddhhhree.",
  ".................................ddd....",
};
static const char *const CLOSED_SPRITE[22] = {  // the markets' sign while the Ashen Fleet blockades the lanes
  "........nn.........................nn.........",
  "........c..........................c..........",
  ".........c..........................c.........",
  "........c..........................c..........",
  ".........c..........................c.........",
  "........c..........................c..........",
  ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
  ".kwnwwwwvwwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvnwk.",
  ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
  ".kwwvwwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvwwwwwwk.",
  ".kvwwwRRRwwRwvwwwwRRRwwwRRRRwRRRRRwRRRRwwwwwk.",
  ".kwwwRwwwRwRwwwwwRwwwRvRwwwwwRwwwvwRwwwRwwwwk.",
  ".kwwwRwwwvwRwwwwwRwwvRwRwwwwwRwvwwwRwwwRwwvwk.",
  ".kwnwRwvwwwRwwwwwRvwwRwwRRRwwRRRRwwRwwwRvwnwk.",
  ".kwwwRwwwwwRwwwwvRwwwRwwwwwRwRwwwwwRwwvRwwwwk.",
  ".kwvwRwwwRwRwwvwwRwwwRwwwvwRwRwwwwwRvwwRwwwwk.",
  ".kwwwwRRRwwRRRRRwwRRRwwRRRRwwRRRRRvRRRRwwwwwk.",
  ".kwwwwwwwwvwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvk.",
  ".kwwwwwwvwwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvwwk.",
  ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
  ".kwnvwwwwwwwwwwvwwwwwwwwwwvwwwwwwwwwwvwwwwnwk.",
  ".kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk.",
};
static uint32_t spriteColor(char c) {
  switch (c) {
    case 'h': return 0x4A5060; case 'l': return 0x7A8296; case 'd': return 0x2A2E38;   // hull
    case 'y': return 0xFFD27A; case 'e': return 0xFF8C42; case 'r': return 0xE0404A;   // fire
    case 'c': return 0x8C8C96; case 'n': return 0xC8C8D2;                                // chain, nails
    case 'w': return 0x8B5E34; case 'v': return 0x78502C; case 'k': return 0x462C18;   // wood
    case 'R': return 0xD62828;                                                           // paint
    default: return 0;
  }
}
// Draws a sprite with its top-left at (x0, y0), `cell` px per pixel; runs of
// one color in a row are one rect.
static void drawSprite(lv_layer_t *layer, const char *const *rows, int nRows, int x0, int y0, int cell) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_opa = LV_OPA_COVER;
  for (int j = 0; j < nRows; j++) {
    if (!layerSees(layer, x0, y0 + j * cell, x0 + 64 * cell, y0 + j * cell + cell - 1)) continue;
    const char *r = rows[j];
    int i = 0;
    while (r[i]) {
      const char c = r[i];
      const int start = i;
      while (r[i] == c) i++;
      if (c == '.') continue;
      d.bg_color = lv_color_hex(spriteColor(c));
      lv_area_t a = { (lv_coord_t)(x0 + start * cell), (lv_coord_t)(y0 + j * cell), (lv_coord_t)(x0 + i * cell - 1), (lv_coord_t)(y0 + j * cell + cell - 1) };
      if (!layerSees(layer, a.x1, a.y1, a.x2, a.y2)) continue;
      lv_draw_rect(layer, &d, &a);
    }
  }
}
// An object that draws the Ashen flagship centered in itself (user_data = cell size).
static void ashenSpriteDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  const int cell = (int)(intptr_t)lv_event_get_user_data(e);
  drawSprite(lv_event_get_layer(e), ASHEN_SPRITE, 17, (a.x1 + a.x2 + 1) / 2 - 20 * cell, (a.y1 + a.y2 + 1) / 2 - 17 * cell / 2, cell);
}
static lv_obj_t *ashenSprite(lv_obj_t *parent, int cx, int cy, int cell) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(o, 40 * cell, 17 * cell);
  lv_obj_set_pos(o, cx - 20 * cell, cy - 17 * cell / 2);
  lv_obj_add_event_cb(o, ashenSpriteDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)cell);
  return o;
}

// (uiDisc, ui_insignia_gray, uiDrawInsignia live in ui_prims.cpp since 3.1-dev.25.)

static void uiInsigniaDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);  // faction<<24 | bg color
  int size = lv_area_get_width(&a);
  uiDrawInsignia(lv_event_get_layer(e), (int)(packed >> 24), a.x1 + size / 2, a.y1 + size / 2, size / 2,
                 packed & 0xFFFFFF);
}

// An insignia as a positioned child object (e.g. inside a row).
static lv_obj_t *uiInsignia(lv_obj_t *parent, int faction, int size, uint32_t bg) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, size, size);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(o, uiInsigniaDrawCb, LV_EVENT_DRAW_MAIN,
                      (void *)(uintptr_t)(((uint32_t)faction << 24) | (bg & 0xFFFFFF)));
  return o;
}

// ---------------------------------------------------------------------------
// COMMANDER PORTRAITS (v0.21.20). The 24x28 pixel art (commander_art.cpp,
// generated from tools/art/portraits_v2.html) is baked once per (commander,
// variant) into a PSRAM ARGB8888 image at an integer scale -- no
// transform_scale (Convention 7). Variants: CMD_IMG_SMALL (2x, Lobby
// circles), CMD_IMG_BIG (5x, the picker), CMD_IMG_HOLO (5x hologram for
// transmissions: brightness, contrast-stretched per portrait, picks one of
// five blues; scanlines every 3rd screen row -- same art, no extra flash).
// Like ringPlanetImage, each descriptor is written once and never reused.
// ---------------------------------------------------------------------------
// v0.30.0: CMD_IMG_HOLO3 (3x hologram, the negotiation conversation) and
// CMD_IMG_AMB (3x, the UNKNOWN AMBASSADOR: same shading in grays, rows torn
// sideways -- ambShift() is the phone's twin).
enum { CMD_IMG_SMALL, CMD_IMG_BIG, CMD_IMG_HOLO, CMD_IMG_HOLO3, CMD_IMG_AMB, CMD_IMG_VARIANTS };
static const int CMD_IMG_SCALE[CMD_IMG_VARIANTS] = { 2, 5, 5, 3, 3 };
static lv_image_dsc_t cmd_img_dsc[CMD_IMG_VARIANTS][NUM_COMMANDERS];
static uint8_t *cmd_img_data[CMD_IMG_VARIANTS][NUM_COMMANDERS];
static const uint32_t CMD_HOLO[5] = { 0x0B2C3C, 0x17607A, 0x2E9CBC, 0x6FDCFA, 0xD4F7FF };
static const uint32_t CMD_AMB[5] = { 0x1A1D22, 0x3A3F47, 0x5D636C, 0x8B9199, 0xC3C8CE };
static int ambShift(int y, int id) { const int h = (y * 7 + id * 3) % 9; return h == 0 ? 2 : h == 4 ? -2 : h == 6 ? 1 : 0; }

static int cmdLum(uint32_t c) { return (30 * (int)(c >> 16) + 59 * (int)((c >> 8) & 0xFF) + 11 * (int)(c & 0xFF)) / 100; }

static const lv_image_dsc_t *commanderImage(int id, int variant) {
  if (id < 0 || id >= NUM_COMMANDERS || variant < 0 || variant >= CMD_IMG_VARIANTS) return nullptr;
  if (!cmd_img_data[variant][id]) {
    const CommanderArt &a = COMMANDER_ART[id];
    const int s = CMD_IMG_SCALE[variant], w = COMMANDER_ART_W * s, h = COMMANDER_ART_H * s;
    const size_t bytes = (size_t)w * h * 4;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)malloc(bytes);
    if (!buf) return nullptr;
    int lo = 255, hi = 0;
    const bool holo = variant == CMD_IMG_HOLO || variant == CMD_IMG_HOLO3 || variant == CMD_IMG_AMB;
    if (holo)
      for (int k = 1; k < a.nColors; k++) { const int l = cmdLum(a.pal[k]); lo = min(lo, l); hi = max(hi, l); }
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const int ax = x / s - (variant == CMD_IMG_AMB ? ambShift(y / s, id) : 0);
        const uint8_t i = (ax >= 0 && ax < COMMANDER_ART_W) ? a.px[(y / s) * COMMANDER_ART_W + ax] : 0;
        uint32_t rgb = i ? a.pal[i] : 0;
        uint8_t alpha = i ? 0xFF : 0;
        if (i && holo) {
          rgb = (variant == CMD_IMG_AMB ? CMD_AMB : CMD_HOLO)[((cmdLum(rgb) - lo) * 4 + max(1, hi - lo) / 2) / max(1, hi - lo)];
          if (y % 3 == 0) rgb = ((rgb >> 1) & 0x7F7F7F) + ((rgb >> 3) & 0x1F1F1F);  // scanline: ~62%
        }
        uint8_t *p = buf + ((size_t)y * w + x) * 4;  // ARGB8888 is B,G,R,A in memory
        p[0] = rgb & 0xFF; p[1] = (rgb >> 8) & 0xFF; p[2] = (rgb >> 16) & 0xFF; p[3] = alpha;
      }
    }
    lv_image_dsc_t &d = cmd_img_dsc[variant][id];
    memset(&d, 0, sizeof(d));
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_ARGB8888;
    d.header.w = w;
    d.header.h = h;
    d.header.stride = w * 4;
    d.data_size = bytes;
    d.data = buf;
    cmd_img_data[variant][id] = buf;
  }
  return &cmd_img_dsc[variant][id];
}

// A commander portrait as a child image (nullptr-safe).
static lv_obj_t *uiPortrait(lv_obj_t *parent, int id, int variant) {
  lv_obj_t *img = lv_image_create(parent);
  const lv_image_dsc_t *d = commanderImage(id, variant);
  if (d) lv_image_set_src(img, d);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
  return img;
}

static void randomizeStars() {
  for (size_t i = 0; i < STAR_COUNT; ++i) {
    stars[i].x = randomInt(18, 447);
    stars[i].y = randomInt(18, 447);
    stars[i].r = (uint8_t)randomInt(1, 2);
    stars[i].brightness = (uint8_t)randomInt(80, 230);
    stars[i].dx = (int8_t)randomInt(-1, 1);
  }
}

// v0.23.4 diagnostics: timed wrapper (see [UI] 5s draw ms).
static void drawStarsImpl(lv_layer_t *layer);
static void drawStars(lv_layer_t *layer) { const uint32_t t0 = micros(); drawStarsImpl(layer); g_uiDrawUs[UI_T_STARS] += micros() - t0; }
static void drawStarsImpl(lv_layer_t *layer) {
  for (size_t i = 0; i < STAR_COUNT; ++i) {
    lv_color_t color = lv_color_make(stars[i].brightness, stars[i].brightness,
                                     (uint8_t)min(255, stars[i].brightness + 25));
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = color;
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = LV_RADIUS_CIRCLE;
    lv_area_t a;
    a.x1 = stars[i].x;
    a.y1 = stars[i].y;
    a.x2 = stars[i].x + stars[i].r;
    a.y2 = stars[i].y + stars[i].r;
    if (!layerSees(layer, a.x1, a.y1, a.x2, a.y2)) continue;
    lv_draw_rect(layer, &dsc, &a);
  }
}

static void starEventCb(lv_event_t *e) {
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  (void)obj;
  // Layer drawing callback is attached to the root; keep event hook unused.
}

static void updateHomeUI(const GameState &gs);
static void showFactionTransmission(int factionIdx, const char *quote, int fromTier = -1, int toTier = -1, bool big = false, int cause = -1, int delta = 0, int myCmd = -1);
static bool checkAndShowTransmission(const GameState &gs);
static bool showPendingTransmission();  // v0.30.2: reads its own copy (CONTINUE handlers)
static bool isMySeat(const Player &p);  // v0.30.3: defined after MY_CLIENT_ID
static void showGameOver();
static void showStart();
static void maybeShowRoundBanner(const GameState &gs);
static void showVictory();  // battle report (v0.21.13)
static void updatePlanetRing(const GameState &gs);  // Galaxy ring owner colors (v0.21.2)
static float shakeNoise(int32_t seed);  // defined with the transmission screen


#if SF_ROLE == SF_ROLE_TABLE
// ===========================================================================
// TABLE HOST (v0.22.0; 1.43" in the middle of the board). This build owns the
// game -- Wi-Fi network, rules, phone server -- but never takes a seat:
// MY_SLOT stays -1 and no player screens are ever shown. Everyone plays on
// phones (4 phones + this = a full game). The screen is the table's shared
// view, rebuilt only when something it shows changes (tableKey):
//   Lobby     -- who has joined (faces), game length, how to join
//   Playing   -- the galaxy clock: round ring, ROUND n / N, phase, who's
//                locked in, each player's face + VP at the diagonals
//   Game over -- the winner
// Nothing here is tappable. The polished clock + big-moment alerts come
// after a mockup (the design notes, multi-board step 3).
// ===========================================================================
static uint32_t table_shown_key = 0xFFFFFFFFu;
// v0.24.4: a public big moment takes over the table for TABLE_ALERT_MS.
static const uint32_t TABLE_ALERT_MS = 8000;
// 2.0: a strike on the Ashen Fleet shows its damage on the table this long (tableShowAshen).
static const uint32_t TABLE_HIT_MS = 8000;
static uint16_t table_ashen_seq = 0;
static uint32_t table_ashen_hit_until = 0;
static bool tableAshenHitOn() { return table_ashen_hit_until && (int32_t)(table_ashen_hit_until - millis()) > 0; }
static uint16_t table_alert_seq = 0;
static uint32_t table_alert_until = 0;
static bool tableAlertOn() { return table_alert_until && (int32_t)(table_alert_until - millis()) > 0; }
// v0.25.2: "who goes first" plays on the table for TABLE_FIRST_MS when a game starts.
static const uint32_t TABLE_FIRST_MS = 6500;
static uint32_t table_first_until = 0;
static bool tableFirstOn() { return table_first_until && (int32_t)(table_first_until - millis()) > 0; }
// v0.27.0: the battle dice tumble for a moment after each new exchange (set in starTick).
static uint32_t table_tumble_until = 0;
static bool tableTumbleOn() { return table_tumble_until && (int32_t)(table_tumble_until - millis()) > 0; }

static uint32_t tableKey(const GameState &gs) {
  uint32_t k = (uint32_t)gs.stage;
  k = k * 31 + (tableAlertOn() ? 7u : 0u) + gs.publicSeq;  // v0.24.4: the alert comes and goes
  k = k * 31 + (tableFirstOn() ? 3u : 0u);  // v0.25.2
  k = k * 31 + (uint32_t)gs.round;
  k = k * 31 + (uint32_t)gs.phase;
  k = k * 31 + (uint32_t)gs.roundLimit;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player &p = gs.players[i];
    k = k * 31 + (p.active ? 1u : 0u);
    k = k * 31 + (uint32_t)(p.chosenFaction + 2);
    k = k * 31 + (uint32_t)(p.commander + 2);
    k = k * 31 + (p.lockedIn ? 1u : 0u);
    k = k * 31 + (uint32_t)(p.vp + 100);
  }
  k = k * 31 + (gs.battleActive ? 1u : 0u);  // v0.24.0: battles show on the table
  k = k * 31 + (uint32_t)(gs.battleKind + 1);
  k = k * 31 + (uint32_t)(gs.battleAttackerSlot + 2);
  k = k * 31 + (uint32_t)(gs.battleDefenderSlot + 2);
  k = k * 31 + (uint32_t)(gs.battleTargetPlanet + 2);
  k = k * 31 + (uint32_t)gs.battleExchange;  // v0.27.0: the live score (dice, ships)
  k = k * 31 + (uint32_t)(gs.defRemaining + 1);
  k = k * 31 + (tableTumbleOn() ? millis() / 150 : 0);  // the dice tumble for a moment
  k = k * 31 + (uint32_t)(gs.firstPlayer + 2);
  k = k * 31 + (uint32_t)(snapshotEra(gs) + 2) * 13u + (uint32_t)(gs.eraUntil + 2);  // 3.1-dev.23: the Era is the clock's weather
  k = k * 31 + (uint32_t)gs.ashenHp + (uint32_t)gs.ashenSeq * 7u + (tableAshenHitOn() ? 5u : 0u);  // 2.0
  return k;
}

// A player's face (or insignia / "?") in a circle; ready = brighter, thicker ring.
static lv_obj_t *tableBadge(const Player &p, int cx, int cy, int d, bool ready) {
  const int f = p.chosenFaction;
  const bool hasF = f >= 0 && f < 4;
  const uint32_t ring = hasF ? FACTION_RING_COLOR[f] : UI_LINE;
  lv_obj_t *c = lv_obj_create(screen_root);
  uiStyleButton(c, ring, ready ? 4 : 2, hasF ? uiTint(ring) : UI_PANEL, LV_RADIUS_CIRCLE);  // style first (Convention 13)
  lv_obj_set_style_clip_corner(c, true, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(c, d, d);
  lv_obj_set_pos(c, cx - d / 2, cy - d / 2);
  if (!ready && hasF) lv_obj_set_style_border_opa(c, LV_OPA_50, 0);
  if (hasF && p.commander >= 0) {
    lv_obj_t *im = uiPortrait(c, p.commander, CMD_IMG_SMALL);
    lv_obj_align(im, LV_ALIGN_BOTTOM_MID, 0, 2);
  } else if (hasF) {
    lv_obj_t *ins = uiInsignia(c, f, d / 2, uiTint(ring));
    lv_obj_center(ins);
  } else {
    lv_obj_t *q = lv_label_create(c);
    lv_label_set_text(q, "?");
    lv_obj_set_style_text_color(q, lv_color_hex(UI_FAINT), 0);
    lv_obj_center(q);
  }
  return c;
}

// Join QR codes (v0.22.4): baked once into PSRAM images (dark modules on
// white, 2-module quiet zone, 4 px per module). Phones scan the first to join
// the Wi-Fi (standard "WIFI:" format -- the camera offers to join) and the
// second to open the game in their real browser. No captive "sign-in" pop-up
// on purpose: faking internet would cut the phone's cellular data, and that
// pop-up browser closes when you switch apps.
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
  for (int y = 0; y < w; y++) {
    for (int x = 0; x < w; x++) {
      const int mx = x / s - quiet, my = y / s - quiet;
      const bool dark = mx >= 0 && my >= 0 && mx < n && my < n && sfqr_getModule(qr, mx, my);
      uint8_t *p = buf + ((size_t)y * w + x) * 4;  // ARGB8888 = B,G,R,A
      const uint8_t v = dark ? 0x05 : 0xF4;
      p[0] = v; p[1] = v; p[2] = v; p[3] = 0xFF;
    }
  }
  lv_image_dsc_t &d = qr_dsc[slot];
  memset(&d, 0, sizeof(d));
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_ARGB8888;
  d.header.w = w;
  d.header.h = w;
  d.header.stride = w * 4;
  d.data_size = bytes;
  d.data = buf;
  qr_px[slot] = buf;
  return &d;
}

// One QR with a caption under it, centered on cx; top at y.
// v0.22.5: switch between home Wi-Fi and the table's own network (remembered;
// the table restarts into the other mode).
static void tableWifiSwitchCb(lv_event_t *e) {
  (void)e;
  networkSwitchMode(!networkOnHomeWifi());
}

static void tableQr(int slot, const char *text, int cx, int y, const char *caption) {
  const lv_image_dsc_t *d = qrImage(slot, text);
  lv_obj_t *img = lv_image_create(screen_root);
  if (d) lv_image_set_src(img, d);
  lv_obj_set_style_radius(img, 10, 0);
  lv_obj_set_style_clip_corner(img, true, 0);
  const int w = d ? (int)d->header.w : 120;
  lv_obj_set_pos(img, cx - w / 2, y);
  lv_obj_t *c = uiLabel(screen_root, caption, UI_CYAN, &sf_font_montserrat_12, 0, 2);
  lv_obj_align(c, LV_ALIGN_TOP_MID, cx - 233, y + w + 6);
}

// ---------------------------------------------------------------------------
// v0.24.0 -- the ANY-ANGLE table (user 2026-10-15: "I would love for most
// things to be understandable from any angle"; mockups "Table Console v2").
// Light, color and shape instead of words: the round ring, a 3-part phase
// ring with symbols (coins = Invest, chevrons = Move, crossed blades =
// Resolve), the round number, and each player's badge sitting toward their
// faction's hangar on the map with their VP chip turned to face them.
// ---------------------------------------------------------------------------
static void drawSegDigit(lv_layer_t *layer, int d, int x, int y, int w, int h, lv_color_t c, int t);  // round banner
static void spinPlanetDrawCb(lv_event_t *e);                                                        // battle scene

// The map direction (deg, 0 = east, counter-clockwise) the screen's TOP faces.
// 3.1-dev.25: everything the table draws (seats, the clock, battles, moments, the Lobby) is in
// table_scenes.cpp -- tools/table_harness renders it on a PC. Defined with the planet renderer:
static void tablePlanetPre();

// ---------------------------------------------------------------------------
// 2.0 THE ASHEN FLEET takes the table (user: it "hogs the center screen and
// taunts until it goes away"; "it needs colors"): a full-color LVGL screen
// instead of the one-color glow -- the HP ring round the rim, the flagship,
// HP, its admiral's taunt (a new one every TABLE_TAUNT_MS), the round line at
// the bottom. A strike shows its damage big for TABLE_HIT_MS. Battles, big
// moments and who-goes-first still use the glow screen.
// ---------------------------------------------------------------------------
static const uint32_t TABLE_TAUNT_MS = 9000;
static lv_obj_t *table_ashen_taunt = nullptr;
static lv_timer_t *table_ashen_timer = nullptr;
static int table_ashen_taunt_i = 0;
static void tableAshenTauntGone(lv_event_t *e) { (void)e; table_ashen_taunt = nullptr; }
static void tableAshenTauntTick(lv_timer_t *t) {
  if (!table_ashen_taunt) { lv_timer_delete(t); table_ashen_timer = nullptr; return; }
  table_ashen_taunt_i++;
  lv_label_set_text(table_ashen_taunt, ashenTaunt(table_ashen_taunt_i));
}
static void tableShowAshen(const GameState &gs) {
  trStop();
  tableComposeReset();
  clearScreen();
  lv_obj_set_style_bg_color(screen_root, lv_color_hex(0x140604), 0);
  lv_obj_invalidate(screen_root);
  lv_obj_t *arc = lv_arc_create(screen_root);  // the HP ring
  lv_obj_set_size(arc, 450, 450);
  lv_obj_center(arc);
  lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_rotation(arc, 270);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_range(arc, 0, max(1, (int)gs.ashenMaxHp));
  lv_arc_set_value(arc, gs.ashenHp);
  lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x3A0E0A), LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(gs.ashenHp * 3 <= gs.ashenMaxHp ? 0xE0404A : 0xFF8C42), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
  uiLabel(screen_root, "THE ASHEN FLEET", 0xFF8C42, &lv_font_montserrat_14, 62, 4);
  ashenSprite(screen_root, 233, 160, 7);
  char buf[96];
  if (tableAshenHitOn() && gs.ashenLastSlot >= 0) {
    snprintf(buf, sizeof(buf), "-%d", gs.ashenLastDmg);
    uiLabel(screen_root, buf, 0xFF5A4A, &sf_font_montserrat_36, 232, 2);
    const int f = gs.players[gs.ashenLastSlot].chosenFaction;
    snprintf(buf, sizeof(buf), "%s STRUCK WITH %d SHIPS - %s", (f >= 0 && f < 4) ? FACTION_SHORT[f] : "A FLEET", gs.ashenLastShips,
             gs.ashenLastHits == 0 ? "IT MISSED" : gs.ashenLastHits == 1 ? "1 SHIP LOST" : "2 SHIPS LOST");
    uiLabel(screen_root, buf, 0xFFD27A, &sf_font_montserrat_12, 280, 1);
  } else {
    snprintf(buf, sizeof(buf), "%d / %d", gs.ashenHp, gs.ashenMaxHp);
    uiLabel(screen_root, buf, UI_TEXT, &sf_font_montserrat_36, 232, 2);
    uiLabel(screen_root, "HULL", 0xFF8C42, &sf_font_montserrat_12, 280, 3);
  }
  table_ashen_taunt = uiLabel(screen_root, ashenTaunt(table_ashen_taunt_i), 0xFFB38A, &lv_font_montserrat_14, 310, 0);
  lv_label_set_long_mode(table_ashen_taunt, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(table_ashen_taunt, 300);
  lv_obj_add_event_cb(table_ashen_taunt, tableAshenTauntGone, LV_EVENT_DELETE, nullptr);
  if (!table_ashen_timer) table_ashen_timer = lv_timer_create(tableAshenTauntTick, TABLE_TAUNT_MS, nullptr);
  int active = 0, locked = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (gs.players[i].active) { active++; if (gs.players[i].lockedIn) locked++; }
  snprintf(buf, sizeof(buf), "ROUND %d / %d  -  %s  -  %d OF %d LOCKED IN", gs.round, gs.roundLimit, PHASE_NAMES[gs.phase], locked, active);
  uiLabel(screen_root, buf, UI_DIM, &sf_font_montserrat_12, 392, 1);
}

static void tableShowGame(const GameState &gs) {
  if (snapshotAshenAlive(gs) && gs.stage == STAGE_PLAYING && !tableFirstOn() && !tableAlertOn() && !gs.battleActive) {
    tableShowAshen(gs);  // 2.0: the Ashen Fleet holds the table until it's gone
    return;
  }
  const uint32_t ovT0 = millis();
  const TableView v = { tableTumbleOn(), tableAlertOn(), tableFirstOn() };
  const int mode = tableCompose(gs, v);
  if (mode < 0) return;
  tr_scene.preFrame = tablePlanetPre;
  const uint32_t ovMs = millis() - ovT0;
  extern uint32_t table_ms_words;
  if (ovMs > 40) Serial.printf("[TABLE] slow overlay: mode %d took %u ms (words %u)\n", mode, (unsigned)ovMs, (unsigned)table_ms_words);  // 3.1-dev.23
  trStart();
}

static void showTable(const GameState &gs) {
  currentScreen = SCR_TABLE;
  onHomeScreen = false;
  table_shown_key = tableKey(gs);
  if (gs.stage != STAGE_LOBBY) {  // v0.26.0: the glow renderer
    clearScreen();
    tableShowGame(gs);
    return;
  }
  trStop();  // back to LVGL for the Lobby (its new objects redraw the whole screen)
  tableComposeReset();
  clearScreen();
  lv_obj_invalidate(screen_root);

  if (gs.stage == STAGE_LOBBY) {  // 3.1-dev.25: table_scenes.cpp (board "Lobby")
    const TableLobbyEnv env = { networkOnHomeWifi(), networkHomeSaved(), networkHomeFailed(), networkGameUrl(), networkHomeSsid(), networkFailWhy(), nullptr };
    tableLobbyBuild(screen_root, gs, env);
    // 3.1-dev.31 (playtest: "pressing the table wifi button isn't doing anything"): the button is the sketch's own
    // uiPill again (the one that always worked), drawn over the Lobby -- table_scenes draws none when the callback is null.
    if (env.homeWifi || env.homeSaved)
      uiPill(screen_root, 168, 350, 130, 40, env.homeWifi ? "TABLE WI-FI" : "HOME WI-FI", nullptr, env.homeWifi ? UI_DIM : UI_CYAN, tableWifiSwitchCb);
    return;
  }
}
#endif  // SF_ROLE_TABLE

// v0.23.5: redraw only what moved. Invalidating the whole screen every tick
// re-rendered every arc, circle and label in 12 slices (~400 ms a pass on
// Home) just so the stars could twinkle. Now each tick marks the few pixels
// around half the stars (each moving twice as far, so the drift speed is
// unchanged -- and LVGL's 32-area invalidation list never overflows into a
// full redraw), plus any animated objects the screen registered.
static void invalidateSpan(int x1, int x2, int y, int r) {  // x1..x2 = left edges covered
  lv_area_t a = { (int32_t)(x1 - 1), (int32_t)(y - 1), (int32_t)(x2 + r + 1), (int32_t)(y + r + 1) };
  lv_obj_invalidate_area(screen_root, &a);
}

// v0.25.3 sounds for the GALAXY-WIDE moments, so the table never gets
// forgotten (user 2026-10-15: "I mostly want sounds for global events so the
// middle screen doesn't get forgotten"; for now on the 1.75's speaker -- the
// 1.43" has none). Watches the mirrored state: a new public big moment ->
// fanfare (or alarm for the dark ones), a battle starting -> horn, a new
// phase -> chime. Primed on the first look, so nothing plays at boot.
static void sfxWatch() {
  static bool primed = false;
  static uint16_t seq = 0;
  static bool battle = false;
  static int phaseKey = -1;
  GameState g = gameStateRead();
  const int pk = g.stage == STAGE_PLAYING ? g.round * 4 + g.phase : -1;
  if (primed) {
    if (g.publicSeq != seq && g.stage == STAGE_PLAYING && g.publicMoment >= 0) {
      const bool dark = g.publicMoment == TXB_BETRAYAL || g.publicMoment == TXB_FLEET || g.publicMoment == TXB_LASTROUND;
      sfxPlay(dark ? SFX_ALARM : SFX_FANFARE);
    }
    if (g.battleActive && !battle) sfxPlay(SFX_HORN);
    if (pk != phaseKey && pk >= 0) sfxPlay(SFX_CHIME);
  }
  static uint32_t lastReport = 0;  // v0.25.6: repeat the audio status (boot lines can be lost before the PC listens)
  if (millis() - lastReport > 10000) { lastReport = millis(); Serial.printf("[AUDIO] %s\n", sfAudioStatus()); }
  primed = true;
  seq = g.publicSeq;
  battle = g.battleActive;
  phaseKey = pk;
}

// v0.28.3: a short toast over whatever screen is up (lv_layer_top), gone after 4 s.
static lv_obj_t *ui_toast = nullptr;
static void uiToastDone(lv_timer_t *t) {
  if (ui_toast) { lv_obj_delete(ui_toast); ui_toast = nullptr; }
  lv_timer_delete(t);
}
static void uiToast(const char *text, uint32_t color) {
  if (ui_toast) { lv_obj_delete(ui_toast); ui_toast = nullptr; }
  ui_toast = lv_obj_create(lv_layer_top());
  uiStyleButton(ui_toast, color, 2, UI_BG, 22);  // style first (Convention 13)
  lv_obj_clear_flag(ui_toast, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(ui_toast, 300, 44);
  lv_obj_set_pos(ui_toast, 83, 64);
  lv_obj_t *l = lv_label_create(ui_toast);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(l, 2, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_center(l);
  lv_timer_create(uiToastDone, 4000, nullptr);
}

// 2.0 "LOOK AT THE TABLE" relay + personal notices (mockup G; user 2026-10-28:
// "I never look at the center screen"). A card on the top layer: tap to close,
// gone by itself after a few seconds. Never over a battle, a transmission or a
// banner -- it waits until you're free.
static lv_obj_t *v2_ovl = nullptr;
static uint32_t v2_ovl_until = 0;
static void v2OvlClose() {
  if (v2_ovl) { lv_obj_delete(v2_ovl); v2_ovl = nullptr; }
}
static void v2OvlTapCb(lv_event_t *e) { (void)e; v2OvlClose(); }
// The card is as tall as its text and centered on the glass; a card up to
// ~230 px tall keeps its corners inside the round edge.
static void v2Card(const char *kicker, uint32_t col, const char *head, const char *sub, uint32_t ms) {
  v2OvlClose();
  v2_ovl = lv_obj_create(lv_layer_top());
  uiStyleButton(v2_ovl, col, 2, UI_BG, 28);  // style first (Convention 13)
  lv_obj_set_size(v2_ovl, 340, 130);
  lv_obj_set_pos(v2_ovl, 63, 168);
  lv_obj_add_flag(v2_ovl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(v2_ovl, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(v2_ovl, v2OvlTapCb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *k = lv_label_create(v2_ovl);
  lv_label_set_text(k, kicker);
  lv_obj_set_style_text_font(k, &sf_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(k, 3, 0);
  lv_obj_set_style_text_color(k, lv_color_hex(col), 0);
  lv_obj_align(k, LV_ALIGN_TOP_MID, 0, 12);
  if (head && head[0]) {
    lv_obj_t *h = lv_label_create(v2_ovl);
    lv_label_set_text(h, head);
    lv_obj_set_style_text_letter_space(h, 3, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(UI_TEXT), 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 36);
  }
  lv_obj_t *t = lv_label_create(v2_ovl);
  lv_label_set_text(t, sub);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(t, 300);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, (head && head[0]) ? 74 : 38);
  lv_obj_update_layout(v2_ovl);
  int h = lv_obj_get_y(t) + lv_obj_get_height(t) + 22;
  if (h < 130) h = 130;
  if (h > 230) h = 230;
  lv_obj_set_size(v2_ovl, 340, h);
  lv_obj_set_pos(v2_ovl, 63, 233 - h / 2);
  v2_ovl_until = millis() + ms;
}
static void v2Overlay(bool table, const char *head, const char *sub) {
  v2Card(table ? "LOOK AT THE TABLE" : "NOTICE", table ? 0xFFC933 : UI_CYAN, head, sub, table ? 6000 : 8000);
  if (table) sfxPlay(SFX_CHIRP);
}
static void showDeals(lv_event_t *e);
static void showDealView();
static void v2RelayWatch() {
  if (v2_ovl && (int32_t)(millis() - v2_ovl_until) > 0) v2OvlClose();
  if (MY_SLOT < 0) return;
  static int lastPub = -1, lastNote = -1;
  static int lastDealSeq = -1;
  char head[24] = "", sub[96] = "";
  bool table = false, note = false, dealsChanged = false;
  {
    GameState g = gameStateRead();  // scoped: gone before the overlay is built
    dealsChanged = lastDealSeq >= 0 && g.dealSeq != lastDealSeq && g.stage == STAGE_PLAYING;  // 2.0: an offer came in / was answered
    lastDealSeq = g.dealSeq;
    if (g.stage != STAGE_PLAYING) { lastPub = g.publicSeq; lastNote = g.players[MY_SLOT].noteSeq; return; }
    if (lastPub < 0) lastPub = g.publicSeq;
    if (lastNote < 0) lastNote = g.players[MY_SLOT].noteSeq;
    if (currentScreen == SCR_BATTLE || currentScreen == SCR_TRANSMISSION || currentScreen == SCR_BANNER || currentScreen == SCR_VICTORY) return;
    if (g.publicSeq != lastPub) {
      lastPub = g.publicSeq;
      snapshotPublicText(g, head, sizeof(head), sub, sizeof(sub));
      table = head[0] != '\0';
    }
    if (g.players[MY_SLOT].noteSeq != lastNote) {
      lastNote = g.players[MY_SLOT].noteSeq;
      if (!table) { head[0] = '\0'; snprintf(sub, sizeof(sub), "%s", g.players[MY_SLOT].note); note = sub[0] != '\0'; }
    }
  }
  if (dealsChanged) {  // the lists and Home's red count follow along (never the form you're filling in)
    if (currentScreen == SCR_DEALS) showDeals(nullptr);
    else if (currentScreen == SCR_DEAL_VIEW) showDealView();
    else if (currentScreen == SCR_HOME) showHome();
  }
  if (table) v2Overlay(true, head, sub);
  else if (note) v2Overlay(false, "", sub);
}

// v0.28.3 (playtest 2026-10-19: "notifications for when you are the last one
// waiting to lock in"): once per phase, when everyone else has locked in.
static void waitingOnYouWatch() {
  static int shownKey = -1;
  if (MY_SLOT < 0) return;
  GameState g = gameStateRead();
  if (g.stage != STAGE_PLAYING || g.battleActive || g.players[MY_SLOT].lockedIn) return;
  int active = 0, others = 0, othersLocked = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!g.players[i].active) continue;
    active++;
    if (i != MY_SLOT) { others++; if (g.players[i].lockedIn) othersLocked++; }
  }
  const int key = g.round * 4 + g.phase;
  if (active > 1 && others == othersLocked && shownKey != key) {
    shownKey = key;
    uiToast("EVERYONE IS WAITING ON YOU", 0xFFD166);
    sfxPlay(SFX_CHIRP);
  }
}

// v0.22.1: unique per console ("CONSOLE-A1B2C3" from its MAC).
static char MY_CLIENT_ID[24] = "CONSOLE";
static void joinGameCb(lv_event_t *e);
// 2.0-dev.6 (user: the table should recover from crashes): a command console
// that rebooted -- crash, watchdog, unplugged -- goes straight back to its
// seat once the table's state shows that seat still under its id, instead of
// waiting on the Start screen for a JOIN GAME tap. A seat that's gone (END
// GAME, NEW GAME) leaves it on Start as before.
static void autoRejoinCheck() {
#if SF_ROLE != SF_ROLE_TABLE
  static uint32_t lastTry = 0;
  if (currentScreen != SCR_START || MY_SLOT >= 0 || !linkIsJoined() || millis() - lastTry < 2000) return;
  lastTry = millis();
  bool mine = false;
  {
    GameState gs = gameStateRead();
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (gs.players[i].active && strcmp(gs.players[i].id, MY_CLIENT_ID) == 0) mine = true;
  }
  if (mine) {
    Serial.println("[JOIN] my seat is still at the table -- rejoining");
    // Its own job from the top of lv_timer_handler -- NOT nested in starTick,
    // whose frame (GameState copies) plus showHome() overflowed the 16 KB
    // LVGL stack (caught by the dev.6 crash record on the first try).
    lv_async_call([](void *) { joinGameCb(nullptr); }, nullptr);
  }
#endif
}

static void starTick(lv_timer_t *timer) {
  (void)timer;
  static uint8_t half = 0;
  half ^= 1;
  autoRejoinCheck();
#if 0  // 2.0: no star field anywhere -- it was the biggest redraw cost on the 1.75" (frame rate)
  for (size_t i = half; i < STAR_COUNT; i += 2) {
    const int oldX = stars[i].x;
    stars[i].x += 2 * stars[i].dx;
    bool wrapped = false;
    if (stars[i].x < 10) { stars[i].x = 455; wrapped = true; }
    if (stars[i].x > 456) { stars[i].x = 10; wrapped = true; }
    // Gentle twinkle.
    int b = (int)stars[i].brightness + randomInt(-10, 10);
    stars[i].brightness = (uint8_t)constrain(b, 70, 240);
    // One area covering where it was and where it is (two if it wrapped):
    // 17 stars stay well under LVGL's 32-area list.
    if (wrapped) { invalidateSpan(oldX, oldX, stars[i].y, stars[i].r); invalidateSpan(stars[i].x, stars[i].x, stars[i].y, stars[i].r); }
    else invalidateSpan(min(oldX, (int)stars[i].x), max(oldX, (int)stars[i].x), stars[i].y, stars[i].r);
  }
#endif
  for (int i = 0; i < anim_obj_count; i++) lv_obj_invalidate(anim_objs[i]);
#if SF_ROLE == SF_ROLE_TABLE
  {  // table host: only the shared table screen, rebuilt when what it shows changes
    GameState gsTable = gameStateRead();
    static int lastExchange = -1;  // v0.27.0: a new battle exchange -> the dice tumble
    if (gsTable.battleActive && gsTable.battleExchange != lastExchange && gsTable.battleExchange > 0) table_tumble_until = millis() + 700;
    lastExchange = gsTable.battleActive ? gsTable.battleExchange : -1;
    if (gsTable.ashenSeq != table_ashen_seq) {  // 2.0: a strike on the Ashen Fleet -- show its damage
      table_ashen_seq = gsTable.ashenSeq;
      if (gsTable.ashenLastSlot >= 0) table_ashen_hit_until = millis() + TABLE_HIT_MS;
    }
    if (table_ashen_hit_until && !tableAshenHitOn()) table_ashen_hit_until = 0;
    if (gsTable.publicSeq != table_alert_seq) {  // v0.24.4: a new public big moment
      table_alert_seq = gsTable.publicSeq;
      if (gsTable.publicMoment >= 0 && gsTable.stage == STAGE_PLAYING) table_alert_until = millis() + TABLE_ALERT_MS;
    }
    if (table_alert_until && !tableAlertOn()) table_alert_until = 0;  // over: back to the clock
    static int lastStage = -1;  // v0.25.2: a game just started -> who goes first
    // 3.1-dev.112 (user: "the who goes first screen is 100% useless ... remove it"): never plays; START goes straight to the clock.
    if (false && gsTable.stage == STAGE_PLAYING && lastStage == STAGE_LOBBY && gsTable.firstPlayer >= 0) {
      table_first_until = millis() + TABLE_FIRST_MS;
    }
    lastStage = gsTable.stage;
    if (table_first_until && !tableFirstOn()) table_first_until = 0;
    if (tableKey(gsTable) != table_shown_key) showTable(gsTable);
    return;
  }
#endif
  sfxWatch();  // v0.25.3: global-event sounds
  waitingOnYouWatch();  // v0.28.3
#if SF_ROLE != SF_ROLE_TABLE
  v2RelayWatch();  // 2.0: "look at the table" + notices
#endif

  if (onHomeScreen) {
    GameState gsHome = gameStateRead();
    updateHomeUI(gsHome);
  }

  // Safety net: the explicit checkAndShowTransmission() calls right after
  // Fight/Pay/Negotiate/Decline resolve are the primary trigger path, but
  // this periodic check catches anything that reaches a tier crossing
  // through a path that doesn't call it directly (e.g. a future PvP-driven
  // reputation effect) -- surfaces within ~180ms either way.
  // Round/phase banner first (v0.21.1), then any pending transmission --
  // the banner is the "new round begins" moment; a check-in coming due this
  // round follows it rather than cutting in ahead of it.
  // v0.21.18: the seat / faction pickers rebuild when someone else claims one;
  // v0.21.19: the Lobby too, and it goes Home once the game starts from any
  // device (rebuilt after gsTx goes out of scope -- the
  // screens read their own copy).
  bool joinStale = false, started = false;
  if (currentScreen != SCR_TRANSMISSION) {
    GameState gsTx = gameStateRead();
    const bool setup = currentScreen == SCR_LOBBY;
    started = setup && gsTx.stage == STAGE_PLAYING;
    joinStale = !started && (setup || currentScreen == SCR_SLOT_PICKER || currentScreen == SCR_FACTION_PICKER)
                && joinScreenKey(gsTx) != join_shown_key;
    updatePlanetRing(gsTx);
    maybeShowRoundBanner(gsTx);
    // v0.21.13: never cut into a battle or its report -- the Victory
    // screen announces the transmission and plays it after CONTINUE.
    // v0.30.0: nor a negotiation or the purchase card -- their CONTINUE plays it.
    if (currentScreen != SCR_BANNER && currentScreen != SCR_BATTLE && currentScreen != SCR_VICTORY
        && currentScreen != SCR_NEGOTIATE && currentScreen != SCR_CONFIRM_PAY)
      checkAndShowTransmission(gsTx);
  }
  if (started && currentScreen == SCR_LOBBY) {
    showHome();  // the round banner follows from Home on the next tick
    return;
  }
  if (joinStale) {
    if (currentScreen == SCR_SLOT_PICKER) showSlotPicker();
    else if (currentScreen == SCR_FACTION_PICKER) showFactionPicker();
    else showLobby();
    return;
  }

  // Game just ended (round limit reached) -- auto-navigate every device to
  // the Game Over screen, wherever they were. Doesn't interrupt an
  // in-progress Faction Transmission; that finishes and this catches it on
  // a later tick instead.
  if (currentScreen != SCR_GAME_OVER && currentScreen != SCR_TRANSMISSION) {
    GameState gsEnd = gameStateRead();
    if (gsEnd.stage == STAGE_GAME_OVER) {
      showGameOver();
    }
  }
  // v0.27.3: NEW GAME (from any device) -> a fresh Lobby: this console has lost
  // its seat, so back to the Start screen. v0.30.3: END GAME does the same from
  // any screen mid-game -- the seat is simply gone.
  if (MY_SLOT >= 0 && currentScreen != SCR_TRANSMISSION) {
    bool seatGone;
    {
      GameState gsNew = gameStateRead();
      seatGone = (currentScreen == SCR_GAME_OVER && gsNew.stage == STAGE_LOBBY)
              || !isMySeat(gsNew.players[MY_SLOT]);
    }
    if (seatGone) {
      MY_SLOT = -1;
      showStart();
    }
  }
}

static void drawEventCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  return;  // 2.0: no star field (frame rate); the table never had one (v0.25.7)
  lv_layer_t *layer = lv_event_get_layer(e);
  drawStars(layer);
}

static void clearScreen() {
  Serial.print("[SCR] ");  // 2.0: lets tools/console_drive.ps1 logs show where taps landed (no printf: stack)
  Serial.println((int)currentScreen);
  ui_back_btn = nullptr;  // 2.0-dev.7: the new screen registers its own back (edge swipe)
  ui_back_fn = nullptr;
  if (screen_root) {
    lv_obj_clean(screen_root);
  }
  screen_root = lv_scr_act();
  setScreenBackground(screen_root);
  lv_obj_remove_style_all(screen_root);
  setScreenBackground(screen_root);
  // v0.23.5: register the star field ONCE. lv_obj_clean() removes children,
  // not the screen's own event callbacks, so adding it on every screen change
  // stacked copies -- after a few dozen screens each frame drew the stars
  // dozens of times (the "slower every round" bug: 2.9 s of every 5 s).
  static bool starsHooked = false;
  if (!starsHooked) {
    lv_obj_add_event_cb(screen_root, drawEventCb, LV_EVENT_DRAW_MAIN, nullptr);
    starsHooked = true;
  }
  anim_obj_count = 0;  // the new screen registers its own animated objects
}

// Tracks whether the battle screen is showing (used by the battle refresh
// timer). Home uses its own onHomeScreen flag declared above.
static bool onBattleScreen = false;

// Standard two-line screen header (title + subtitle).
static void addHeader(const char *title, const char *subtitle) {
  title_label = lv_label_create(screen_root);
  lv_label_set_text(title_label, title);
  lv_obj_set_style_text_color(title_label, lv_color_hex(0xD7F7FF), 0);
  lv_obj_set_style_text_font(title_label, LV_FONT_DEFAULT, 0);
  lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 28);

  lv_obj_t *sub = lv_label_create(screen_root);
  lv_label_set_text(sub, subtitle);
  lv_obj_set_style_text_color(sub, lv_color_hex(0x62C7F2), 0);
  lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 56);
}

// ===========================================================================
// Shadow Fleet - Prototype 02  (multiplayer restructure, v0.7.0)
//
// The AMOLED is player slot 0 = "HOST". Phones join from the lobby as slots
// 1-3. This file is the HOST's on-device UI; it drives its own actions through
// the same game_state API the phones use, so all players share capabilities.
//
// Screen flow:
//   Start screen (HOST GAME) -> Lobby (see who's joined, START GAME)
//     -> Home (phase + lock-in + nav)
//       -> Planets (list -> detail: intel in Invest, takeover in Resolution)
//       -> Fleets (static roster for now)
//       -> Battle (only during a committed FIGHT takeover)
// ===========================================================================

static int currentPlanetIdx = 0;

// Forward declarations for screen builders / callbacks.
static void showStart();
static void showLobby();
static void showHome();
static void showPlanets(lv_event_t *e);
static void showPlanetDetail();
static void showConfirmPay();
static void showNegotiate();
static void askLeaveNegotiationCb(lv_event_t *e);
static void showFleets(lv_event_t *e);
static void showAttackChoice(lv_event_t *e);  // v0.27.3
static void showBattle();
static void reinforceCb(lv_event_t *e);  // v0.28.0
static void backToHome(lv_event_t *e);
static void showMarkets(lv_event_t *e);  // 2.0
static void showDeals(lv_event_t *e);    // 2.0
static void showAshen(lv_event_t *e);    // 2.0
static void showPvpTargets(lv_event_t *e);
static void showPvpFleetPick();
static void showFleetDetail();
static void showReputation(lv_event_t *e);
static void showPromises(lv_event_t *e);  // v0.27.3
static void showStandings(lv_event_t *e);
static void showSlotPicker();
static void showFactionPicker();

// PvP selection in progress on this device.
static int pvpTargetSlot = -1;
static int pvpAttackerFleet = 0;  // which of MY fleets attacks (picked on the Fleets screen, v0.21.10)
static int pvpDefenderFleet = -1;     // their fleet being struck
static bool forecast_pvp = false;     // Battle Forecast is showing a PvP attack (v0.21.10)
static const char *pvp_refusal = nullptr;  // why the last PvP ENGAGE was refused
static void pvpEngageCb(lv_event_t *e);
static void backFromPvpForecastCb(lv_event_t *e);
static int fleetAlive(const Player &pl, int f);
static int currentFleetIdx = 0;

// Join-flow state, in progress until MY_SLOT is actually assigned. The
// AMOLED has a fixed client id (only ever one AMOLED, unlike phones which
// need to disambiguate themselves) so it can rejoin its own slot the same
// way a phone reconnecting would. No typed name anymore -- your "name" is
// just your faction once you pick one.

// ---------------------------------------------------------------------------
// HYPERSPACE STREAKS -- 18 streaks at fixed angles; a dash of light races
// outward along each on a staggered ~1.1s loop. Shared by the round banner
// and the Start screen (v0.21.18).
// 2.0 frame rate: each tick redraws only the streaks' own boxes (where each
// was + where it is now), not the whole glass -- the draw uses the same `t`
// the tick invalidated for (warpInvalidate), never a fresher lv_tick_get().
// ---------------------------------------------------------------------------
static const int WARP_STREAKS = 18;
// Each streak's angle + loop offset, worked out once: warpStreak runs for every
// streak in every redrawn area, and shakeNoise's sinf on big numbers made
// that ~20 ms a frame on the 1.75".
static float warp_cos[WARP_STREAKS], warp_sin[WARP_STREAKS], warp_off[WARP_STREAKS];
static bool warp_tab_ok = false;
static void warpStreak(int k, uint32_t t, lv_point_t *p1, lv_point_t *p2, float *phaseOut) {
  if (!warp_tab_ok) {
    for (int i = 0; i < WARP_STREAKS; i++) {
      const float ang = i * 0.349f + shakeNoise(i * 11) * 0.25f;
      warp_cos[i] = cosf(ang);
      warp_sin[i] = sinf(ang);
      warp_off[i] = shakeNoise(i * 5 + 3);
    }
    warp_tab_ok = true;
  }
  float phase = (t % 1100) / 1100.0f + warp_off[k];
  if (phase >= 1.0f) phase -= 1.0f;
  float r0 = 60.0f + phase * 170.0f;
  float r1 = r0 + 14.0f + phase * 38.0f;  // streaks stretch as they fly out
  p1->x = 233 + (int)(warp_cos[k] * r0); p1->y = 233 + (int)(warp_sin[k] * r0);
  p2->x = 233 + (int)(warp_cos[k] * r1); p2->y = 233 + (int)(warp_sin[k] * r1);
  *phaseOut = phase;
}

// 2.0 frame rate: LVGL draws a slanted line row by row through four edge
// masks (~1 ms per streak on the 1.75"). Each streak keeps its angle, so its
// whole path (r 58..284) is drawn ONCE into an A8 image in PSRAM; a frame
// draws that image clipped to the streak's current segment -- one plain
// blend, no masks. (A straight ray clipped to a segment's box IS the segment.)
static lv_image_dsc_t warp_img[WARP_STREAKS];
static int32_t warp_img_x0[WARP_STREAKS], warp_img_y0[WARP_STREAKS];
static bool warp_img_ok = false, warp_img_tried = false;

static void warpBuildImages() {
  warp_img_tried = true;
  for (int k = 0; k < WARP_STREAKS; k++) {
    lv_point_t pa, pb;
    float ph;
    warpStreak(k, 0, &pa, &pb, &ph);  // fills the angle table
    const float c = warp_cos[k], s = warp_sin[k];
    const float ax = 233 + c * 58, ay = 233 + s * 58, bx = 233 + c * 284, by = 233 + s * 284;
    const int x0 = max(0, (int)floorf(min(ax, bx)) - 2), x1 = min(465, (int)ceilf(max(ax, bx)) + 2);
    const int y0 = max(0, (int)floorf(min(ay, by)) - 2), y1 = min(465, (int)ceilf(max(ay, by)) + 2);
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    uint8_t *buf = (uint8_t *)heap_caps_calloc((size_t)w * h, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;  // fall back to lines
    const float hw = (k % 3 == 0) ? 1.0f : 0.5f;  // half width: 2 px / 1 px
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const float px = x0 + x + 0.5f - 233, py = y0 + y + 0.5f - 233;
        const float along = px * c + py * s, d = fabsf(px * -s + py * c);
        if (along < 58 || along > 284) continue;
        const float cov = hw + 0.5f - d;
        if (cov > 0) buf[y * w + x] = cov >= 1 ? 255 : (uint8_t)(cov * 255);
      }
    }
    lv_image_dsc_t &img = warp_img[k];
    memset(&img, 0, sizeof(img));
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_A8;
    img.header.w = w;
    img.header.h = h;
    img.header.stride = w;
    img.data_size = (uint32_t)w * h;
    img.data = buf;
    warp_img_x0[k] = x0;  // where it sits on the glass
    warp_img_y0[k] = y0;
  }
  warp_img_ok = true;
}

static void drawWarpStreaks(lv_layer_t *layer, uint32_t t) {
  if (!warp_img_tried) warpBuildImages();
  for (int k = 0; k < WARP_STREAKS; k++) {
    lv_point_t a, b;
    float phase;
    warpStreak(k, t, &a, &b, &phase);
    if (!layerSees(layer, min(a.x, b.x) - 2, min(a.y, b.y) - 2, max(a.x, b.x) + 2, max(a.y, b.y) + 2)) continue;
    if (warp_img_ok) {
      const lv_area_t seg = { (int32_t)min(a.x, b.x) - 1, (int32_t)min(a.y, b.y) - 1, (int32_t)max(a.x, b.x) + 1, (int32_t)max(a.y, b.y) + 1 };
      const lv_area_t &lc = layer->_clip_area;
      const lv_area_t clip = { max(seg.x1, lc.x1), max(seg.y1, lc.y1), min(seg.x2, lc.x2), min(seg.y2, lc.y2) };
      if (clip.x1 > clip.x2 || clip.y1 > clip.y2) continue;
      lv_draw_image_dsc_t d;
      lv_draw_image_dsc_init(&d);
      d.src = &warp_img[k];
      d.recolor = lv_color_hex(0xCFE9FF);
      d.opa = (lv_opa_t)(60 + 170 * phase);
      const lv_area_t at = { warp_img_x0[k], warp_img_y0[k], warp_img_x0[k] + (int32_t)warp_img[k].header.w - 1, warp_img_y0[k] + (int32_t)warp_img[k].header.h - 1 };
      const lv_area_t saved = layer->_clip_area;
      layer->_clip_area = clip;  // the draw task copies the clip when it's added
      lv_draw_image(layer, &d, &at);
      layer->_clip_area = saved;
      continue;
    }
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = a.x; d.p1.y = a.y;
    d.p2.x = b.x; d.p2.y = b.y;
    d.color = lv_color_hex(0xCFE9FF);
    d.width = (k % 3 == 0) ? 2 : 1;
    d.opa = (lv_opa_t)(60 + 170 * phase);
    d.round_start = 1;
    d.round_end = 1;
    lv_draw_line(layer, &d);
  }
}

// One box per streak covering its old and new segment (two when it wrapped
// back to the center): ~19 areas, under LVGL's 32-area list.
static void warpInvalidate(lv_obj_t *obj, uint32_t tOld, uint32_t tNew) {
  for (int k = 0; k < WARP_STREAKS; k++) {
    lv_point_t a0, b0, a1, b1;
    float ph0, ph1;
    warpStreak(k, tOld, &a0, &b0, &ph0);
    warpStreak(k, tNew, &a1, &b1, &ph1);
    lv_area_t o = { (int32_t)min(a0.x, b0.x) - 3, (int32_t)min(a0.y, b0.y) - 3, (int32_t)max(a0.x, b0.x) + 3, (int32_t)max(a0.y, b0.y) + 3 };
    lv_area_t n = { (int32_t)min(a1.x, b1.x) - 3, (int32_t)min(a1.y, b1.y) - 3, (int32_t)max(a1.x, b1.x) + 3, (int32_t)max(a1.y, b1.y) + 3 };
    if (ph1 < ph0) { lv_obj_invalidate_area(obj, &o); lv_obj_invalidate_area(obj, &n); continue; }
    lv_area_t u = { min(o.x1, n.x1), min(o.y1, n.y1), max(o.x2, n.x2), max(o.y2, n.y2) };
    lv_obj_invalidate_area(obj, &u);
  }
}

// ---------------------------------------------------------------------------
// JOIN FLOW (v0.21.18 UI refresh; mockups "Start", "Choose seat", "Choose
// faction"). Start -> Seat -> Faction -> Lobby. (Commander step comes next.)
// The seat and faction pickers rebuild live when someone else claims a seat
// or faction (loop() watches joinScreenKey), instead of only after a failed
// tap.
// ---------------------------------------------------------------------------
// v0.22.1: unique per console ("CONSOLE-A1B2C3" from its MAC), so a command
// console joining a table console never collides with another console.
// (MY_CLIENT_ID is defined above autoRejoinCheck)
static bool isMySeat(const Player &p) { return p.active && strcmp(p.id, MY_CLIENT_ID) == 0; }
static lv_obj_t *start_warp_obj = nullptr;
static lv_timer_t *start_warp_timer = nullptr;
static uint32_t start_warp_t = 0;  // the moment the streaks are drawn at (see warpInvalidate)


// Who holds which seat and faction (+ stage): changes => picker is stale.
static uint32_t joinScreenKey(const GameState &gs) {
  uint32_t k = (uint32_t)gs.stage;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    k = k * 31 + (gs.players[i].active ? 1 : 0);
    k = k * 31 + (uint32_t)(gs.players[i].chosenFaction + 2);
    k = k * 31 + (uint32_t)(gs.players[i].commander + 2);  // v0.21.20: Lobby shows portraits
  }
  k = k * 31 + (uint32_t)gs.roundLimit;  // v0.21.19: the Lobby shows it too
  return k;
}

// v0.30.2 (playtest 2026-10-28: the 1.75" crashed and the game had to be
// restarted): a console that reboots mid-game still owns its seat (the table
// keeps it under this console's id), so JOIN GAME takes it straight back to
// where it left off instead of a seat picker where its seat looks taken.
static void joinGameCb(lv_event_t *e) {
  (void)e;
  int mine = -1, stage = STAGE_LOBBY, faction = -1, commander = -1;
  {
    GameState gs = gameStateRead();
    stage = gs.stage;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (gs.players[i].active && strcmp(gs.players[i].id, MY_CLIENT_ID) == 0) {
        mine = i;
        faction = gs.players[i].chosenFaction;
        commander = gs.players[i].commander;
        break;
      }
    }
  }
  if (mine < 0 || gameStateJoinSlot(MY_CLIENT_ID, "", mine) != mine) { showSlotPicker(); return; }
  MY_SLOT = mine;
  Serial.printf("[JOIN] rejoined seat %d (stage %d)\n", mine + 1, stage);
  if (faction < 0) showFactionPicker();
  else if (commander < 0) { cmd_pick_i = 0; showCommanderPicker(); }
  else if (stage == STAGE_LOBBY) showLobby();
  else showHome();  // the round banner / any pending transmission follow from Home
}

static void startWarpDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
#if defined(SF_PERF_PROBE) && (SF_PERF_PROBE & 1)
  return;  // perf_ab probe: the full-screen redraw without the streaks
#endif
  drawWarpStreaks(lv_event_get_layer(e), start_warp_t);
}

static void startWarpTick(lv_timer_t *timer) {
  (void)timer;
  if (currentScreen != SCR_START || !start_warp_obj) {
    lv_timer_del(start_warp_timer);
    start_warp_timer = nullptr;
    start_warp_obj = nullptr;
    return;
  }
  const uint32_t now = lv_tick_get();
  warpInvalidate(start_warp_obj, start_warp_t, now);
  start_warp_t = now;
}

// Start: hyperspace streaks behind the title, one JOIN GAME pill, and how
// phones get in. Pill (143..323, 262..344): far corner ~143px.
static void showStart() {
  currentScreen = SCR_START;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  start_warp_obj = lv_obj_create(screen_root);
  lv_obj_remove_style_all(start_warp_obj);
  lv_obj_set_size(start_warp_obj, 466, 466);
  lv_obj_set_pos(start_warp_obj, 0, 0);
  lv_obj_clear_flag(start_warp_obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(start_warp_obj, startWarpDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
  start_warp_t = lv_tick_get();
  if (!start_warp_timer) start_warp_timer = lv_timer_create(startWarpTick, 40, nullptr);
#if defined(SF_PERF_PROBE) && (SF_PERF_PROBE & 2)
  return;  // perf_ab probe: no title, pill or footer
#endif

  uiLabel(screen_root, "SHADOW", UI_TEXT, &sf_font_montserrat_36, 98, 8);
  uiLabel(screen_root, "FLEET", 0xFFC933, &sf_font_montserrat_36, 142, 8);
  uiLabel(screen_root, FIRMWARE_VERSION, UI_FAINT, &sf_font_montserrat_12, 194, 3);

  lv_obj_t *join = uiPill(screen_root, 143, 262, 180, 82, "JOIN GAME", nullptr, 0xFFC933, joinGameCb);
  (void)join;

  uiLabel(screen_root, "Phones: Wi-Fi ShadowFleet / starwars1\nthen open 192.168.4.1", UI_DIM,
          &lv_font_montserrat_14, 362, 0);
}

// ---------------------------------------------------------------------------
// SEAT -- four 110px circles around the center (P1 top, clockwise), on a
// faint dashed-looking orbit. Open seats are cyan and tappable; taken ones
// show the holder's insignia (or "?" while they're still choosing).
// Circles centered 130px out: outer edge 185px from center.
// ---------------------------------------------------------------------------
static const int SEAT_POS[MAX_PLAYERS][2] = { {233, 103}, {363, 233}, {233, 363}, {103, 233} };

static void slotPickCb(lv_event_t *e) {
  int slotIdx = (int)(intptr_t)lv_event_get_user_data(e);
  int result = gameStateJoinSlot(MY_CLIENT_ID, "", slotIdx);
  if (result == slotIdx) {
    MY_SLOT = result;
    showFactionPicker();
  } else {
    // Someone else claimed it a moment before we tapped -- refresh and let
    // the player pick again from the current, up-to-date state.
    showSlotPicker();
  }
}

static void showSlotPicker() {
  currentScreen = SCR_SLOT_PICKER;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  GameState gs = gameStateRead();
  join_shown_key = joinScreenKey(gs);

  lv_obj_t *orbit = lv_obj_create(screen_root);
  lv_obj_remove_style_all(orbit);
  lv_obj_set_style_border_color(orbit, lv_color_hex(UI_RING), 0);
  lv_obj_set_style_border_width(orbit, 1, 0);
  lv_obj_set_style_radius(orbit, LV_RADIUS_CIRCLE, 0);
  lv_obj_clear_flag(orbit, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(orbit, 260, 260);
  lv_obj_set_pos(orbit, 103, 103);

  uiLabel(screen_root, "CHOOSE\nYOUR SEAT", UI_TEXT, &lv_font_montserrat_14, 214, 3);

  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player &p = gs.players[i];
    const bool open = !p.active;
    lv_obj_t *btn = lv_btn_create(screen_root);
    uiStyleButton(btn, open ? UI_CYAN : 0x22303F, open ? 2 : 1, open ? uiTint(UI_CYAN) : UI_PANEL, LV_RADIUS_CIRCLE);
    lv_obj_set_size(btn, 110, 110);
    lv_obj_set_pos(btn, SEAT_POS[i][0] - 55, SEAT_POS[i][1] - 55);

    char num[4];
    snprintf(num, sizeof(num), "%d", i + 1);
    if (open) {
      lv_obj_t *t = lv_label_create(btn);
      lv_label_set_text(t, "PLAYER");
      lv_obj_set_style_text_font(t, &sf_font_montserrat_12, 0);
      lv_obj_set_style_text_letter_space(t, 2, 0);
      lv_obj_set_style_text_color(t, lv_color_hex(UI_CYAN), 0);
      lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
      lv_obj_t *n = lv_label_create(btn);
      lv_label_set_text(n, num);
      lv_obj_set_style_text_font(n, &sf_font_montserrat_36, 0);
      lv_obj_set_style_text_color(n, lv_color_hex(UI_CYAN), 0);
      lv_obj_align(n, LV_ALIGN_CENTER, 0, 0);
      lv_obj_t *o = lv_label_create(btn);
      lv_label_set_text(o, "OPEN");
      lv_obj_set_style_text_font(o, &sf_font_montserrat_12, 0);
      lv_obj_set_style_text_color(o, lv_color_hex(UI_CYAN), 0);
      lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -14);
      lv_obj_add_event_cb(btn, slotPickCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    } else {
      lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
      const int f = p.chosenFaction;
      if (f >= 0 && f < 4) {
        lv_obj_t *ins = uiInsignia(btn, f, 30, UI_PANEL);
        lv_obj_align(ins, LV_ALIGN_TOP_MID, 0, 18);
      } else {
        lv_obj_t *q = lv_label_create(btn);
        lv_label_set_text(q, "?");
        lv_obj_set_style_text_color(q, lv_color_hex(UI_FAINT), 0);
        lv_obj_align(q, LV_ALIGN_TOP_MID, 0, 20);
      }
      char who[24];
      snprintf(who, sizeof(who), "PLAYER %d", i + 1);
      lv_obj_t *t = lv_label_create(btn);
      lv_label_set_text(t, who);
      lv_obj_set_style_text_font(t, &sf_font_montserrat_12, 0);
      lv_obj_set_style_text_letter_space(t, 1, 0);
      lv_obj_set_style_text_color(t, lv_color_hex(UI_DIM), 0);
      lv_obj_align(t, LV_ALIGN_CENTER, 0, 12);
      lv_obj_t *s = lv_label_create(btn);
      lv_label_set_text(s, (f >= 0 && f < 4) ? FACTION_SHORT[f] : "JOINING");
      lv_obj_set_style_text_font(s, &sf_font_montserrat_12, 0);
      lv_obj_set_style_text_color(s, lv_color_hex((f >= 0 && f < 4) ? FACTION_TEXT_COLOR[f] : UI_FAINT), 0);
      lv_obj_align(s, LV_ALIGN_CENTER, 0, 30);
    }
  }
}

// ---------------------------------------------------------------------------
// FACTION -- 2x2 cards (insignia + name in its color); taken ones are dimmed
// with the holder's seat. One player per faction. Sets real starting
// reputation the moment a faction is claimed (see gameStateChooseFaction).
// Cards 140x120 in a 290x250 grid at (88,106): far corners ~193px.
// ---------------------------------------------------------------------------
static const char *FACTION_TWO_LINE[4] = { "REBEL\nALLIANCE", "GALACTIC\nEMPIRE", "PYKE\nSYNDICATE", "CRIMSON\nDAWN" };

static void factionPickCb(lv_event_t *e) {
  int factionIdx = (int)(intptr_t)lv_event_get_user_data(e);
  if (gameStateChooseFaction(MY_SLOT, factionIdx)) {
    cmd_pick_i = 0;
    showCommanderPicker();  // v0.21.20: then pick a commander
  } else {
    // Someone else claimed it a moment before we tapped -- refresh.
    showFactionPicker();
  }
}

static void showFactionPicker() {
  currentScreen = SCR_FACTION_PICKER;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  GameState gs = gameStateRead();
  join_shown_key = joinScreenKey(gs);

  char who[16];
  snprintf(who, sizeof(who), "PLAYER %d", MY_SLOT + 1);
  uiLabel(screen_root, who, UI_DIM, &sf_font_montserrat_12, 52, 3);
  uiLabel(screen_root, "CHOOSE YOUR FACTION", UI_TEXT, nullptr, 70, 2);

  for (int f = 0; f < 4; f++) {
    int holder = -1;
    for (int i = 0; i < MAX_PLAYERS; i++) {
      if (i != MY_SLOT && gs.players[i].active && gs.players[i].chosenFaction == f) holder = i;
    }
    const bool taken = holder >= 0;
    lv_obj_t *card = lv_btn_create(screen_root);
    uiStyleButton(card, taken ? 0x22303F : FACTION_RING_COLOR[f], taken ? 1 : 2,
                  taken ? UI_BG : uiTint(FACTION_RING_COLOR[f]), 18);
    lv_obj_set_size(card, 140, 120);
    lv_obj_set_pos(card, 88 + (f % 2) * 150, 106 + (f / 2) * 130);

    lv_obj_t *ins = uiInsignia(card, f, 40, taken ? UI_BG : uiTint(FACTION_RING_COLOR[f]));
    lv_obj_align(ins, LV_ALIGN_TOP_MID, 0, taken ? 12 : 16);
    lv_obj_t *n = lv_label_create(card);
    lv_label_set_text(n, FACTION_TWO_LINE[f]);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(n, 1, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(FACTION_TEXT_COLOR[f]), 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, taken ? 56 : 62);
    if (taken) {
      char tk[20];
      snprintf(tk, sizeof(tk), "TAKEN - P%d", holder + 1);
      lv_obj_t *t = lv_label_create(card);
      lv_label_set_text(t, tk);
      lv_obj_set_style_text_font(t, &sf_font_montserrat_12, 0);
      lv_obj_set_style_text_color(t, lv_color_hex(UI_DIM), 0);
      lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -8);
      lv_obj_set_style_opa(ins, LV_OPA_50, 0);
      lv_obj_set_style_opa(n, LV_OPA_50, 0);
      lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    } else {
      lv_obj_add_event_cb(card, factionPickCb, LV_EVENT_CLICKED, (void *)(intptr_t)f);
    }
  }

  uiLabel(screen_root, "Allied with your own faction,\nHostile with its rival", UI_DIM, &lv_font_montserrat_14, 368, 0);
}

// ---------------------------------------------------------------------------
// COMMANDER (v0.21.20; mockup "AMOLED · Commander pick"). Your faction's
// three commanders: the chosen one big in a ring at the center (5x art,
// clipped to the circle), the other two as small faces left and right --
// tap one to bring it to the center. Name + one-line bio, TAKE COMMAND pill,
// back circle to the faction cards. No two players can share a commander
// (factions are already one per player).
// Touch: side faces 68px at (100,150)/(366,150) -- edge ~191px; back 56px at
// (113,372) -- edge ~212px; pill 170x72 at (158,336) -- corner ~199px.
// ---------------------------------------------------------------------------

static void commanderSideCb(lv_event_t *e) {
  cmd_pick_i = (int)(intptr_t)lv_event_get_user_data(e);
  showCommanderPicker();
}

static void commanderTakeCb(lv_event_t *e) {
  (void)e;
  GameState gs = gameStateRead();
  const int f = gs.players[MY_SLOT].chosenFaction;
  if (f >= 0 && gameStateChooseCommander(MY_SLOT, f * COMMANDERS_PER_FACTION + cmd_pick_i)) showLobby();
  else showFactionPicker();
}

static void commanderBackCb(lv_event_t *e) {
  (void)e;
  showFactionPicker();
}

static void showCommanderPicker() {
  currentScreen = SCR_COMMANDER_PICKER;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  GameState gs = gameStateRead();
  const int f = gs.players[MY_SLOT].chosenFaction;
  if (f < 0 || f > 3) { showFactionPicker(); return; }
  const uint32_t ring = FACTION_RING_COLOR[f], text = FACTION_TEXT_COLOR[f];
  if (cmd_pick_i < 0 || cmd_pick_i > 2) cmd_pick_i = 0;
  const int id = f * COMMANDERS_PER_FACTION + cmd_pick_i;

  uiLabel(screen_root, "CHOOSE YOUR COMMANDER", UI_DIM, &sf_font_montserrat_12, 34, 3);

  // Center: the chosen commander, 5x (120x140), sitting on the bottom of a
  // 168px ring and clipped to it.
  lv_obj_t *halo = lv_obj_create(screen_root);
  uiStyleButton(halo, ring, 1, UI_BG, LV_RADIUS_CIRCLE);  // style first (Convention 13)
  lv_obj_set_style_border_opa(halo, LV_OPA_30, 0);
  lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(halo, 192, 192);
  lv_obj_set_pos(halo, 233 - 96, 150 - 96);
  lv_obj_t *disc = lv_obj_create(screen_root);
  uiStyleButton(disc, ring, 2, uiTint(ring), LV_RADIUS_CIRCLE);
  lv_obj_set_style_clip_corner(disc, true, 0);
  lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(disc, 168, 168);
  lv_obj_set_pos(disc, 233 - 84, 150 - 84);
  lv_obj_t *big = uiPortrait(disc, id, CMD_IMG_BIG);
  lv_obj_align(big, LV_ALIGN_BOTTOM_MID, 0, 2);

  // The other two, small, left and right -- tap to bring to the center.
  const int sides[2] = { (cmd_pick_i + 2) % 3, (cmd_pick_i + 1) % 3 };
  for (int k = 0; k < 2; k++) {
    lv_obj_t *b = lv_btn_create(screen_root);
    uiStyleButton(b, UI_LINE, 1, UI_PANEL, LV_RADIUS_CIRCLE);
    lv_obj_set_style_clip_corner(b, true, 0);
    lv_obj_set_size(b, 68, 68);
    lv_obj_set_pos(b, (k ? 366 : 100) - 34, 150 - 34);
    lv_obj_t *im = uiPortrait(b, f * COMMANDERS_PER_FACTION + sides[k], CMD_IMG_SMALL);
    lv_obj_set_style_opa(im, LV_OPA_60, 0);
    lv_obj_align(im, LV_ALIGN_BOTTOM_MID, 0, 2);
    lv_obj_add_event_cb(b, commanderSideCb, LV_EVENT_CLICKED, (void *)(intptr_t)sides[k]);
  }

  // Which of the three: a pill for the chosen one, dots for the others.
  for (int k = 0; k < 3; k++) {
    lv_obj_t *d = lv_obj_create(screen_root);
    lv_obj_remove_style_all(d);
    lv_obj_set_style_bg_color(d, lv_color_hex(k == cmd_pick_i ? ring : 0x3D5063), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d, 4, 0);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    const int w = k == cmd_pick_i ? 22 : 8;
    int x = 233 - (22 + 8 + 8 + 16) / 2;
    for (int j = 0; j < k; j++) x += (j == cmd_pick_i ? 22 : 8) + 8;
    lv_obj_set_size(d, w, 8);
    lv_obj_set_pos(d, x, 252);
  }

  uiLabel(screen_root, COMMANDER_INFO[id].name, text, nullptr, 266, 1);
  lv_obj_t *bio = uiLabel(screen_root, COMMANDER_INFO[id].bio, 0xA9BDD0, &lv_font_montserrat_14, 292, 0);
  lv_label_set_long_mode(bio, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(bio, 330);
  lv_obj_align(bio, LV_ALIGN_TOP_MID, 0, 292);

  uiPill(screen_root, 128, 336, 210, 72, "TAKE COMMAND", nullptr, text, commanderTakeCb);  // 2.0-dev.9: centered (no back button); corners ~204 px
  lv_obj_t *back = uiBackButton(screen_root, 113, 372, commanderBackCb);
  lv_obj_set_size(back, 56, 56);
  lv_obj_set_pos(back, 113 - 28, 372 - 28);
}

// ---------------------------------------------------------------------------
// LOBBY (v0.21.19 UI refresh; mockup "Lobby"). Four player circles (insignia
// in its color, "?" while choosing, empty when open), GAME LENGTH 6/8/10 and
// START -- greyed until every seated player has a faction. Any device can
// start (phones too, via /start_game). starTick rebuilds this screen when
// joinScreenKey changes (players, factions, game length) and moves to Home
// once the game starts, wherever it was started from. No planet picker: the
// printed board has every planet, so all 12 are always in play (v0.21.19).
// Touch: length pills 84x72 at y202 (x 98/191/284); START 180x72 at
// (143,288) -- far corner (323,360) ~156px.
// ---------------------------------------------------------------------------
static void startGameCb(lv_event_t *e) {
  (void)e;
  gameStateStartGame();
  showHome();
}

static void roundLenBtnCb(lv_event_t *e) {
  int rounds = (int)(intptr_t)lv_event_get_user_data(e);
  gameStateSetRoundLimit(rounds);
  showLobby();  // rebuild to refresh which button is highlighted
}

// A pill in the quiet (unselected / unavailable) style: hairline border,
// dim text.
static lv_obj_t *uiQuietPill(int x, int y, int w, int h, const char *text, const char *sub, lv_event_cb_t cb) {
  lv_obj_t *b = uiPill(screen_root, x, y, w, h, text, sub, UI_DIM, cb);
  lv_obj_set_style_border_color(b, lv_color_hex(UI_LINE), 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(UI_BG), 0);
  return b;
}

static void showLobby() {
  currentScreen = SCR_LOBBY;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  GameState gs = gameStateRead();
  join_shown_key = joinScreenKey(gs);
  uiLabel(screen_root, "LOBBY", UI_TEXT, nullptr, 46, 5);

  int waitingOn = -1, seated = 0;
  bool waitingCommander = false;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    const Player &p = gs.players[i];
    const int f = p.active ? p.chosenFaction : -1;
    const bool hasF = f >= 0 && f < 4;
    if (p.active) seated++;
    const bool hasC = hasF && p.commander >= 0;
    if (p.active && !hasC && waitingOn < 0) { waitingOn = i; waitingCommander = hasF; }
    const int cx = 114 + i * 79;

    lv_obj_t *c = lv_obj_create(screen_root);
    uiStyleButton(c, hasF ? FACTION_RING_COLOR[f] : UI_LINE, hasF ? 2 : 1, hasF ? uiTint(FACTION_RING_COLOR[f]) : UI_PANEL,
                  LV_RADIUS_CIRCLE);  // style first (Convention 13)
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(c, 68, 68);
    lv_obj_set_pos(c, cx - 34, 82);
    if (hasC) {  // v0.21.20: the commander's face
      lv_obj_set_style_clip_corner(c, true, 0);
      lv_obj_t *im = uiPortrait(c, p.commander, CMD_IMG_SMALL);
      lv_obj_align(im, LV_ALIGN_BOTTOM_MID, 0, 2);
    } else if (hasF) {
      lv_obj_t *ins = uiInsignia(c, f, 34, uiTint(FACTION_RING_COLOR[f]));
      lv_obj_center(ins);
    } else if (p.active) {
      lv_obj_t *q = lv_label_create(c);
      lv_label_set_text(q, "?");
      lv_obj_set_style_text_color(q, lv_color_hex(UI_FAINT), 0);
      lv_obj_center(q);
    }

    char nm[24];
    if (!p.active) snprintf(nm, sizeof(nm), "P%d OPEN", i + 1);
    else if (i == MY_SLOT) snprintf(nm, sizeof(nm), "P%d - YOU", i + 1);
    else if (hasF) snprintf(nm, sizeof(nm), "P%d %s", i + 1, FACTION_SHORT[f]);
    else snprintf(nm, sizeof(nm), "P%d CHOOSING", i + 1);
    lv_obj_t *l = lv_label_create(screen_root);
    lv_label_set_text(l, nm);
    lv_obj_set_style_text_font(l, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(hasF ? FACTION_TEXT_COLOR[f] : UI_FAINT), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, 78);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(l, cx - 39, 154);
  }

  uiLabel(screen_root, "GAME LENGTH", UI_DIM, &sf_font_montserrat_12, 184, 3);
  const int rounds[3] = { 6, 8, 10 };
  for (int i = 0; i < 3; i++) {
    char t[8];
    snprintf(t, sizeof(t), "%d", rounds[i]);
    const int x = 98 + i * 93;
    lv_obj_t *b = (gs.roundLimit == rounds[i])
        ? uiPill(screen_root, x, 202, 84, 72, t, nullptr, UI_CYAN, nullptr)
        : uiQuietPill(x, 202, 84, 72, t, nullptr, nullptr);
    lv_obj_add_event_cb(b, roundLenBtnCb, LV_EVENT_CLICKED, (void *)(intptr_t)rounds[i]);
  }

  const bool ready = waitingOn < 0 && seated > 0;
  if (ready) uiPill(screen_root, 143, 288, 180, 72, "START", nullptr, 0xFFC933, startGameCb);
  else uiQuietPill(143, 288, 180, 72, "START", nullptr, nullptr);

  char st[64];
  if (!ready) snprintf(st, sizeof(st), "Waiting for Player %d to pick a %s", waitingOn + 1, waitingCommander ? "commander" : "faction");
  else snprintf(st, sizeof(st), "%d commander%s ready - anyone can start", seated, seated == 1 ? "" : "s");
  uiLabel(screen_root, st, ready ? UI_DIM : 0xFF9B70, &lv_font_montserrat_14, 372, 0);
  if (MY_SLOT >= 0 && gs.players[MY_SLOT].chosenFaction >= 0) {  // 2.0: what you play for
    const int f = gs.players[MY_SLOT].chosenFaction;
    char q[96];
    snprintf(q, sizeof(q), "QUEST: %s\nSTRENGTH: %s", questName(f), strengthName(f));  // names only: the round edge clips long lines
    lv_obj_t *l = lv_label_create(screen_root);
    lv_label_set_text(l, q);
    lv_obj_set_style_text_font(l, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(UI_DIM), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 300);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 394);
  }
}

// ---------------------------------------------------------------------------
// HOME (phase + lock-in + nav)
// ---------------------------------------------------------------------------
static void showLockConfirm(const GameState &gs, int readyMask, bool offersOpen);

static void lockInCb(lv_event_t *e) {
  (void)e;
  GameState gs = gameStateRead();
  if (gs.players[MY_SLOT].lockedIn) return;
  // Anything left on the table? Then confirm first (v0.21.1).
  // v0.30.2: every ready fleet, not just the first (playtest: "only ever says fleet 1").
  int readyMask = 0;
  if (gs.phase == PHASE_RESOLUTION) {
    for (int f = 0; f < NUM_FLEETS; f++) {
      if (gs.players[MY_SLOT].fleetSpent[f]) continue;
      for (int s = 0; s < FLEET_SHIPS; s++) if (gs.players[MY_SLOT].fleetShipAlive[f][s]) { readyMask |= 1 << f; break; }
    }
  }
  bool offersOpen = gs.takeoverPlanet[MY_SLOT] >= 0 && gs.takeoverMethod[MY_SLOT] == TAKEOVER_NEGOTIATE && gs.negResult[MY_SLOT] == NEG_TALKING;
  if (gs.players[MY_SLOT].credits >= 2 || readyMask || offersOpen || snapshotDealsWaiting(gs, MY_SLOT)) {
    showLockConfirm(gs, readyMask, offersOpen);
    return;
  }
  gameStateLockIn(MY_SLOT);
  gs = gameStateRead();  // reuse the one ~4KB copy -- never two on the LVGL stack
  updateHomeUI(gs);
}

// ---------------------------------------------------------------------------
// HOME HUB (v0.21.0 UI refresh; mockup "3 . Home hub"). Rim ring = round
// progress. Top status cap (round / phase / CR / VP rank) is a hidden tap
// zone into Standings -- no extra button. GALAXY is the one big action;
// FLEETS and FACTIONS flank it; LOCK IN pill at the bottom shows who's
// ready. During Resolution the FLEETS circle becomes a red ATTACK circle
// (v0.21.10) that opens Fleets in attack mode.
// Handles: phase_title_label = phase name, phase_status_label = CR/VP line,
// lock_in_button = pill (child 0 main text, child 1 subline).
// ---------------------------------------------------------------------------
static int homeShownPhase = -1;  // phase the current Home screen's nav buttons reflect
static lv_obj_t *home_round_label = nullptr;
static int home_planet_idx = 0;  // planet shown turning inside the GALAXY button
static void spinPlanetDrawCb(lv_event_t *e);  // turning pixel planet; user_data = planet index (defined with the battle scene)

// 1 = leading on VP; ties share a rank.
static int myVpRank(const GameState &gs) {
  int mine = gs.players[MY_SLOT].vp, rank = 1;
  for (int i = 0; i < MAX_PLAYERS; i++)
    if (i != MY_SLOT && gs.players[i].active && gs.players[i].vp > mine) rank++;
  return rank;
}

static const char *ordinalStr(int n) {
  switch (n) { case 1: return "1st"; case 2: return "2nd"; case 3: return "3rd"; default: return "4th"; }
}

static void homeStandingsCb(lv_event_t *e) { showStandings(e); }

static void updateHomeUI(const GameState &gs) {
  if (currentScreen != SCR_HOME || !phase_title_label || !phase_status_label || !lock_in_button
      || !home_round_label) return;

  // The phase changed since this screen was built (most commonly: your own
  // LOCK IN was the one that advanced it). Phase-specific buttons (ATTACK)
  // are only created at build time -- do a full rebuild instead.
  if (gs.phase != homeShownPhase) {
    showHome();
    return;
  }

  char buf[64];
  snprintf(buf, sizeof(buf), "ROUND %d / %d", gs.round, gs.roundLimit);
  lv_label_set_text(home_round_label, buf);
  lv_label_set_text(phase_title_label, PHASE_NAMES[gs.phase]);
  snprintf(buf, sizeof(buf), "%d CR    %d VP  %s  " LV_SYMBOL_RIGHT,
           gs.players[MY_SLOT].credits, gs.players[MY_SLOT].vp, ordinalStr(myVpRank(gs)));
  lv_label_set_text(phase_status_label, buf);

  int active = 0, locked = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!gs.players[i].active) continue;
    active++;
    if (gs.players[i].lockedIn) locked++;
  }
  lv_obj_t *mainLabel = lv_obj_get_child(lock_in_button, 0);
  lv_obj_t *subLabel = lv_obj_get_child(lock_in_button, 1);
  if (gs.players[MY_SLOT].lockedIn) {
    if (mainLabel) lv_label_set_text(mainLabel, "LOCKED IN");
    int waiting = active - locked;
    snprintf(buf, sizeof(buf), "waiting for %d commander%s", waiting, waiting == 1 ? "" : "s");
  } else {
    if (mainLabel) lv_label_set_text(mainLabel, "LOCK IN");
    snprintf(buf, sizeof(buf), "%d of %d ready", locked, active);
  }
  if (subLabel) lv_label_set_text(subLabel, buf);
}

static void showHome() {
  currentScreen = SCR_HOME;
  onHomeScreen = true;
  onBattleScreen = false;
  clearScreen();

  GameState gsNav = gameStateRead();
  homeShownPhase = gsNav.phase;
  const uint32_t accent = uiAccentFor(gsNav.players[MY_SLOT].chosenFaction);
  uiAddRim(gsNav.roundLimit, gsNav.round, accent);

  // Top status cap -> Standings. ~240x106 invisible tap zone around the
  // text (bigger than any button); a subtle panel shows while pressed.
  lv_obj_t *cap = lv_obj_create(screen_root);
  lv_obj_remove_style_all(cap);
  lv_obj_set_size(cap, 240, 106);
  lv_obj_set_pos(cap, 113, 44);
  lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(cap, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(cap, 24, 0);
  lv_obj_set_style_bg_color(cap, lv_color_hex(UI_PANEL), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_add_event_cb(cap, homeStandingsCb, LV_EVENT_CLICKED, nullptr);

  home_round_label = lv_label_create(cap);
  lv_obj_set_style_text_font(home_round_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(home_round_label, lv_color_hex(UI_DIM), 0);
  lv_obj_set_style_text_letter_space(home_round_label, 3, 0);
  lv_obj_align(home_round_label, LV_ALIGN_TOP_MID, 0, 8);

  phase_title_label = lv_label_create(cap);
  lv_obj_set_style_text_color(phase_title_label, lv_color_hex(accent), 0);
  lv_obj_set_style_text_letter_space(phase_title_label, 3, 0);
  lv_obj_align(phase_title_label, LV_ALIGN_TOP_MID, 0, 28);

  phase_status_label = lv_label_create(cap);
  lv_obj_set_style_text_font(phase_status_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(phase_status_label, lv_color_hex(UI_TEXT), 0);
  lv_obj_align(phase_status_label, LV_ALIGN_TOP_MID, 0, 62);

  // GALAXY: the one big action, with one of your planets (or Tatooine)
  // turning inside it. Planets list/detail are rebuilt in a later step.
  home_planet_idx = 0;
  for (int p = 0; p < NUM_PLANETS; p++) {
    if (gsNav.planetOwner[p] == MY_SLOT) { home_planet_idx = p; break; }
  }
  lv_obj_t *galaxy = uiCircleButton(screen_root, 233, 233, 156, "GALAXY", accent, true, showPlanets);
  lv_obj_t *galLabel = lv_obj_get_child(galaxy, 0);
  lv_obj_set_style_text_letter_space(galLabel, 2, 0);
  lv_obj_align(galLabel, LV_ALIGN_BOTTOM_MID, 0, -34);
  lv_obj_t *mini = lv_obj_create(galaxy);
  lv_obj_remove_style_all(mini);
  lv_obj_set_size(mini, 56, 56);
  lv_obj_align(mini, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_clear_flag(mini, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(mini, spinPlanetDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)home_planet_idx);
  trackAnimated(mini);

  // v0.21.10: in Resolution FLEETS becomes the red ATTACK circle (user's
  // idea) -- the Fleets screen is the attack screen then. Replaces the
  // separate lower-right ATTACK circle the user found awkward.
  if (gsNav.phase == PHASE_RESOLUTION) uiCircleButton(screen_root, 92, 233, 100, "ATTACK", UI_RED, true, showAttackChoice);  // v0.27.3
  else uiCircleButton(screen_root, 92, 233, 100, "FLEETS", accent, false, showFleets);
  const bool blockade = snapshotAshenAlive(gsNav);
  lv_obj_t *factions = (gsNav.phase == PHASE_INVEST)  // 2.0: MARKETS during Prepare (CLOSED while the Ashen Fleet lives)
      ? uiCircleButton(screen_root, 374, 233, 100, blockade ? "MARKETS\nCLOSED" : "MARKETS", blockade ? UI_RED : 0xFFD166, true, showMarkets)
      : uiCircleButton(screen_root, 374, 233, 100, "FACTIONS", accent, false, showReputation);
  lv_obj_set_style_text_font(lv_obj_get_child(factions, 0), &lv_font_montserrat_14, 0);

  // Corners: pill (123,406) ~205px from center -- touch-safe.
  // 2.0: in Prepare a DEALS circle sits lower left (visible edge ~213 px from
  // center) with a red count of offers waiting on you; LOCK IN narrows to fit.
  if (gsNav.phase == PHASE_INVEST) {
    lv_obj_t *deals = uiCircleButton(screen_root, 100, 338, 72, "DEALS", accent, false, showDeals);
    lv_obj_set_style_text_font(lv_obj_get_child(deals, 0), &sf_font_montserrat_12, 0);
    const int waiting = snapshotDealsWaiting(gsNav, MY_SLOT);
    if (waiting) {
      lv_obj_t *b = lv_obj_create(screen_root);
      uiStyleButton(b, UI_RED, 0, UI_RED, LV_RADIUS_CIRCLE);
      lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(b, 26, 26);
      lv_obj_set_pos(b, 113, 295);
      lv_obj_t *n = lv_label_create(b);
      char w[4];
      snprintf(w, sizeof(w), "%d", waiting);
      lv_label_set_text(n, w);
      lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
      lv_obj_set_style_text_color(n, lv_color_hex(0xFFFFFF), 0);
      lv_obj_center(n);
    }
    lock_in_button = uiPill(screen_root, 153, 324, 180, 82, "LOCK IN", " ", UI_CYAN, lockInCb);
  } else {
    lock_in_button = uiPill(screen_root, 123, 324, 220, 82, "LOCK IN", " ", UI_CYAN, lockInCb);
  }

  // Reuses gsNav rather than a second GameState copy (two stacked ~4KB
  // copies in showHome -> updateHomeUI once overflowed the LVGL stack).
  updateHomeUI(gsNav);
}

// ---------------------------------------------------------------------------
// ROUND / PHASE BANNER (v0.21.1 UI refresh; mockup "1 . Round banner").
// Shown whenever the round or phase changes (and at game start): animated
// hyperspace streaks racing outward, the round number as big segmented
// digits (drawn with lines -- our largest font is 20px and a transformed
// label would cost an offscreen pass per frame), the rim ring, and the
// phase name. Tap or wait ~3s to continue to Home. If the change happens
// while you're mid-battle / in a dialog, it waits until you're back on a
// calm screen (Home, lists, lobby).
// ---------------------------------------------------------------------------
static const uint32_t BANNER_MS = 3000;
static int banner_seen_key = -1;         // round*4 + phase last bannered
static lv_obj_t *banner_obj = nullptr;
static lv_timer_t *banner_timer = nullptr;
static uint32_t banner_start = 0;
static uint32_t banner_t = 0;  // ms into the banner the streaks are drawn at (see warpInvalidate)
static int banner_round = 1, banner_phase = 0;
static uint32_t banner_accent = 0x5FD4FF;

// Classic 7-segment masks, bit 0..6 = segments a..g.
static const uint8_t SEG7[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };

static void drawSegDigit(lv_layer_t *layer, int d, int x, int y, int w, int h, lv_color_t c, int t) {
  uint8_t m = SEG7[d % 10];
  int g = t;  // inset so segment ends don't overlap
  int mid = y + h / 2;
  if (m & 0x01) uiLine(layer, x + g, y, x + w - g, y, c, t);                  // a
  if (m & 0x02) uiLine(layer, x + w, y + g, x + w, mid - g, c, t);            // b
  if (m & 0x04) uiLine(layer, x + w, mid + g, x + w, y + h - g, c, t);        // c
  if (m & 0x08) uiLine(layer, x + g, y + h, x + w - g, y + h, c, t);          // d
  if (m & 0x10) uiLine(layer, x, mid + g, x, y + h - g, c, t);                // e
  if (m & 0x20) uiLine(layer, x, y + g, x, mid - g, c, t);                    // f
  if (m & 0x40) uiLine(layer, x + g, mid, x + w - g, mid, c, t);              // g
}

static void bannerDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  drawWarpStreaks(layer, banner_t);

  // Round number as segmented digits, centered; "/ N" drawn by a label.
  char num[4];
  snprintf(num, sizeof(num), "%d", banner_round);
  int n = strlen(num);
  const int dw = 46, dh = 92, gap = 22, th = 8;
  int total = n * dw + (n - 1) * gap;
  int x0 = 233 - total / 2 - 18, y0 = 148;
  for (int i = 0; i < n; i++) {
    drawSegDigit(layer, num[i] - '0', x0 + i * (dw + gap), y0, dw, dh, lv_color_hex(UI_TEXT), th);
  }
}

static void bannerDone() {
  if (banner_timer) { lv_timer_del(banner_timer); banner_timer = nullptr; }
  banner_obj = nullptr;
  showHome();
}

static void bannerTick(lv_timer_t *timer) {
  (void)timer;
  if (currentScreen != SCR_BANNER || !banner_obj) {
    lv_timer_del(banner_timer);
    banner_timer = nullptr;
    return;
  }
  const uint32_t t = lv_tick_get() - banner_start;
  warpInvalidate(banner_obj, banner_t, t);
  banner_t = t;
  if (t > BANNER_MS) bannerDone();
}

static void bannerTapCb(lv_event_t *e) {
  (void)e;
  bannerDone();
}

static void showRoundBanner(const GameState &gs) {
  currentScreen = SCR_BANNER;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  banner_round = gs.round;
  banner_phase = gs.phase;
  banner_accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  banner_start = lv_tick_get();
  banner_t = 0;

  banner_obj = lv_obj_create(screen_root);
  lv_obj_remove_style_all(banner_obj);
  lv_obj_set_size(banner_obj, 466, 466);
  lv_obj_set_pos(banner_obj, 0, 0);
  lv_obj_clear_flag(banner_obj, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(banner_obj, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(banner_obj, bannerDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(banner_obj, bannerTapCb, LV_EVENT_CLICKED, nullptr);
  uiAddRim(gs.roundLimit, gs.round, UI_CYAN);

  uiLabel(screen_root, "ROUND", UI_DIM, &lv_font_montserrat_14, 118, 6);
  char of[12];
  snprintf(of, sizeof(of), "/ %d", gs.roundLimit);
  lv_obj_t *ofl = lv_label_create(screen_root);
  lv_label_set_text(ofl, of);
  lv_obj_set_style_text_color(ofl, lv_color_hex(UI_DIM), 0);
  int n = gs.round >= 10 ? 2 : 1;
  lv_obj_align(ofl, LV_ALIGN_TOP_MID, (n * 46 + (n - 1) * 22) / 2 + 12, 214);
  char ph[24];
  snprintf(ph, sizeof(ph), "%s PHASE", PHASE_NAMES[gs.phase]);
  uiLabel(screen_root, ph, UI_CYAN, nullptr, 262, 4);
  // v0.23.1: a new round pays income -- say how much, on its Invest banner.
  const int income = gs.players[MY_SLOT].lastIncome;
  if (gs.phase == PHASE_INVEST && gs.round > 1 && income > 0) {
    char inc[32];
    snprintf(inc, sizeof(inc), "+%d CR INCOME", income);
    uiLabel(screen_root, inc, 0xFFD166, &lv_font_montserrat_14, 292, 3);
  }
  uiLabel(screen_root, "TAP TO CONTINUE", UI_FAINT, &lv_font_montserrat_14, 318, 2);

  if (!banner_timer) banner_timer = lv_timer_create(bannerTick, 33, nullptr);
}

// Called from starTick: banner a round/phase change once the player is on a
// calm screen (never interrupts battles, dialogs, or transmissions).
static void maybeShowRoundBanner(const GameState &gs) {
  if (gs.stage != STAGE_PLAYING || MY_SLOT < 0) return;
  int key = gs.round * 4 + gs.phase;
  if (key == banner_seen_key) return;
  bool calm = currentScreen == SCR_HOME || currentScreen == SCR_LOBBY || currentScreen == SCR_PLANETS
              || currentScreen == SCR_FLEETS || currentScreen == SCR_REPUTATION || currentScreen == SCR_STANDINGS;
  if (!calm) return;
  banner_seen_key = key;
  showRoundBanner(gs);
}

// ---------------------------------------------------------------------------
// LOCK-IN CONFIRM (v0.21.1; mockup "Lock-in confirm") -- Fire Emblem's "End
// turn?". Only appears when something is left on the table (2+ Credits
// unspent, a fleet ready to fight in Resolution, or negotiation offers
// still open), so it doesn't add a tap to every lock-in.
// ---------------------------------------------------------------------------
static void lockConfirmStayCb(lv_event_t *e) { (void)e; showHome(); }

static void lockConfirmGoCb(lv_event_t *e) {
  (void)e;
  gameStateLockIn(MY_SLOT);
  showHome();
}

static void showLockConfirm(const GameState &gs, int readyMask, bool offersOpen) {
  currentScreen = SCR_LOCK_CONFIRM;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  uiAddRim(gs.roundLimit, gs.round, UI_CYAN);

  char buf[48];
  snprintf(buf, sizeof(buf), "END YOUR %s PHASE?", PHASE_NAMES[gs.phase]);
  uiLabel(screen_root, buf, UI_DIM, &lv_font_montserrat_14, 74, 2);
  uiLabel(screen_root, "LOCK IN?", UI_CYAN, nullptr, 94, 4);
  uiLabel(screen_root, "You still have", UI_DIM, &lv_font_montserrat_14, 150, 1);

  int y = 176;
  const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  if (gs.players[MY_SLOT].credits >= 2) {
    snprintf(buf, sizeof(buf), "%d Credits unspent", gs.players[MY_SLOT].credits);
    uiLabel(screen_root, buf, accent, nullptr, y, 0);
    y += 34;
  }
  if (readyMask) {
    // "Fleet 2" / "Fleets 1 + 3" / "Fleets 1, 2 + 3"
    char nums[16] = "";
    int n = 0, total = 0;
    for (int f = 0; f < NUM_FLEETS; f++) if (readyMask & (1 << f)) total++;
    for (int f = 0; f < NUM_FLEETS; f++) {
      if (!(readyMask & (1 << f))) continue;
      const size_t len = strlen(nums);
      snprintf(nums + len, sizeof(nums) - len, "%s%d", n == 0 ? "" : (n == total - 1 ? " + " : ", "), f + 1);
      n++;
    }
    snprintf(buf, sizeof(buf), "Fleet%s %s ready to fight", total > 1 ? "s" : "", nums);
    uiLabel(screen_root, buf, accent, nullptr, y, 0);
    y += 34;
  }
  if (offersOpen) {
    uiLabel(screen_root, "A negotiation is still going", UI_CYAN, nullptr, y, 0);
    y += 34;
  }
  if (const int waiting = snapshotDealsWaiting(gs, MY_SLOT)) {  // 2.0 deals
    snprintf(buf, sizeof(buf), "%d offer%s unanswered", waiting, waiting == 1 ? "" : "s");
    uiLabel(screen_root, buf, UI_RED, nullptr, y, 0);
  }

  // Corners (93,402)/(373,402) ~219px from center.
  uiMarkBack(uiPill(screen_root, 93, 330, 120, 72, "STAY", nullptr, UI_DIM, lockConfirmStayCb), false);  // edge swipe = STAY
  uiPill(screen_root, 223, 330, 150, 72, "LOCK IN", nullptr, UI_CYAN, lockConfirmGoCb);
}

// ---------------------------------------------------------------------------
// FACTIONS (v0.21.17 UI refresh; mockup "Factions / reputation") -- your
// standing with each faction, tier only (never the raw score): insignia, name
// in its color, five pips lit up to your tier, and the tier word, colored by
// how they feel about you. Your own faction is marked YOU. (The old TEST
// COMMS demo button is gone -- it played canned lines that didn't match the
// game; real context-aware transmissions are on the roadmap.)
// Rows 320x62 at y 84 + 68i (not tappable); back circle 72 at (233,412),
// edge ~215px from center.
// ---------------------------------------------------------------------------
static uint32_t repTierColor(int tier) {
  switch (tier) {
    case REP_HOSTILE: return UI_RED;
    case REP_WARY: return 0xFF9B70;
    case REP_NEUTRAL: return 0xA9BDD0;
    case REP_FAVORABLE: return 0x8CE7A0;
    default: return 0xFFD166;  // allied
  }
}

// ---------------------------------------------------------------------------
// v0.29.0 REPUTATION BARS + ARROWS (user 2026-10-19; mockup
// https://claude.ai/artifact/HHeRWNatde6jL7AEEvhVcA). No numbers anywhere: the
// Factions bar has five zones and one bright line at your exact standing, and a
// change shows as arrows (one small, two medium, three big) on the screen that
// follows the action. Twins of the phone's repArrows / repLinesHtml / .rbar.
// ---------------------------------------------------------------------------
// Text width in px (glyphs + letter spacing) -- LVGL 9.5 keeps lv_text_get_width private.
static int uiTextWidth(const char *s, const lv_font_t *font, int space) {
  int w = 0;
  for (const char *q = s; *q; q++) w += lv_font_get_glyph_width(font, (uint32_t)(uint8_t)*q, (uint32_t)(uint8_t)q[1]) + space;
  return *s ? w - space : 0;
}

static int repArrowCount(int d) {
  const int a = d < 0 ? -d : d;
  return a == 0 ? 0 : a >= REP_ARROWS_THREE ? 3 : a >= REP_ARROWS_TWO ? 2 : 1;
}

// Arrows filling their object: user_data = the change (signed).
static void repArrowsDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const int d = (int)(intptr_t)lv_event_get_user_data(e), n = repArrowCount(d);
  lv_draw_triangle_dsc_t t;
  lv_draw_triangle_dsc_init(&t);
  t.color = lv_color_hex(d > 0 ? 0x8CE7A0 : UI_RED);
  t.opa = LV_OPA_COVER;
  for (int k = 0; k < n; k++) {
    const int x = a.x1 + k * 13, y1 = a.y1, y2 = a.y2;
    if (d > 0) { t.p[0].x = x + 5; t.p[0].y = y1; t.p[1].x = x + 10; t.p[1].y = y2; t.p[2].x = x; t.p[2].y = y2; }
    else       { t.p[0].x = x;     t.p[0].y = y1; t.p[1].x = x + 10; t.p[1].y = y1; t.p[2].x = x + 5; t.p[2].y = y2; }
    lv_draw_triangle(layer, &t);
  }
}
static int repArrowsWidth(int d) { const int n = repArrowCount(d); return n ? n * 13 - 3 : 0; }
static lv_obj_t *uiRepArrows(lv_obj_t *parent, int x, int y, int d) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(o, repArrowsWidth(d), 10);
  lv_obj_set_pos(o, x, y);
  lv_obj_add_event_cb(o, repArrowsDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)d);
  return o;
}

// Your standing per faction before an action (a battle starting, a purchase).
static int rep_base[NUM_FACTIONS];
static bool rep_base_set = false;
static void repBaseTake(const GameState &gs) {
  if (MY_SLOT < 0) return;
  for (int f = 0; f < NUM_FACTIONS; f++) rep_base[f] = gs.players[MY_SLOT].reputation[f];
  rep_base_set = true;
}

// One centered row on `y`: each faction whose standing moved since rep_base,
// "REBELS ▲▲   EMPIRE ▼". Returns false (and draws nothing) when nothing moved.
static bool uiRepRow(const GameState &gs, int y) {
  if (!rep_base_set || MY_SLOT < 0) return false;
  int fs[NUM_FACTIONS], ds[NUM_FACTIONS], n = 0, total = 0;
  for (int f = 0; f < NUM_FACTIONS; f++) {
    const int d = gs.players[MY_SLOT].reputation[f] - rep_base[f];
    if (!d) continue;
    fs[n] = f; ds[n] = d; n++;
    total += uiTextWidth(FACTION_SHORT[f], &sf_font_montserrat_12, 1) + 6 + repArrowsWidth(d) + 18;
  }
  if (!n) return false;
  int x = 233 - (total - 18) / 2;
  for (int i = 0; i < n; i++) {
    const int f = fs[i];
    lv_obj_t *l = lv_label_create(screen_root);
    lv_label_set_text(l, FACTION_SHORT[f]);
    lv_obj_set_style_text_font(l, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(FACTION_TEXT_COLOR[f]), 0);
    lv_obj_set_pos(l, x, y);
    x += uiTextWidth(FACTION_SHORT[f], &sf_font_montserrat_12, 1) + 6;
    uiRepArrows(screen_root, x, y + 3, ds[i]);
    x += repArrowsWidth(ds[i]) + 18;
  }
  return true;
}

// The Factions bar: five zones, a bright line at your standing that glides from
// where this console last showed it, and a short streak for the change.
static const int REP_ZONE_LO[5] = { REP_MIN, REP_HOSTILE_MAX + 1, REP_WARY_MAX + 1, REP_NEUTRAL_MAX + 1, REP_FAVORABLE_MAX + 1 };
static const int REP_ZONE_HI[5] = { REP_HOSTILE_MAX, REP_WARY_MAX, REP_NEUTRAL_MAX, REP_FAVORABLE_MAX, REP_MAX };
static int rep_seen[NUM_FACTIONS] = { 999, 999, 999, 999 };  // 999 = never shown
static int rep_from[NUM_FACTIONS], rep_to[NUM_FACTIONS];
static uint32_t rep_moved_at[NUM_FACTIONS];
static lv_obj_t *rep_bar_obj[NUM_FACTIONS];
static lv_timer_t *rep_bar_timer = nullptr;
static const uint32_t REP_GLIDE_MS = 900, REP_STREAK_MS = 8000;

static void repBarDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const int f = (int)(intptr_t)lv_event_get_user_data(e);
  const int w = lv_area_get_width(&a), cy = (a.y1 + a.y2) / 2, span = REP_MAX - REP_MIN + 1;
  auto xOf = [&](float v) { return a.x1 + (int)lroundf((v - REP_MIN + 0.5f) / span * w); };
  const int to = rep_to[f], tier = to <= REP_HOSTILE_MAX ? 0 : to <= REP_WARY_MAX ? 1 : to <= REP_NEUTRAL_MAX ? 2 : to <= REP_FAVORABLE_MAX ? 3 : 4;
  for (int z = 0; z < 5; z++) {  // the zones, a 2 px gap between them
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(repTierColor(z));
    d.bg_opa = z == tier ? LV_OPA_70 : LV_OPA_20;
    d.radius = 3;
    lv_area_t r = { (int32_t)(xOf(REP_ZONE_LO[z] - 0.5f) + (z ? 1 : 0)), (int32_t)(cy - 3), (int32_t)(xOf(REP_ZONE_HI[z] + 0.5f) - (z < 4 ? 1 : 0)), (int32_t)(cy + 2) };
    lv_draw_rect(layer, &d, &r);
  }
  const uint32_t since = millis() - rep_moved_at[f];
  float now = (float)to;
  if (rep_moved_at[f] && since < REP_GLIDE_MS) {
    const float p = since / (float)REP_GLIDE_MS, e2 = 1 - (1 - p) * (1 - p) * (1 - p);
    now = rep_from[f] + (to - rep_from[f]) * e2;
  }
  if (rep_moved_at[f] && since < REP_STREAK_MS && rep_from[f] != to) {  // the change, as a streak
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(to > rep_from[f] ? 0x8CE7A0 : UI_RED);
    d.bg_opa = LV_OPA_80;
    d.radius = 3;
    const int x1 = xOf(min((float)rep_from[f], now)), x2 = xOf(max((float)rep_from[f], now));
    lv_area_t r = { (int32_t)x1, (int32_t)(cy - 3), (int32_t)max(x1 + 1, x2), (int32_t)(cy + 2) };
    lv_draw_rect(layer, &d, &r);
  }
  const int mx = xOf(now);  // the line
  lv_draw_rect_dsc_t g;
  lv_draw_rect_dsc_init(&g);
  g.bg_color = lv_color_white();
  g.bg_opa = LV_OPA_COVER;
  g.radius = 1;
  g.shadow_width = 8;
  g.shadow_color = lv_color_white();
  g.shadow_opa = LV_OPA_60;
  lv_area_t m = { (int32_t)(mx - 1), (int32_t)(cy - 8), (int32_t)mx, (int32_t)(cy + 7) };
  lv_draw_rect(layer, &g, &m);
}

static void repBarTick(lv_timer_t *t) {
  bool moving = false;
  for (int f = 0; f < NUM_FACTIONS; f++) {
    if (rep_bar_obj[f] && !lv_obj_is_valid(rep_bar_obj[f])) rep_bar_obj[f] = nullptr;  // its screen is gone
    if (rep_bar_obj[f] && rep_moved_at[f] && millis() - rep_moved_at[f] < REP_GLIDE_MS + 60) { lv_obj_invalidate(rep_bar_obj[f]); moving = true; }
  }
  if (!moving || currentScreen != SCR_REPUTATION) { lv_timer_delete(t); rep_bar_timer = nullptr; }
}

// A bar for faction f inside `row` at (x, y), w wide. Starts the glide if the
// standing moved since this console last showed it.
static void uiRepBar(lv_obj_t *row, int f, int score, int x, int y, int w) {
  if (rep_seen[f] == 999) { rep_seen[f] = score; rep_from[f] = score; rep_to[f] = score; rep_moved_at[f] = 0; }
  if (score != rep_seen[f]) { rep_from[f] = rep_seen[f]; rep_to[f] = score; rep_moved_at[f] = millis(); rep_seen[f] = score; }
  rep_to[f] = score;
  lv_obj_t *o = lv_obj_create(row);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(o, w, 18);
  lv_obj_align(o, LV_ALIGN_TOP_LEFT, x, y);
  lv_obj_add_event_cb(o, repBarDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)f);
  rep_bar_obj[f] = o;
  if (rep_moved_at[f] && millis() - rep_moved_at[f] < REP_GLIDE_MS && !rep_bar_timer) rep_bar_timer = lv_timer_create(repBarTick, 33, nullptr);
}

static void showReputation(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_REPUTATION;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  GameState gs = gameStateRead();
  const int mine = gs.players[MY_SLOT].chosenFaction;
  uiAddRim(gs.roundLimit, gs.round, uiAccentFor(mine));
  uiLabel(screen_root, "FACTIONS", UI_TEXT, nullptr, 42, 4);

  for (int f = 0; f < 4; f++) {
    const int tier = snapshotReputationTier(gs, MY_SLOT, f);
    const uint32_t tc = repTierColor(tier);
    lv_obj_t *row = lv_obj_create(screen_root);
    uiStyleButton(row, f == mine ? uiAccentFor(mine) : UI_LINE, 1, UI_BG, 16);  // style first (Convention 13)
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, 320, 58);
    lv_obj_set_pos(row, 73, 78 + f * 64);  // v0.27.3: a little tighter, room for PROMISES below

    lv_obj_t *ins = uiInsignia(row, f, 28, UI_BG);
    lv_obj_align(ins, LV_ALIGN_LEFT_MID, 14, 0);

    // v0.29.0: name and tier on the top line (your own row is the outlined one),
    // the zone bar with your line below -- it replaced the five pips.
    lv_obj_t *n = lv_label_create(row);
    lv_label_set_text(n, FACTION_VISUALS[f].name);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(n, 1, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(FACTION_TEXT_COLOR[f]), 0);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 54, 9);

    lv_obj_t *t = lv_label_create(row);
    lv_label_set_text(t, REP_TIER_NAMES[tier]);
    lv_obj_set_style_text_font(t, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(t, 1, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(tc), 0);
    lv_obj_align(t, LV_ALIGN_TOP_RIGHT, -14, 11);

    uiRepBar(row, f, gs.players[MY_SLOT].reputation[f], 54, 31, 250);
  }

  // v0.27.3 (playtest: "there is no place to see where your favors are and when
  // they are due"): a pill to your promises, the soonest due on it. 220x36 at (123,334).
  int owed = 0, soonest = 999;
  for (int k = 0; k < MAX_PENDING_OBLIGATIONS; k++) {
    const PendingObligation &ob = gs.players[MY_SLOT].obligations[k];
    if (ob.active) { owed++; soonest = min(soonest, ob.dueRound); }
  }
  const bool history = gs.players[MY_SLOT].promiseLog[0].faction >= 0;  // v0.28.3
  if (owed || history) {
    char pl[40];
    if (!owed) snprintf(pl, sizeof(pl), "PROMISE HISTORY");
    else if (soonest <= gs.round) snprintf(pl, sizeof(pl), "PROMISES: %d - DUE NOW", owed);
    else snprintf(pl, sizeof(pl), "PROMISES: %d - NEXT R%d", owed, soonest);
    uiPill(screen_root, 123, 334, 220, 36, pl, nullptr, soonest <= gs.round ? 0xFFD166 : UI_CYAN, showPromises);
  }
  uiBackButton(screen_root, 233, 412, backToHome);
}

// v0.27.3 YOUR PROMISES: each honor-system promise -- to whom, for which planet,
// when it comes due, and what you agreed to. Up to MAX_PENDING_OBLIGATIONS (3).
static void showPromises(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_REPUTATION;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  const Player &me = gs.players[MY_SLOT];
  uiAddRim(gs.roundLimit, gs.round, uiAccentFor(me.chosenFaction));
  uiLabel(screen_root, "PROMISES", UI_TEXT, nullptr, 42, 4);
  int order[MAX_PENDING_OBLIGATIONS], n = 0;
  for (int k = 0; k < MAX_PENDING_OBLIGATIONS; k++) if (me.obligations[k].active) order[n++] = k;
  for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++)
    if (me.obligations[order[b]].dueRound < me.obligations[order[a]].dueRound) { const int t = order[a]; order[a] = order[b]; order[b] = t; }
  if (!n) uiLabel(screen_root, "You owe no one. For now.", UI_DIM, &lv_font_montserrat_14, 90, 0);
  int y = n ? 80 : 124;
  for (int i = 0; i < n; i++) {
    const PendingObligation &ob = me.obligations[order[i]];
    const int f = ob.factionIdx;
    const bool due = ob.dueRound <= gs.round;
    char head[64];
    snprintf(head, sizeof(head), "%s%s%s", (f >= 0 && f < 4) ? FACTION_SHORT[f] : "",
             (ob.planetIdx >= 0 && ob.planetIdx < NUM_PLANETS) ? "  -  " : "",
             (ob.planetIdx >= 0 && ob.planetIdx < NUM_PLANETS) ? PLANETS[ob.planetIdx].name : "");
    lv_obj_t *h = uiLabel(screen_root, head, (f >= 0 && f < 4) ? FACTION_TEXT_COLOR[f] : UI_TEXT, &lv_font_montserrat_14, y, 2);
    (void)h;
    char when[56];
    const char *who = ob.check ? "GAME CHECKS" : "ON YOUR HONOR";  // v0.28.1
    if (due) snprintf(when, sizeof(when), "DUE THIS ROUND  -  %s", who);
    else snprintf(when, sizeof(when), "DUE ROUND %d  -  %s", ob.dueRound, who);
    uiLabel(screen_root, when, due ? 0xFFD166 : UI_DIM, &sf_font_montserrat_12, y + 20, 2);
    lv_obj_t *tx = lv_label_create(screen_root);
    lv_label_set_text(tx, ob.text);
    lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tx, 300);
    lv_obj_set_style_text_font(tx, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(tx, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(tx, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tx, LV_ALIGN_TOP_MID, 0, y + 38);
    y += 98;
  }
  // v0.28.3: the history, one line each, while there is room above the back button.
  for (int k = 0; k < PROMISE_HISTORY && y < 352; k++) {
    const PromiseRecord &r = me.promiseLog[k];
    if (r.faction < 0 || r.faction >= 4) continue;
    char line[64];
    snprintf(line, sizeof(line), "%s  -  %s%s%s  -  R%d", r.kept ? "KEPT" : "BROKEN", FACTION_SHORT[r.faction],
             (r.planet >= 0 && r.planet < NUM_PLANETS) ? "  -  " : "", (r.planet >= 0 && r.planet < NUM_PLANETS) ? PLANETS[r.planet].name : "", r.round);
    uiLabel(screen_root, line, r.kept ? 0x8CE7A0 : UI_RED, &sf_font_montserrat_12, y, 1);
    y += 22;
  }
  uiBackButton(screen_root, 233, 412, showReputation);
}

// Everyone's row, best VP first: rank, insignia, faction, VP, planets; yours
// outlined in your color. Shared by Standings and Game Over (v0.25.1).
// Rows 320 wide at x 73, not tappable.
static void uiStandingsRows(const GameState &gs, int y, int rowH, int step) {
  const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  // Sort active players by VP, descending. MAX_PLAYERS is tiny (4) -- a
  // simple selection sort is plenty.
  int order[MAX_PLAYERS];
  int count = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (gs.players[i].active) order[count++] = i;
  }
  for (int a = 0; a < count - 1; a++) {
    for (int b = a + 1; b < count; b++) {
      if (gs.players[order[b]].vp > gs.players[order[a]].vp) {
        int tmp = order[a];
        order[a] = order[b];
        order[b] = tmp;
      }
    }
  }

  for (int i = 0; i < count; i++) {
    int slot = order[i];
    bool mine = (slot == MY_SLOT);
    int f = gs.players[slot].chosenFaction;
    uint32_t fColor = uiAccentFor(f);
    uint32_t rowBg = mine ? uiTint(accent) : UI_BG;
    lv_obj_t *row = lv_obj_create(screen_root);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 320, rowH);
    lv_obj_set_pos(row, 73, y);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(rowBg), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, mine ? 2 : 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(mine ? accent : UI_LINE), 0);
    lv_obj_set_style_radius(row, 14, 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", i + 1);
    lv_obj_t *rank = lv_label_create(row);
    lv_label_set_text(rank, buf);
    lv_obj_set_style_text_color(rank, lv_color_hex(mine ? accent : UI_FAINT), 0);
    lv_obj_align(rank, LV_ALIGN_LEFT_MID, 14, 0);

    if (f >= 0) {
      lv_obj_t *ins = uiInsignia(row, f, 28, rowBg);
      lv_obj_align(ins, LV_ALIGN_LEFT_MID, 38, 0);
    }

    lv_obj_t *nameLabel = lv_label_create(row);
    snprintf(buf, sizeof(buf), "%s%s", f >= 0 ? FACTION_SHORT[f] : gs.players[slot].name, mine ? " (YOU)" : "");
    lv_label_set_text(nameLabel, buf);
    lv_obj_set_style_text_font(nameLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(nameLabel, 1, 0);
    lv_obj_set_style_text_color(nameLabel, lv_color_hex(fColor), 0);
    lv_obj_align(nameLabel, LV_ALIGN_LEFT_MID, 78, 0);

    lv_obj_t *vpLabel = lv_label_create(row);
    snprintf(buf, sizeof(buf), "%d", gs.players[slot].vp);
    lv_label_set_text(vpLabel, buf);
    lv_obj_set_style_text_color(vpLabel, lv_color_hex(mine ? accent : UI_TEXT), 0);
    lv_obj_align(vpLabel, LV_ALIGN_RIGHT_MID, -62, 0);

    int planets = 0;
    for (int p = 0; p < NUM_PLANETS; p++) if (gs.planetOwner[p] == slot) planets++;
    lv_obj_t *plLabel = lv_label_create(row);
    snprintf(buf, sizeof(buf), "%d pl.", planets);
    lv_label_set_text(plLabel, buf);
    lv_obj_set_style_text_font(plLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(plLabel, lv_color_hex(UI_DIM), 0);
    lv_obj_align(plLabel, LV_ALIGN_RIGHT_MID, -12, 0);

    y += step;
  }
}

// ---------------------------------------------------------------------------
// STANDINGS -- everyone's VP total, fully public (unlike reputation).
// ---------------------------------------------------------------------------
// 2.0-dev.6 (user: END GAME "is too easy to click, and there should be an
// 'are you sure?' page"): END GAME on Standings opens this page; KEEP PLAYING
// is the big safe button, END GAME the small red one below it.
// Corners: KEEP PLAYING (113..353, 236..312) ~152 px, END GAME (163..303,
// 350..400) ~183 px from center.
static void endConfirmKeepCb(lv_event_t *e) { showStandings(e); }
static void endConfirmGoCb(lv_event_t *e) {
  (void)e;
  gameStateEndGame();  // starTick sees the seat gone and goes back to Start
}
static void showEndConfirm() {
  currentScreen = SCR_END_CONFIRM;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  uiLabel(screen_root, "END THE GAME?", UI_RED, nullptr, 96, 4);
  lv_obj_t *why = uiLabel(screen_root, "This ends it for EVERYONE at the table.\nThe saved game is erased --\nit can't be brought back.",
                          UI_DIM, &lv_font_montserrat_14, 140, 0);
  lv_obj_set_style_text_align(why, LV_TEXT_ALIGN_CENTER, 0);
  uiMarkBack(uiPill(screen_root, 113, 236, 240, 76, "KEEP PLAYING", nullptr, UI_CYAN, endConfirmKeepCb), false);  // edge swipe = keep playing
  uiPill(screen_root, 163, 350, 140, 50, "END GAME", nullptr, UI_RED, endConfirmGoCb);
}
static void standingsEndGameCb(lv_event_t *e) {
  (void)e;
  showEndConfirm();
}

// ---------------------------------------------------------------------------
// 2.0 MARKETS (mockup "B · Markets (cycling)"): the four open this round, the
// same for everyone. Tap a row, then BUY (Prepare only). Rows 330x44 from
// y 84 (farthest corner ~222 px); what the good does (y 282..322, white 14 pt
// -- the pill subline was dim and clipped); BUY pill (113..353, 326..376); BACK (233,414).
// ---------------------------------------------------------------------------
static int market_sel = 0;
// Two planks nailed across the page, the CLOSED sign hanging over them.
static void marketsClosedDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  for (int k = 0; k < 2; k++) {
    const int x1 = k ? 400 : 66, x2 = k ? 66 : 400;
    uiLine(layer, x1, 100, x2, 272, lv_color_hex(0x462C18), 26);
    uiLine(layer, x1, 100, x2, 272, lv_color_hex(0x8B5E34), 20);
    uiDisc(layer, x1 + (k ? -22 : 22), 111, 3, lv_color_hex(0xC8C8D2));
    uiDisc(layer, x2 + (k ? 22 : -22), 261, 3, lv_color_hex(0xC8C8D2));
  }
  drawSprite(layer, CLOSED_SPRITE, 22, 233 - 23 * 6, 116, 6);
}
static void marketRowCb(lv_event_t *e) {
  market_sel = (int)(intptr_t)lv_event_get_user_data(e);
  showMarkets(nullptr);
}
static void marketBuyCb(lv_event_t *e) {
  (void)e;
  int p;
  { GameState gs = gameStateRead(); p = (market_sel >= 0 && market_sel < MARKETS_OPEN) ? gs.marketOpen[market_sel] : -1; }
  if (p >= 0 && gameStateMarketBuy(MY_SLOT, p)) uiToast("BOUGHT", 0xFFD166);
  showMarkets(nullptr);
}
static void showMarkets(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_MARKETS;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  uiAddRim(gs.roundLimit, gs.round, 0xFFD166);
  uiLabel(screen_root, "MARKETS", 0xFFD166, nullptr, 34, 5);
  if (snapshotAshenAlive(gs)) {  // 2.0 (user: a "CLOSED" sprite, "so it looks like someone actually closed it")
    lv_obj_t *boards = lv_obj_create(screen_root);
    lv_obj_remove_style_all(boards);
    lv_obj_clear_flag(boards, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(boards, 466, 466);
    lv_obj_add_event_cb(boards, marketsClosedDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
    uiLabel(screen_root, "THE ASHEN FLEET BLOCKADES THE LANES", 0xFF8C42, &sf_font_montserrat_12, 290, 2);
    lv_obj_t *t = uiLabel(screen_root, "Destroy it and the markets reopen next round.", UI_DIM, &lv_font_montserrat_14, 312, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, 300);
    uiBackButton(screen_root, 233, 414, backToHome);
    return;
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "%d CR - OPEN THIS ROUND", gs.players[MY_SLOT].credits);
  uiLabel(screen_root, buf, UI_DIM, &sf_font_montserrat_12, 62, 2);
  if (market_sel < 0 || market_sel >= MARKETS_OPEN) market_sel = 0;
  for (int i = 0; i < MARKETS_OPEN; i++) {
    const int p = gs.marketOpen[i];
    if (p < 0) continue;
    const bool sel = i == market_sel, stock = gs.marketStock[p] > 0;
    lv_obj_t *row = lv_btn_create(screen_root);
    uiStyleButton(row, sel ? 0xFFD166 : UI_LINE, sel ? 2 : 1, sel ? uiTint(0xFFD166) : UI_BG, 14);  // style first (Convention 13)
    lv_obj_set_size(row, 330, 44);
    lv_obj_set_pos(row, 68, 84 + i * 49);
    lv_obj_add_event_cb(row, marketRowCb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *n = lv_label_create(row);
    snprintf(buf, sizeof(buf), "%s - %s", goodName(gs.planetGood[p]), PLANETS[p].name);
    lv_label_set_text(n, buf);
    lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(stock ? UI_TEXT : UI_FAINT), 0);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 6, -8);
    lv_obj_t *st = lv_label_create(row);
    if (stock) snprintf(buf, sizeof(buf), "%d LEFT", gs.marketStock[p]);
    else snprintf(buf, sizeof(buf), "SOLD OUT");
    lv_label_set_text(st, buf);
    lv_obj_set_style_text_font(st, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_color(st, lv_color_hex(UI_DIM), 0);
    lv_obj_align(st, LV_ALIGN_LEFT_MID, 6, 10);
    const int tier = snapshotReputationTier(gs, MY_SLOT, gs.planetFactionOf[p]);
    const uint32_t pc = tier >= REP_FAVORABLE ? 0x8CE7A0 : tier <= REP_WARY ? 0xFF6B5A : 0xFFD166;
    lv_obj_t *pr = lv_label_create(row);
    snprintf(buf, sizeof(buf), "%d CR", snapshotMarketPrice(gs, MY_SLOT, p));
    lv_label_set_text(pr, buf);
    lv_obj_set_style_text_font(pr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(pr, lv_color_hex(stock ? pc : UI_FAINT), 0);
    lv_obj_align(pr, LV_ALIGN_RIGHT_MID, -6, 0);
  }
  const int sp = gs.marketOpen[market_sel];
  if (sp >= 0) {
    const bool can = snapshotCanBuy(gs, MY_SLOT, sp);
    char t[40];
    if (gs.phase != PHASE_INVEST) snprintf(t, sizeof(t), "BUY IN PREPARE");
    else snprintf(t, sizeof(t), "BUY - %d CR", snapshotMarketPrice(gs, MY_SLOT, sp));
    lv_obj_t *fx = lv_label_create(screen_root);
    lv_label_set_text(fx, goodEffect(gs.planetGood[sp]));
    lv_obj_set_style_text_font(fx, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(fx, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(fx, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(fx, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(fx, 300);
    lv_obj_align(fx, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_t *buy = uiPill(screen_root, 113, 326, 240, 50, t, nullptr, 0xFFD166, can ? marketBuyCb : nullptr);
    if (!can) uiDimColors(buy, LV_OPA_50);
  }
  uiBackButton(screen_root, 233, 414, backToHome);
}

// 2.0: what your quest and strength actually are (user 2026-10-29: "it would be
// good if somewhere i could reread what it actually is i am supposed to do").
static void standingsQuestCb(lv_event_t *e) {
  (void)e;
  char body[200];
  int f, prog, goal = 1;
  { GameState gs = gameStateRead(); f = gs.players[MY_SLOT].chosenFaction; prog = snapshotQuestProgress(gs, MY_SLOT, &goal); }
  snprintf(body, sizeof(body), "%s\nSo far: %d of %d - +%d VP at the end\n\nSTRENGTH - %s\n%s",
           questText(f), prog, goal, QUEST_VP, strengthName(f), strengthText(f));
  v2Card("YOUR QUEST", uiAccentFor(f), questName(f), body, 15000);
}

// ---------------------------------------------------------------------------
// 2.0 THE ASHEN FLEET (user 2026-10-29): the flagship with its HP, then in
// Resolution your ready fleets as toggles and STRIKE. Every ship sent rolls a
// die; it answers with one die (1-2 miss, 3-5 a hit, 6 two hits). Layout:
// sprite (113..353, 66..168), HP bar y 182, text y 204, fleet toggles
// 96x56 at x 77 / 185 / 293, y 262; STRIKE (133..333, 330..386); BACK (233,414).
// ---------------------------------------------------------------------------
static int ashen_mask = -1;  // fleets picked (bit per fleet); -1 = every ready one
static void ashenFleetCb(lv_event_t *e) {
  const int f = (int)(intptr_t)lv_event_get_user_data(e);
  ashen_mask ^= 1 << f;
  showAshen(nullptr);
}
static void ashenStrikeCb(lv_event_t *e) {
  (void)e;
  const int dmg = gameStateAshenStrike(MY_SLOT, ashen_mask);
  ashen_mask = -1;
  char head[24], body[160];
  {
    GameState gs = gameStateRead();
    if (dmg < 0 || gs.ashenLastSlot != MY_SLOT) { uiToast("COULDN'T STRIKE", UI_RED); showAshen(nullptr); return; }
    snprintf(head, sizeof(head), "-%d HP", gs.ashenLastDmg);
    const int r = gs.ashenLastRoll, lost = gs.ashenLastHits;
    if (gs.ashenHp <= 0) snprintf(body, sizeof(body), "%d ships hit it for %d. THE ASHEN FLEET IS DESTROYED.", gs.ashenLastShips, gs.ashenLastDmg);
    else snprintf(body, sizeof(body), "%d ships hit it for %d.\nIt answered with a %d: %s", gs.ashenLastShips, gs.ashenLastDmg, r,
                  lost == 0 ? "a miss." : lost == 1 ? "1 ship lost." : "2 ships lost.");
  }
  showAshen(nullptr);
  v2Card("STRIKE", 0xFF8C42, head, body, 9000);
}
static void showAshen(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_ASHEN;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  if (!snapshotAshenAlive(gs)) { uiLabel(screen_root, "THE ASHEN FLEET IS GONE", 0xFF8C42, &lv_font_montserrat_14, 200, 2); uiBackButton(screen_root, 233, 414, backToHome); return; }
  const Player &me = gs.players[MY_SLOT];
  uiAddRim(gs.roundLimit, gs.round, 0xFF8C42);
  uiLabel(screen_root, "THE ASHEN FLEET", 0xFF8C42, &lv_font_montserrat_14, 40, 3);
  ashenSprite(screen_root, 233, 117, 6);
  lv_obj_t *bar = lv_bar_create(screen_root);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, 260, 10);
  lv_obj_set_pos(bar, 103, 180);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x3A0E0A), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0xFF8C42), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
  lv_bar_set_range(bar, 0, max(1, (int)gs.ashenMaxHp));
  lv_bar_set_value(bar, gs.ashenHp, LV_ANIM_OFF);
  char buf[96];
  snprintf(buf, sizeof(buf), "%d / %d HP  -  YOU: %d DAMAGE", gs.ashenHp, gs.ashenMaxHp, gs.ashenDmg[MY_SLOT]);
  uiLabel(screen_root, buf, UI_TEXT, &sf_font_montserrat_12, 196, 1);
  if (gs.phase != PHASE_RESOLUTION) {
    lv_obj_t *t = uiLabel(screen_root, "Every market is closed while it lives. Strike it in Resolution: every ship you send rolls a die.", UI_DIM, &lv_font_montserrat_14, 228, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, 320);
    snprintf(buf, sizeof(buf), "Sink it: +%d VP each, +%d for the most damage.", ASHEN_VP, ASHEN_TOP_VP);
    lv_obj_t *v = uiLabel(screen_root, buf, 0xFF8C42, &lv_font_montserrat_14, 300, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, 300);
    uiBackButton(screen_root, 233, 414, backToHome);
    return;
  }
  uiLabel(screen_root, "Send fleets: each ship rolls a die", UI_DIM, &lv_font_montserrat_14, 214, 0);
  int ready = 0;
  for (int f = 0; f < NUM_FLEETS; f++) if (fleetAlive(me, f) > 0 && !me.fleetSpent[f]) ready |= 1 << f;
  if (ashen_mask < 0) ashen_mask = ready;
  ashen_mask &= ready;
  int ships = 0;
  for (int f = 0; f < NUM_FLEETS; f++) {
    const bool ok = ready & (1 << f), on = ashen_mask & (1 << f);
    if (on) ships += fleetAlive(me, f);
    const int n = fleetAlive(me, f);
    if (ok) snprintf(buf, sizeof(buf), "FLEET %d\n%d SHIP%s", f + 1, n, n == 1 ? "" : "S");
    else snprintf(buf, sizeof(buf), "FLEET %d\n%s", f + 1, n ? "SPENT" : "EMPTY");
    lv_obj_t *b = uiPill(screen_root, 77 + f * 108, 254, 96, 56, buf, nullptr, on ? 0xFF8C42 : UI_LINE, nullptr);
    if (ok) lv_obj_add_event_cb(b, ashenFleetCb, LV_EVENT_CLICKED, (void *)(intptr_t)f);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    lv_obj_set_style_text_font(l, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (!ok) uiDimColors(b, LV_OPA_40);
  }
  const char *why = snapshotAshenStrikeRefusal(gs, MY_SLOT, ashen_mask);
  if (why) snprintf(buf, sizeof(buf), "%s", ready ? "PICK A FLEET" : "NO FLEET READY");
  else snprintf(buf, sizeof(buf), "STRIKE - %d SHIP%s", ships, ships == 1 ? "" : "S");
  lv_obj_t *go = uiPill(screen_root, 133, 326, 200, 52, buf, nullptr, 0xFF8C42, why ? nullptr : ashenStrikeCb);
  lv_obj_set_style_text_letter_space(lv_obj_get_child(go, 0), 1, 0);
  if (why) uiDimColors(go, LV_OPA_40);
  uiSmallBack(backToHome);
}

// ---------------------------------------------------------------------------
// 2.0 DEALS (mockup "C . Deals"): offers waiting on you, your offers out and
// your promises (rows 330x50 from y 84, up to four), then one offer (ACCEPT /
// COUNTER / DECLINE) and the offer form: five rows of < > / - + (no keyboard
// on the 1.75" -- it picks a canned line; phones can type). Rules in
// game_state.cpp (gameStateDealSend / gameStateDealAnswer).
// ---------------------------------------------------------------------------
static int deal_view = -1;
struct DealForm { int deal, to, give, ask, world, askCr, lineK; };
static DealForm deal_form = { -1, -1, 2, DEAL_NO_ATTACK, -1, 3, 0 };
static void showDealView();
static void showDealCompose();

static const char *dealWho(const GameState &gs, int slot) {
  const int f = (slot >= 0 && slot < MAX_PLAYERS) ? gs.players[slot].chosenFaction : -1;
  return (f >= 0 && f < 4) ? FACTION_SHORT[f] : "SOMEONE";
}
static uint32_t dealColor(const GameState &gs, int slot) {
  const int f = (slot >= 0 && slot < MAX_PLAYERS) ? gs.players[slot].chosenFaction : -1;
  return (f >= 0 && f < 4) ? FACTION_TEXT_COLOR[f] : UI_DIM;
}
static void dealRowCb(lv_event_t *e) { deal_view = (int)(intptr_t)lv_event_get_user_data(e); showDealView(); }
// Each callback lets go of its GameState copy before the next screen reads its
// own -- two ~4.7 KB copies on the 16 KB LVGL stack is how 1.0 crashed.
static void dealNewCb(lv_event_t *e) {
  (void)e;
  deal_form = { -1, -1, 2, DEAL_NO_ATTACK, -1, 3, 0 };
  {
    GameState gs = gameStateRead();
    for (int i = 0; i < MAX_PLAYERS; i++)
      if (i != MY_SLOT && gs.players[i].active && gs.players[i].chosenFaction >= 0) { deal_form.to = i; break; }
  }
  showDealCompose();
}

static void showDeals(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_DEALS;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  uiAddRim(gs.roundLimit, gs.round, accent);
  uiLabel(screen_root, "DEALS", accent, nullptr, 34, 5);
  const int waiting = snapshotDealsWaiting(gs, MY_SLOT);
  char buf[96];
  if (waiting) snprintf(buf, sizeof(buf), "%d WAITING ON YOU", waiting);
  else snprintf(buf, sizeof(buf), "CREDITS FOR A PROMISE");
  uiLabel(screen_root, buf, waiting ? UI_RED : UI_DIM, &sf_font_montserrat_12, 62, 2);
  int row = 0;
  for (int k = 0; k < DEAL_SLOTS && row < 4; k++) {
    const Deal &d = gs.deals[k];
    if (d.state == DEAL_FREE || (d.payer != MY_SLOT && d.promiser != MY_SLOT)) continue;
    const int other = d.payer == MY_SLOT ? d.promiser : d.payer;
    const bool mine = d.state == DEAL_OFFERED && d.turn == MY_SLOT;
    const uint32_t col = mine ? dealColor(gs, other) : d.state == DEAL_ACTIVE ? 0x8CE7A0 : UI_LINE;
    lv_obj_t *r = lv_btn_create(screen_root);
    uiStyleButton(r, col, mine ? 2 : 1, mine ? uiTint(col) : UI_BG, 14);  // style first (Convention 13)
    lv_obj_set_size(r, 330, 50);
    lv_obj_set_pos(r, 68, 84 + row * 58);
    lv_obj_add_event_cb(r, dealRowCb, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    lv_obj_t *h = lv_label_create(r);
    if (d.state == DEAL_ACTIVE) snprintf(buf, sizeof(buf), "DEAL WITH %s", dealWho(gs, other));
    else if (mine) snprintf(buf, sizeof(buf), "%s FROM %s", d.counters ? "COUNTER" : "OFFER", dealWho(gs, other));
    else snprintf(buf, sizeof(buf), "WAITING ON %s", dealWho(gs, other));
    lv_label_set_text(h, buf);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(h, lv_color_hex(mine ? dealColor(gs, other) : UI_TEXT), 0);
    lv_obj_align(h, LV_ALIGN_LEFT_MID, 8, -9);
    lv_obj_t *t = lv_label_create(r);
    snapshotDealAskText(gs, k, MY_SLOT, buf, sizeof(buf));
    lv_label_set_text(t, buf);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, 312);
    lv_obj_set_style_text_font(t, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(UI_DIM), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 8, 11);
    row++;
  }
  if (!row) uiLabel(screen_root, "No offers or promises yet.", UI_DIM, &lv_font_montserrat_14, 180, 0);
  bool others = false;
  for (int i = 0; i < MAX_PLAYERS; i++) if (i != MY_SLOT && gs.players[i].active && gs.players[i].chosenFaction >= 0) others = true;
  if (gs.phase == PHASE_INVEST && others) uiPill(screen_root, 113, 326, 240, 50, "NEW OFFER", nullptr, accent, dealNewCb);
  else uiLabel(screen_root, gs.phase == PHASE_INVEST ? "Nobody to deal with." : "New offers are made in Prepare.", UI_FAINT, &sf_font_montserrat_12, 338, 1);
  uiBackButton(screen_root, 233, 414, backToHome);
}

// One offer. Buttons (69..397, 300..356): ACCEPT / COUNTER / DECLINE when it's
// your turn, WITHDRAW when you're waiting; BACK (233,414).
static void dealAnswerCb(lv_event_t *e) {
  const bool yes = (intptr_t)lv_event_get_user_data(e) != 0;
  if (gameStateDealAnswer(MY_SLOT, deal_view, yes)) uiToast(yes ? "DEAL AGREED" : "DONE", yes ? 0x8CE7A0 : UI_DIM);
  else uiToast("COULDN'T DO THAT", UI_RED);
  showDeals(nullptr);
}
static void dealCounterCb(lv_event_t *e) {
  (void)e;
  if (deal_view < 0 || deal_view >= DEAL_SLOTS) return;
  {
    GameState gs = gameStateRead();
    const Deal &d = gs.deals[deal_view];
    deal_form = { deal_view, d.payer == MY_SLOT ? d.promiser : d.payer, d.give, d.ask, d.world, d.askCr ? d.askCr : 3, 3 };  // "Too low."
  }
  showDealCompose();
}
static bool dealStillThere(int k, bool offeredOnly) {
  if (k < 0 || k >= DEAL_SLOTS) return false;
  GameState gs = gameStateRead();
  return offeredOnly ? gs.deals[k].state == DEAL_OFFERED : gs.deals[k].state != DEAL_FREE;
}
static void showDealView() {
  if (!dealStillThere(deal_view, false)) { showDeals(nullptr); return; }  // checked before this screen's own copy
  currentScreen = SCR_DEAL_VIEW;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  const Deal &d = gs.deals[deal_view];
  const int other = d.payer == MY_SLOT ? d.promiser : d.payer;
  const bool myTurn = d.state == DEAL_OFFERED && d.turn == MY_SLOT;
  const uint32_t col = dealColor(gs, other);
  uiAddRim(gs.roundLimit, gs.round, col);
  char buf[112];
  if (d.state == DEAL_ACTIVE) snprintf(buf, sizeof(buf), "DEAL WITH");
  else if (myTurn) snprintf(buf, sizeof(buf), "%s FROM", d.counters ? "COUNTER" : "OFFER");
  else snprintf(buf, sizeof(buf), "WAITING ON");
  uiLabel(screen_root, buf, UI_DIM, &sf_font_montserrat_12, 58, 3);
  uiLabel(screen_root, dealWho(gs, other), col, nullptr, 76, 4);
  snprintf(buf, sizeof(buf), "%s %d CR", d.payer == MY_SLOT ? "You give" : "They give", d.give);
  uiLabel(screen_root, buf, UI_TEXT, nullptr, 122, 1);
  snapshotDealAskText(gs, deal_view, MY_SLOT, buf, sizeof(buf));
  lv_obj_t *a = uiLabel(screen_root, buf, UI_TEXT, &lv_font_montserrat_14, 160, 0);
  lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(a, 330);
  const char *say = d.line >= 0 ? dealLine(d.line) : d.msg;
  if (say && say[0]) {
    snprintf(buf, sizeof(buf), "\"%s\"", say);
    lv_obj_t *q = uiLabel(screen_root, buf, UI_DIM, &lv_font_montserrat_14, 214, 0);
    lv_label_set_long_mode(q, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(q, 320);
  }
  if (myTurn) {
    lv_obj_t *y = uiPill(screen_root, 69, 300, 104, 56, "ACCEPT", nullptr, 0x8CE7A0, nullptr);
    lv_obj_add_event_cb(y, dealAnswerCb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *c = uiPill(screen_root, 181, 300, 104, 56, "COUNTER", nullptr, UI_CYAN, d.counters < DEAL_MAX_COUNTERS ? dealCounterCb : nullptr);
    if (d.counters >= DEAL_MAX_COUNTERS) uiDimColors(c, LV_OPA_40);
    lv_obj_t *n = uiPill(screen_root, 293, 300, 104, 56, "DECLINE", nullptr, UI_RED, nullptr);
    lv_obj_add_event_cb(n, dealAnswerCb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    lv_obj_t *trio[3] = { y, c, n };
    for (lv_obj_t *o : trio) lv_obj_set_style_text_letter_space(lv_obj_get_child(o, 0), 0, 0);  // 104 px pills
    snprintf(buf, sizeof(buf), "%d COUNTER%s LEFT - LAPSES WHEN PREPARE ENDS", DEAL_MAX_COUNTERS - d.counters, DEAL_MAX_COUNTERS - d.counters == 1 ? "" : "S");
    uiLabel(screen_root, buf, UI_FAINT, &sf_font_montserrat_12, 362, 1);
  } else if (d.state == DEAL_OFFERED) {
    lv_obj_t *w = uiPill(screen_root, 143, 300, 180, 56, "WITHDRAW", nullptr, UI_DIM, nullptr);
    lv_obj_add_event_cb(w, dealAnswerCb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
  } else {
    uiLabel(screen_root, "The game holds you both to it.", 0x8CE7A0, &lv_font_montserrat_14, 318, 0);
  }
  uiBackButton(screen_root, 233, 414, showDeals);
}

// The offer form. Rows 330x44 at y 84 + 50i (TO, GIVE, ASK, its detail, LINE);
// the refusal reason at y 336; BACK (165,400) left, SEND (213..353, 372..424).
enum { DF_TO = 0, DF_GIVE, DF_ASK, DF_WORLD, DF_ASKCR, DF_LINE };
static bool dealWorldOk(const GameState &gs, int ask, int promiser, int q) {
  return ask == DEAL_STAY_AWAY ? gs.planetOwner[q] != promiser : ask == DEAL_MARKET_CUT ? gs.planetOwner[q] == promiser : false;
}
static void dealFormStepCb(lv_event_t *e) {
  const int ud = (int)(intptr_t)lv_event_get_user_data(e), field = ud >> 1, dir = (ud & 1) ? 1 : -1;
  {
    GameState gs = gameStateRead();
    DealForm &f = deal_form;
    const int payer = f.deal >= 0 ? gs.deals[f.deal].payer : MY_SLOT;
    const int promiser = f.deal >= 0 ? gs.deals[f.deal].promiser : f.to;
    switch (field) {
      case DF_TO:
        for (int n = 1; n <= MAX_PLAYERS; n++) {
          const int i = ((f.to < 0 ? 0 : f.to) + dir * n + MAX_PLAYERS * 2) % MAX_PLAYERS;
          if (i != MY_SLOT && gs.players[i].active && gs.players[i].chosenFaction >= 0) { f.to = i; break; }
        }
        f.world = -1;
        break;
      case DF_GIVE: {
        const int hi = payer == MY_SLOT ? min(DEAL_MAX_GIVE, gs.players[MY_SLOT].credits) : DEAL_MAX_GIVE;
        f.give = constrain(f.give + dir, 0, max(0, hi));
        break;
      }
      case DF_ASK: f.ask = (f.ask + dir + DEAL_ASK_COUNT) % DEAL_ASK_COUNT; f.world = -1; break;
      case DF_WORLD:
        for (int n = 1; n <= NUM_PLANETS; n++) {
          const int q = ((f.world < 0 ? (dir > 0 ? -1 : 0) : f.world) + dir * n + NUM_PLANETS * 2) % NUM_PLANETS;
          if (dealWorldOk(gs, f.ask, promiser, q)) { f.world = q; break; }
        }
        break;
      case DF_ASKCR: f.askCr = constrain(f.askCr + dir, 1, DEAL_MAX_ASK_CR); break;
      case DF_LINE: f.lineK = (f.lineK + dir + DEAL_PICK_LINES) % DEAL_PICK_LINES; break;
    }
  }
  showDealCompose();
}
static void dealSendCb(lv_event_t *e) {
  (void)e;
  const DealForm &f = deal_form;
  int faction;
  { GameState gs = gameStateRead(); faction = gs.players[MY_SLOT].chosenFaction; }
  if (gameStateDealSend(MY_SLOT, f.deal, f.to, f.give, f.ask, f.world, f.askCr, dealLineFor(faction, f.lineK), nullptr) >= 0) {
    uiToast(f.deal >= 0 ? "COUNTER SENT" : "OFFER SENT", 0x8CE7A0);
    showDeals(nullptr);
  } else {
    uiToast("COULDN'T SEND IT", UI_RED);
    showDealCompose();
  }
}
static void dealFormRow(int y, const char *key, const char *value, int field, bool arrows, bool plusMinus, uint32_t valueColor) {
  lv_obj_t *r = lv_obj_create(screen_root);
  uiStyleButton(r, UI_LINE, 1, UI_BG, 14);  // style first (Convention 13)
  lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(r, 330, 44);
  lv_obj_set_pos(r, 68, y);
  lv_obj_t *k = lv_label_create(r);
  lv_label_set_text(k, key);
  lv_obj_set_style_text_font(k, &sf_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(k, 2, 0);
  lv_obj_set_style_text_color(k, lv_color_hex(UI_DIM), 0);
  lv_obj_align(k, LV_ALIGN_TOP_MID, 0, 3);
  lv_obj_t *v = lv_label_create(r);
  lv_label_set_text(v, value);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  lv_obj_set_width(v, 220);
  lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(v, lv_color_hex(valueColor), 0);
  lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -4);
  if (!arrows) return;
  for (int side = 0; side < 2; side++) {
    lv_obj_t *b = lv_btn_create(r);
    uiStyleButton(b, UI_LINE, 1, UI_PANEL, 12);
    lv_obj_set_size(b, 50, 40);
    lv_obj_align(b, side ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, side ? -2 : 2, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, plusMinus ? (side ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS) : (side ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT));
    lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, dealFormStepCb, LV_EVENT_CLICKED, (void *)(intptr_t)((field << 1) | side));
  }
}
static void showDealCompose() {
  if (deal_form.deal >= 0 && !dealStillThere(deal_form.deal, true)) { showDeals(nullptr); return; }
  currentScreen = SCR_DEAL_COMPOSE;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  DealForm &f = deal_form;
  const int payer = f.deal >= 0 ? gs.deals[f.deal].payer : MY_SLOT;
  const int promiser = f.deal >= 0 ? gs.deals[f.deal].promiser : f.to;
  if ((f.ask == DEAL_STAY_AWAY || f.ask == DEAL_MARKET_CUT) && (f.world < 0 || !dealWorldOk(gs, f.ask, promiser, f.world))) {
    f.world = -1;
    for (int q = 0; q < NUM_PLANETS && f.world < 0; q++) if (dealWorldOk(gs, f.ask, promiser, q)) f.world = q;
  }
  const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  uiAddRim(gs.roundLimit, gs.round, accent);
  uiLabel(screen_root, f.deal >= 0 ? "COUNTER" : "NEW OFFER", accent, nullptr, 46, 4);
  char v[64];
  dealFormRow(84, "TO", dealWho(gs, f.to), DF_TO, f.deal < 0, false, dealColor(gs, f.to));
  snprintf(v, sizeof(v), "%d CR", f.give);
  dealFormRow(134, payer == MY_SLOT ? "YOU GIVE" : "THEY GIVE", v, DF_GIVE, true, true, UI_TEXT);
  dealFormRow(184, payer == MY_SLOT ? "YOU ASK" : "THEY ASK", dealAskLabel(f.ask), DF_ASK, true, false, UI_TEXT);
  if (f.ask == DEAL_STAY_AWAY || f.ask == DEAL_MARKET_CUT)
    dealFormRow(234, "WHICH WORLD", f.world >= 0 ? PLANETS[f.world].name : "NONE", DF_WORLD, f.world >= 0, false, UI_TEXT);
  else if (f.ask == DEAL_PAY_LATER) {
    snprintf(v, sizeof(v), "%d CR NEXT ROUND", f.askCr);
    dealFormRow(234, "HOW MUCH", v, DF_ASKCR, true, true, UI_TEXT);
  } else {
    snprintf(v, sizeof(v), "FOR %d ROUNDS", DEAL_ROUNDS);
    dealFormRow(234, "HOW LONG", v, DF_ASK, false, false, UI_DIM);
  }
  const int line = dealLineFor(gs.players[MY_SLOT].chosenFaction, f.lineK);
  snprintf(v, sizeof(v), "\"%s\"", dealLine(line));
  dealFormRow(284, "SAY", v, DF_LINE, true, false, UI_DIM);
  const char *why = snapshotDealSendRefusal(gs, MY_SLOT, f.deal, f.to, f.give, f.ask, f.world, f.askCr);
  if (why) uiLabel(screen_root, why, UI_RED, &sf_font_montserrat_12, 336, 0);
  uiBackButton(screen_root, 165, 400, f.deal >= 0 ? (lv_event_cb_t)[](lv_event_t *e) { (void)e; showDealView(); } : showDeals);
  lv_obj_t *send = uiPill(screen_root, 143, 366, 180, 56, "SEND", nullptr, accent, why ? nullptr : dealSendCb);  // 2.0-dev.9: centered; corners ~209 px
  if (why) uiDimColors(send, LV_OPA_40);
}

static void showStandings(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_STANDINGS;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  GameState gs = gameStateRead();
  const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  char sub[32];
  snprintf(sub, sizeof(sub), "ROUND %d / %d", gs.round, gs.roundLimit);
  uiLabel(screen_root, sub, UI_DIM, &lv_font_montserrat_14, 40, 3);
  uiLabel(screen_root, "STANDINGS", UI_TEXT, nullptr, 58, 4);

  uiStandingsRows(gs, 100, 56, 62);

  {  // 2.0: your quest and how close you are
    const int f = gs.players[MY_SLOT].chosenFaction;
    int goal = 1;
    const int prog = snapshotQuestProgress(gs, MY_SLOT, &goal);
    char q[64];
    snprintf(q, sizeof(q), "QUEST: %s  %d / %d  - TAP", questName(f), prog, goal);
    lv_obj_t *ql = uiLabel(screen_root, q, accent, &sf_font_montserrat_12, 346, 1);
    lv_obj_add_flag(ql, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ql, 12);
    lv_obj_add_event_cb(ql, standingsQuestCb, LV_EVENT_CLICKED, nullptr);
  }
  // v0.30.3: END GAME -- the table resumes a saved game when it boots, so this is
  // how a game is abandoned. 2.0-dev.6: it opens an "are you sure?" page.
  // BACK moves left, END GAME right; farthest visible corners ~216 / ~222 px.
  uiBackButton(screen_root, 165, 400, backToHome);
  uiPill(screen_root, 163, 372, 140, 52, "END GAME", nullptr, UI_RED, standingsEndGameCb);  // 2.0-dev.9: centered; corners ~203 px
}

// ---------------------------------------------------------------------------
// GAME OVER -- winner announcement + final standings. Auto-triggered on
// every device once stage flips to STAGE_GAME_OVER (see starTick).
// ---------------------------------------------------------------------------
static uint32_t game_over_new_armed = 0;
static void gameOverNewGameCb(lv_event_t *e) {
  (void)e;
  if (game_over_new_armed && millis() - game_over_new_armed < 4000) {
    game_over_new_armed = 0;
    gameStateNewGame();  // starTick sees the Lobby and goes back to Start
  } else {
    game_over_new_armed = millis();
    showGameOver();
  }
}

static void showGameOver() {
  currentScreen = SCR_GAME_OVER;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  // v0.25.1 restyle: the winner's face in their color, YOUR result big
  // (VICTORY in gold if it's you), then the final standings rows. Replaces
  // the last transform_scale label (Convention 7).
  GameState gs = gameStateRead();
  const int winner = gameStateWinnerSlot();
  const bool iWon = winner >= 0 && winner == MY_SLOT;
  const int wf = winner >= 0 ? gs.players[winner].chosenFaction : -1;
  uiAddRim(gs.roundLimit, gs.roundLimit, iWon ? 0xFFC933 : (wf >= 0 ? FACTION_RING_COLOR[wf] : UI_DIM));

  char buf[48];
  if (MY_SLOT >= 0 && gs.players[MY_SLOT].questDone) snprintf(buf, sizeof(buf), "QUEST COMPLETE  +%d VP", QUEST_VP);  // 2.0
  else snprintf(buf, sizeof(buf), "GAME OVER  -  %d ROUNDS", gs.roundLimit);
  uiLabel(screen_root, buf, (MY_SLOT >= 0 && gs.players[MY_SLOT].questDone) ? 0x8CE7A0 : UI_DIM, &sf_font_montserrat_12, 34, 3);

  if (winner >= 0 && wf >= 0) {
    lv_obj_t *disc = lv_obj_create(screen_root);
    uiStyleButton(disc, FACTION_RING_COLOR[wf], 3, uiTint(FACTION_RING_COLOR[wf]), LV_RADIUS_CIRCLE);  // style first (Convention 13)
    lv_obj_set_style_clip_corner(disc, true, 0);
    lv_obj_clear_flag(disc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(disc, 84, 84);
    lv_obj_set_pos(disc, 233 - 42, 54);
    if (gs.players[winner].commander >= 0) {
      lv_obj_t *im = uiPortrait(disc, gs.players[winner].commander, CMD_IMG_BIG);
      lv_obj_align(im, LV_ALIGN_BOTTOM_MID, 0, 2);
    } else {
      lv_obj_center(uiInsignia(disc, wf, 44, uiTint(FACTION_RING_COLOR[wf])));
    }
    if (iWon) uiLabel(screen_root, "VICTORY", 0xFFC933, &sf_font_montserrat_36, 142, 4);
    else {
      snprintf(buf, sizeof(buf), "%s %s", FACTION_SHORT[wf], (wf == 1 || wf == 3) ? "WINS" : "WIN");  // the Empire wins, the Rebels win
      uiLabel(screen_root, buf, FACTION_TEXT_COLOR[wf], nullptr, 150, 3);
    }
  } else {
    uiLabel(screen_root, "IT'S A DRAW", 0xFFC933, &sf_font_montserrat_36, 110, 3);
  }

  uiStandingsRows(gs, 186, 46, 50);  // last row ends y 382: corners ~218px from center
  // v0.27.3: NEW GAME -- a fresh Lobby for everyone; two taps (the first arms
  // it for 4 s) so a stray touch can't wipe the results. Corner ~212 px.
  const bool armed = game_over_new_armed && millis() - game_over_new_armed < 4000;
  lv_obj_t *ng = uiPill(screen_root, 153, 390, 160, 40, armed ? "TAP AGAIN" : "NEW GAME", nullptr,
                        armed ? UI_RED : UI_CYAN, gameOverNewGameCb);
  (void)ng;
}

// ---------------------------------------------------------------------------
// PLANETS LIST
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// GALAXY (v0.21.2 ring; v0.21.14 = a map of the physical board). Every
// planet sits at its board slot (GALAXY_POS below). Each is a round
// circle holding its pixel-planet art (the battle scene's renderer, baked
// once into a small image per planet) with a name tag across its lower
// edge. The circle's border is the holder's faction color (hairline if
// nobody holds it); yours also gets a tinted fill. The center carries the
// title and Credits (top-left) and the back button (bottom-left). Name tags
// (Montserrat 12, widest CORUSCANT ~79px) overhang the circle a little, so
// they are screen children rather than button children.
// ---------------------------------------------------------------------------
static const lv_image_dsc_t *ringPlanetImage(int planetIdx, int size);  // defined with the battle scene's planet renderer

// v0.21.14: the Galaxy screen IS the physical board (layout A2, see the design notes
// "Board design" and tools/board/): every planet sits at its board slot --
// six Outer Rim worlds on the outer ring, four Mid Rim on the middle ring,
// Coruscant and Corellia in the Core -- with the ring routes, hyperspace
// lanes, the two nebulae and the four faction hangars drawn underneath. So
// the screen shows exactly how the table is laid out (planets left out of
// the game show as empty slots). Positions are px from the screen center,
// +y down, indexed like PLANETS[] in game_state.cpp; the phone's
// GALAXY_POS in web_server.cpp must match.
static const int16_t GALAXY_POS[NUM_PLANETS][2] = {
  {  156,   90 },  // TATOOINE  outer, SE (Rebels)
  {    0, -180 },  // HOTH      outer, N  (shared Empire / Crimson Dawn)
  {    0,  108 },  // BESPIN    mid, S
  {    0,  180 },  // DAGOBAH   outer, S  (shared Rebels / Pykes)
  {    0,  -36 },  // CORUSCANT core
  { -156,  -90 },  // KESSEL    outer, NW (Crimson Dawn)
  { -156,   90 },  // OBA DIAH  outer, SW (Pykes)
  { -108,    0 },  // DATHOMIR  mid, W
  {    0,   36 },  // CORELLIA  core
  {  108,    0 },  // NABOO     mid, E
  {  156,  -90 },  // ENDOR     outer, NE (Empire)
  {    0, -108 },  // KIJIMI    mid, N
};
static const int GALAXY_D = 68, GALAXY_ART = 52;  // circle / baked art size
// Ring routes (radius) and hyperspace lanes (planet index pairs).
static const int GALAXY_RINGS[3] = { 180, 108, 36 };
static const int8_t GALAXY_LANES[][2] = {
  { 1, 11 }, { 11, 4 }, { 3, 2 }, { 2, 8 }, { 10, 9 }, { 0, 9 }, { 6, 7 }, { 5, 7 }, { 4, 8 },
};
// Faction hangars on the diagonals (faction, x, y) and their two lanes.
static const int16_t GALAXY_HANGARS[4][3] = { { 1, 141, -141 }, { 0, 141, 141 }, { 2, -141, 141 }, { 3, -141, -141 } };
static const int8_t GALAXY_HANGAR_LANES[4][2] = { { 1, 10 }, { 0, 3 }, { 3, 6 }, { 5, 1 } };
// Touch (Convention 5): outer circles' edge 180 + 34 = 214px from center;
// nearest neighbors 72px apart (4px gaps); back button 60px at (-88, 88).

// 2.0 (user: "fit it in the middle of the galaxy map so it matches the board"):
// while the Ashen Fleet lives, Coruscant and Corellia step out to +-46 at 54 px
// so the flagship fits between them (band +-19; Kijimi / Bespin edges stay clear).
static bool galaxy_ashen = false;
static int galaxy_ashen_hp = 0, galaxy_ashen_max = 1;
static const int GALAXY_ASHEN_CORE = 46, GALAXY_ASHEN_D = 54, GALAXY_ASHEN_SNAP = 30;
static const int GALAXY_ASHEN = NUM_PLANETS + 1;  // touch target id
static bool galaxyIsCore(int p) { return p == 4 || p == 8; }  // CORUSCANT, CORELLIA
static int galaxyX(int p) { return GALAXY_POS[p][0]; }
static int galaxyY(int p) {
  if (galaxy_ashen && galaxyIsCore(p)) return GALAXY_POS[p][1] < 0 ? -GALAXY_ASHEN_CORE : GALAXY_ASHEN_CORE;
  return GALAXY_POS[p][1];
}
static lv_obj_t *galaxy_ashen_obj = nullptr;
static void galaxyAshenDrawCb(lv_event_t *e) {  // the flagship + its HP bar
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  drawSprite(layer, ASHEN_SPRITE, 17, a.x1, a.y1, 2);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_opa = LV_OPA_COVER;
  d.bg_color = lv_color_hex(0x3A0E0A);
  lv_area_t bar = { a.x1, (lv_coord_t)(a.y1 + 35), a.x2, (lv_coord_t)(a.y1 + 37) };
  lv_draw_rect(layer, &d, &bar);
  d.bg_color = lv_color_hex(0xFF8C42);
  bar.x2 = (lv_coord_t)(a.x1 + (a.x2 - a.x1) * galaxy_ashen_hp / max(1, galaxy_ashen_max));
  lv_draw_rect(layer, &d, &bar);
}

static lv_obj_t *ring_btn[NUM_PLANETS];      // per planet index, nullptr if not on the map
static lv_obj_t *ring_tag[NUM_PLANETS];
static int8_t ring_owner_shown[NUM_PLANETS];  // planetOwner each circle was last styled for

// Builds the ordered list of planet indices actually included in this game.
// Returns the count; fills outList (must have room for NUM_PLANETS).
static int buildIncludedPlanetList(const GameState &gs, int *outList) {
  int count = 0;
  for (int i = 0; i < NUM_PLANETS; i++) {
    if (gs.planetIncluded[i]) outList[count++] = i;
  }
  return count;
}

// Border/fill/tag colors for one ring circle from who holds the planet.
static void styleRingPlanet(const GameState &gs, int p) {
  lv_obj_t *btn = ring_btn[p];
  if (!btn) return;
  int owner = gs.planetOwner[p];
  int f = (owner >= 0 && owner < MAX_PLAYERS) ? gs.players[owner].chosenFaction : -1;
  bool mine = owner >= 0 && owner == MY_SLOT;
  uint32_t border = (f >= 0 && f < 4) ? FACTION_RING_COLOR[f] : UI_LINE;
  uint32_t text = (f >= 0 && f < 4) ? FACTION_TEXT_COLOR[f] : UI_TEXT;
  lv_obj_set_style_border_color(btn, lv_color_hex(border), 0);
  // v0.28.3 (playtest: owners "really hard to tell", the Empire's worst): owned
  // worlds get a thick ring in the owner's color and a tinted fill.
  lv_obj_set_style_border_width(btn, f >= 0 ? (mine ? 6 : 5) : 2, 0);
  lv_obj_set_style_bg_color(btn, lv_color_hex(f >= 0 ? uiTint(border) : UI_BG), 0);
  if (ring_tag[p]) lv_obj_set_style_text_color(ring_tag[p], lv_color_hex(text), 0);
  ring_owner_shown[p] = (int8_t)owner;
}

// Called from starTick while the ring is up: restyle circles whose holder
// changed (someone's takeover landing while you browse).
static void updatePlanetRing(const GameState &gs) {
  if (currentScreen != SCR_PLANETS) return;
  for (int p = 0; p < NUM_PLANETS; p++) {
    if (ring_btn[p] && ring_owner_shown[p] != gs.planetOwner[p]) styleRingPlanet(gs, p);
  }
}


// The board underneath the planet circles: ring routes, lanes, hangars (a
// faction-colored diamond with its two lanes), and the two nebulae. One
// DRAW_MAIN object -- a few dozen primitives, cheap enough for the 180ms
// star-field redraw.
static void galaxyMapPaint(lv_layer_t *layer);
// 2.0 frame rate: the map never changes while you look at it, but its arcs,
// slanted lanes and see-through glows cost ~100 ms a full redraw on the 1.75".
// galaxyBakeMap() paints it ONCE (per Ashen-Fleet layout) into a full-screen
// RGB565 picture in PSRAM, on the screen's background color; the map object
// then just copies that picture. Called when the Galaxy screen is built --
// never from inside a draw.
static uint8_t *galaxy_bg_buf = nullptr;
static lv_obj_t *galaxy_bg_canvas = nullptr;
static int galaxy_bg_for = -1;  // galaxy_ashen it was painted for, -1 = none
static lv_image_dsc_t galaxy_bg_img;
static void galaxyBakeMap() {
  if (galaxy_bg_for == (int)galaxy_ashen) return;
  const uint32_t stride = lv_draw_buf_width_to_stride(466, LV_COLOR_FORMAT_RGB565);
  if (!galaxy_bg_buf) {
    galaxy_bg_buf = (uint8_t *)heap_caps_malloc((size_t)stride * 466, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!galaxy_bg_buf) return;  // draws live instead
    galaxy_bg_canvas = lv_canvas_create(lv_layer_sys());  // survives clearScreen(); never shown
    lv_obj_add_flag(galaxy_bg_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_buffer(galaxy_bg_canvas, galaxy_bg_buf, 466, 466, LV_COLOR_FORMAT_RGB565);
  }
  lv_canvas_fill_bg(galaxy_bg_canvas, lv_color_hex(0x02060D), LV_OPA_COVER);  // = setScreenBackground
  lv_layer_t layer;
  lv_canvas_init_layer(galaxy_bg_canvas, &layer);
  galaxyMapPaint(&layer);
  lv_canvas_finish_layer(galaxy_bg_canvas, &layer);
  memset(&galaxy_bg_img, 0, sizeof(galaxy_bg_img));
  galaxy_bg_img.header.magic = LV_IMAGE_HEADER_MAGIC;
  galaxy_bg_img.header.cf = LV_COLOR_FORMAT_RGB565;
  galaxy_bg_img.header.w = 466;
  galaxy_bg_img.header.h = 466;
  galaxy_bg_img.header.stride = stride;
  galaxy_bg_img.data_size = stride * 466;
  galaxy_bg_img.data = galaxy_bg_buf;
  galaxy_bg_for = (int)galaxy_ashen;
}

static void galaxyMapDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  if (galaxy_bg_for == (int)galaxy_ashen) {
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &galaxy_bg_img;
    const lv_area_t at = { 0, 0, 465, 465 };
    lv_draw_image(layer, &d, &at);
    return;
  }
  galaxyMapPaint(layer);
}

static void galaxyMapPaint(lv_layer_t *layer) {
  const int cx = 233, cy = 233;
  for (int i = 0; i < 3; i++) {
    lv_draw_arc_dsc_t a;
    lv_draw_arc_dsc_init(&a);
    a.center.x = cx; a.center.y = cy;
    a.radius = (i == 2 && galaxy_ashen ? GALAXY_ASHEN_CORE : GALAXY_RINGS[i]) + 1;
    a.width = 3;
    a.start_angle = 0; a.end_angle = 360;
    a.color = lv_color_hex(i == 2 ? 0xFFD166 : UI_CYAN);
    a.opa = i == 2 ? LV_OPA_50 : LV_OPA_30;
    lv_draw_arc(layer, &a);
  }
  // Nebulae: soft overlapping glows on the outer ring, east and west.
  for (int s = -1; s <= 1; s += 2) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_opa = LV_OPA_20;
    d.bg_color = lv_color_hex(0x9B5FE0);
    lv_area_t a1 = { (lv_coord_t)(cx + s * 180 - 26), (lv_coord_t)(cy - 34), (lv_coord_t)(cx + s * 180 + 26), (lv_coord_t)(cy + 34) };
    lv_draw_rect(layer, &d, &a1);
    d.bg_color = lv_color_hex(UI_CYAN);
    d.bg_opa = LV_OPA_10;
    lv_area_t a2 = { (lv_coord_t)(cx + s * 180 - 16), (lv_coord_t)(cy - 22 + 8 * s), (lv_coord_t)(cx + s * 180 + 16), (lv_coord_t)(cy + 22 + 8 * s) };
    lv_draw_rect(layer, &d, &a2);
  }
  for (size_t i = 0; i < sizeof(GALAXY_LANES) / sizeof(GALAXY_LANES[0]); i++) {
    const int a = GALAXY_LANES[i][0], b = GALAXY_LANES[i][1];
    uiLine(layer, cx + galaxyX(a), cy + galaxyY(a), cx + galaxyX(b), cy + galaxyY(b), lv_color_hex(UI_CYAN), 3);
  }
  for (int h = 0; h < 4; h++) {
    const int f = GALAXY_HANGARS[h][0], hx = cx + GALAXY_HANGARS[h][1], hy = cy + GALAXY_HANGARS[h][2];
    const lv_color_t col = lv_color_hex(FACTION_RING_COLOR[f]);
    for (int k = 0; k < 2; k++) {
      const int p = GALAXY_HANGAR_LANES[h][k];
      uiLine(layer, hx, hy, cx + galaxyX(p), cy + galaxyY(p), col, 2);
    }
    lv_draw_triangle_dsc_t t;  // diamond = two triangles
    lv_draw_triangle_dsc_init(&t);
    t.color = col;
    t.opa = LV_OPA_COVER;
    t.p[0].x = hx - 9; t.p[0].y = hy; t.p[1].x = hx; t.p[1].y = hy - 9; t.p[2].x = hx + 9; t.p[2].y = hy;
    lv_draw_triangle(layer, &t);
    t.p[1].y = hy + 9;
    lv_draw_triangle(layer, &t);
  }
}

// v0.21.15 -- nearest-target touch (user, testing the v0.21.14 mockup: worried
// about "pressability, especially with the back button"; planet circles are
// ~6.5 mm on the 1.75" with 4 px gaps). One invisible layer over the whole
// map catches every touch and picks the closest target -- a planet in play
// or the back button -- within GALAXY_SNAP px of its center, so a tap in a
// gap, on a lane or just off a circle still lands. The target under your
// finger shows its pressed look while you hold (slide to change it; lift
// over empty space to cancel), and acts on release. The circles themselves
// are no longer clickable; the picture is unchanged.
static lv_obj_t *galaxy_back = nullptr;
static int galaxy_pressed = -1;  // planet index, GALAXY_BACK, or -1
static int galaxy_release = -1;  // target at release, acted on at CLICKED
static const int GALAXY_BACK = NUM_PLANETS;
static const int GALAXY_SNAP = 45, GALAXY_BACK_SNAP = 56;  // px from a target's center
static const int GALAXY_BACK_X = -88, GALAXY_BACK_Y = -88;

static lv_obj_t *galaxyTargetObj(int t) {
  if (t == GALAXY_BACK) return galaxy_back;
  if (t == GALAXY_ASHEN) return nullptr;
  return (t >= 0 && t < NUM_PLANETS) ? ring_btn[t] : nullptr;
}

static int galaxyNearest(const lv_point_t &pt) {
  const int x = pt.x - 233, y = pt.y - 233;
  int best = -1;
  long bestD2 = 0;
  for (int p = 0; p < NUM_PLANETS; p++) {
    if (!ring_btn[p]) continue;  // not in this game
    const long dx = x - galaxyX(p), dy = y - galaxyY(p), d2 = dx * dx + dy * dy;
    if (d2 <= (long)GALAXY_SNAP * GALAXY_SNAP && (best < 0 || d2 < bestD2)) { best = p; bestD2 = d2; }
  }
  if (galaxy_ashen) {  // the flagship: a wide, flat target in the middle
    const long d2 = (long)x * x / 4 + (long)y * y;
    if (d2 <= (long)GALAXY_ASHEN_SNAP * GALAXY_ASHEN_SNAP && (best < 0 || d2 < bestD2)) { best = GALAXY_ASHEN; bestD2 = d2; }
  }
  const long bx = x - GALAXY_BACK_X, by = y - GALAXY_BACK_Y, bd2 = bx * bx + by * by;
  if (UI_BACK_BUTTONS && bd2 <= (long)GALAXY_BACK_SNAP * GALAXY_BACK_SNAP && (best < 0 || bd2 < bestD2)) best = GALAXY_BACK;
  return best;
}

static void galaxyTouchCb(lv_event_t *e) {
  const lv_event_code_t code = lv_event_get_code(e);
  lv_indev_t *indev = lv_indev_active();
  if (!indev) return;
  lv_point_t pt;
  lv_indev_get_point(indev, &pt);
  if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
    const int t = galaxyNearest(pt);
    if (t != galaxy_pressed) {
      if (lv_obj_t *o = galaxyTargetObj(galaxy_pressed)) lv_obj_remove_state(o, LV_STATE_PRESSED);
      if (lv_obj_t *o = galaxyTargetObj(t)) lv_obj_add_state(o, LV_STATE_PRESSED);
      galaxy_pressed = t;
    }
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    galaxy_release = (code == LV_EVENT_RELEASED) ? galaxy_pressed : -1;
    if (lv_obj_t *o = galaxyTargetObj(galaxy_pressed)) lv_obj_remove_state(o, LV_STATE_PRESSED);
    galaxy_pressed = -1;
  } else if (code == LV_EVENT_CLICKED) {
    // Act on CLICKED (LVGL's last event for a tap), like every other screen:
    // it rebuilds the screen, deleting this layer -- return at once.
    const int t = galaxy_release;
    galaxy_release = -1;
    if (t == GALAXY_BACK) { backToHome(nullptr); return; }
    if (t == GALAXY_ASHEN) { showAshen(nullptr); return; }
    if (t >= 0 && t < NUM_PLANETS) { currentPlanetIdx = t; showPlanetDetail(); return; }
  }
}

static void showPlanets(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_PLANETS;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  for (int p = 0; p < NUM_PLANETS; p++) { ring_btn[p] = nullptr; ring_tag[p] = nullptr; }
  galaxy_back = nullptr;
  ui_back_fn = backToHome;  // 2.0-dev.7: its back is part of the touch layer, so the edge swipe calls this
  galaxy_pressed = -1;

  GameState gs = gameStateRead();
  const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  uiAddRim(gs.roundLimit, gs.round, accent);
  galaxy_ashen = snapshotAshenAlive(gs);
  galaxy_ashen_hp = gs.ashenHp;
  galaxy_ashen_max = gs.ashenMaxHp;

  galaxyBakeMap();
  lv_obj_t *map = lv_obj_create(screen_root);
  lv_obj_remove_style_all(map);
  lv_obj_set_size(map, 466, 466);
  lv_obj_set_pos(map, 0, 0);
  lv_obj_clear_flag(map, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(map, galaxyMapDrawCb, LV_EVENT_DRAW_MAIN, nullptr);

  // The two free diagonals (v0.21.15 swap): back top-left -- the roomier
  // pocket, no lanes cross it -- and GALAXY + your Credits bottom-left.
  lv_obj_t *t = lv_label_create(screen_root);
  lv_label_set_text(t, "GALAXY");
  lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_letter_space(t, 3, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(accent), 0);
  lv_obj_align(t, LV_ALIGN_CENTER, -80, 74);
  char buf[16];
  snprintf(buf, sizeof(buf), "%d CR", gs.players[MY_SLOT].credits);
  lv_obj_t *cr = lv_label_create(screen_root);
  lv_label_set_text(cr, buf);
  lv_obj_set_style_text_font(cr, &sf_font_montserrat_12, 0);
  lv_obj_set_style_text_color(cr, lv_color_hex(UI_DIM), 0);
  lv_obj_align(cr, LV_ALIGN_CENTER, -80, 92);
  lv_obj_t *back = lv_btn_create(screen_root);
  uiStyleButton(back, UI_LINE, 1, UI_BG, LV_RADIUS_CIRCLE);
  lv_obj_set_size(back, 60, 60);
  lv_obj_set_pos(back, 233 - 88 - 30, 233 - 88 - 30);
  galaxy_back = back;
  if (!UI_BACK_BUTTONS) lv_obj_add_flag(back, LV_OBJ_FLAG_HIDDEN);  // 2.0-dev.8: swipe instead
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(bl, lv_color_hex(UI_DIM), 0);
  lv_obj_center(bl);
  lv_obj_clear_flag(back, LV_OBJ_FLAG_CLICKABLE);  // the touch layer handles it (v0.21.15)
  lv_obj_set_style_border_color(back, lv_color_hex(UI_CYAN), LV_STATE_PRESSED);

  if (galaxy_ashen) {
    galaxy_ashen_obj = lv_obj_create(screen_root);
    lv_obj_remove_style_all(galaxy_ashen_obj);
    lv_obj_clear_flag(galaxy_ashen_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(galaxy_ashen_obj, 80, 38);
    lv_obj_set_pos(galaxy_ashen_obj, 233 - 40, 233 - 19);
    lv_obj_add_event_cb(galaxy_ashen_obj, galaxyAshenDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
  }
  for (int p = 0; p < NUM_PLANETS; p++) {
    const int x = 233 + galaxyX(p), y = 233 + galaxyY(p);
    const int D = (galaxy_ashen && galaxyIsCore(p)) ? GALAXY_ASHEN_D : GALAXY_D;
    if (!gs.planetIncluded[p]) {
      // Not in this game: an empty slot, so the table still matches.
      lv_obj_t *slot = lv_obj_create(screen_root);
      uiStyleButton(slot, UI_RING, 2, UI_BG, LV_RADIUS_CIRCLE);  // style first (Convention 13)
      lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(slot, D - 16, D - 16);
      lv_obj_set_pos(slot, x - (D - 16) / 2, y - (D - 16) / 2);
      continue;
    }
    lv_obj_t *btn = lv_btn_create(screen_root);
    uiStyleButton(btn, UI_LINE, 2, UI_BG, LV_RADIUS_CIRCLE);
    lv_obj_set_size(btn, D, D);
    lv_obj_set_pos(btn, x - D / 2, y - D / 2);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);  // the touch layer picks the nearest (v0.21.15)
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_CYAN), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 3, LV_STATE_PRESSED);

    lv_obj_t *img = lv_image_create(btn);
    const lv_image_dsc_t *art = ringPlanetImage(p, D == GALAXY_D ? GALAXY_ART : 40);
    if (art) lv_image_set_src(img, art);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, D == GALAXY_D ? -4 : -3);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

    // Name tag on the screen, not the button (a content-sized label is capped
    // at its parent's width -- CORUSCANT is wider than the circle).
    lv_obj_t *tag = lv_label_create(screen_root);
    lv_label_set_text(tag, PLANETS[p].name);
    lv_obj_set_style_text_font(tag, &sf_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(tag, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(tag, LV_OPA_80, 0);
    lv_obj_set_style_radius(tag, 4, 0);
    lv_obj_set_style_pad_hor(tag, 3, 0);
    lv_obj_clear_flag(tag, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(tag, LV_ALIGN_CENTER, galaxyX(p), galaxyY(p) + (D == GALAXY_D ? 20 : 16));

    ring_btn[p] = btn;
    ring_tag[p] = tag;
    styleRingPlanet(gs, p);
#if defined(SF_PERF_PROBE) && (SF_PERF_PROBE & 4)
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);  // perf_ab probe: circles without fill / border
    lv_obj_set_style_border_width(btn, 0, 0);
#endif
#if defined(SF_PERF_PROBE) && (SF_PERF_PROBE & 8)
    lv_obj_set_style_bg_opa(tag, LV_OPA_TRANSP, 0);  // perf_ab probe: name tags without their backing
#endif
  }

  // Topmost: the invisible touch layer that snaps every tap to the nearest
  // target (galaxyTouchCb, v0.21.15).
  lv_obj_t *touch = lv_obj_create(screen_root);
  lv_obj_remove_style_all(touch);
  lv_obj_set_size(touch, 466, 466);
  lv_obj_set_pos(touch, 0, 0);
  lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(touch, galaxyTouchCb, LV_EVENT_ALL, nullptr);
}

// ---------------------------------------------------------------------------
// PLANET DETAIL  (Invest/Movement: showPlanetPage above, v0.21.4;
//                 Resolution: launch takeover)
// ---------------------------------------------------------------------------
static void revealCb(lv_event_t *e) {
  int encoded = (int)(intptr_t)lv_event_get_user_data(e);
  gameStateRevealCharacteristic(MY_SLOT, encoded / 10, encoded % 10);
  showPlanetDetail();
}

static void investDefenseCb(lv_event_t *e) {
  (void)e;
  gameStateInvestDefense(MY_SLOT, currentPlanetIdx);
  showPlanetDetail();
}

// ---------------------------------------------------------------------------
// PLANET PAGE (v0.21.4 UI refresh; canvas row "Planet
// page by phase"). The turning planet up top, its name, and who holds it;
// then what this phase lets you do:
//   Invest, your planet  -> GARRISON n / 8 + a big INVEST DEFENSE pill.
//                           INTEL (right circle) flips to the intel tiles.
//   Invest, other planet -> DISCOVER INTEL: 2x2 tiles, revealed ones show
//                           the value, the rest REVEAL + cost.
//   Movement             -> the same tiles, read-only (unrevealed = ???).
//   Resolution (v0.21.6)  -> read-only intel chips + FIGHT / NEGOTIATE / PAY.
// Touch: back/INTEL circles 96px at (112|354, 120) -> visible edge ~214px
// from center; tiles' far corners <= ~211px; pill corners ~170px.
// ---------------------------------------------------------------------------
static bool planet_page_intel = false;  // your planet in Invest: tiles instead of defense

static void planetPageToggleCb(lv_event_t *e) {
  (void)e;
  planet_page_intel = !planet_page_intel;
  showPlanetDetail();
}

static void planetBackCb(lv_event_t *e) {
  planet_page_intel = false;
  // Leaving with an uncommitted session open (e.g. after a refusal) closes
  // it -- free, since no approach was committed (v0.21.6). Scoped so the
  // ~4KB copy is gone before showPlanets() reads its own.
  {
    GameState gs = gameStateRead();
    if (gs.takeoverPlanet[MY_SLOT] >= 0 && gs.takeoverMethod[MY_SLOT] != TAKEOVER_NEGOTIATE)
      gameStateCancelTakeover(MY_SLOT);
  }
  showPlanets(e);
}

// 2.0: tapping a revealed trait spells it out (the tile shows it too).
static void traitInfoCb(lv_event_t *e) {
  const int t = (int)(intptr_t)lv_event_get_user_data(e);
  char body[96];
  snprintf(body, sizeof(body), "Hold this world: %s.", traitEffect(t));
  v2Card("TRAIT", UI_CYAN, traitName(t, currentPlanetIdx), body, 8000);
}
// 2.0: send the diplomat / buy at this world's market from its page.
static void showPlanetDetail();
static void planetDiplomatCb(lv_event_t *e) {
  (void)e;
  if (gameStateSendDiplomat(MY_SLOT, currentPlanetIdx)) uiToast("DIPLOMAT SENT", 0x8CE7A0);
  showPlanetDetail();
}
static void planetMarketCb(lv_event_t *e) {
  (void)e;
  if (gameStateMarketBuy(MY_SLOT, currentPlanetIdx)) uiToast("BOUGHT", 0xFFD166);
  showPlanetDetail();
}

// One 150x72 intel tile. Revealed: key + value (+ your reputation with the
// planet's faction under Affiliation). Hidden: a REVEAL button in Invest,
// a dashed-looking "???" otherwise.
static void planetIntelTile(const GameState &gs, int p, int c, int x, int y, bool canReveal) {
  const Player &me = gs.players[MY_SLOT];
  static const char *KEYS[NUM_CHARACTERISTICS] = { "SYMPATHETIC TO", "RESOURCES", "TRAIT", "DEFENSE" };  // 2.0 wording
  const bool revealed = me.revealed[p][c];
  const int cost = snapshotRevealCost(gs, MY_SLOT, p, c);  // 2.0: a free one a round with TR_SPYNET
  const bool afford = me.credits >= cost;
  lv_obj_t *t = lv_obj_create(screen_root);
  // Style FIRST: in LVGL 9 set_pos/set_size are local styles, and
  // uiStyleButton's remove_style_all wipes them -- v0.21.4 styled after
  // placing, which stacked every tile at (0,0) off the glass.
  if (revealed) uiStyleButton(t, UI_LINE, 1, UI_BG, 16);
  else if (canReveal) uiStyleButton(t, afford ? UI_CYAN : UI_LINE, 2, afford ? uiTint(UI_CYAN) : UI_BG, 16);
  else uiStyleButton(t, UI_RING, 1, UI_BG, 16);
  lv_obj_set_size(t, 150, 72);
  lv_obj_set_pos(t, x, y);
  lv_obj_t *key = lv_label_create(t);
  lv_label_set_text(key, KEYS[c]);
  lv_obj_set_style_text_font(key, &sf_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(key, 1, 0);
  lv_obj_set_style_text_color(key, lv_color_hex(UI_CYAN), 0);

  if (revealed && c == 2) {  // 2.0 (user: "it just says freight hub, what does that do for me?"): name on top, effect below
    lv_obj_add_event_cb(t, traitInfoCb, LV_EVENT_CLICKED, (void *)(intptr_t)gs.planetTrait[p]);
    lv_label_set_text(key, traitName(gs.planetTrait[p], p));
    lv_obj_set_style_text_letter_space(key, 0, 0);
    lv_label_set_long_mode(key, LV_LABEL_LONG_DOT);
    lv_obj_set_width(key, 144);
    lv_obj_set_style_text_align(key, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(key, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_t *v = lv_label_create(t);
    lv_label_set_text(v, traitEffect(gs.planetTrait[p]));
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(UI_TEXT), 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, 140);
    lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 25);
    return;
  }
  if (revealed) {
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *v = lv_label_create(t);
    lv_label_set_text(v, planetCharValueNow(p, c));  // v0.25.4: this game's galaxy
    lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, 140);
    if (c == 0) {
      int f = gameStateFactionForPlanet(p);
      lv_obj_align(key, LV_ALIGN_TOP_MID, 0, 6);
      lv_obj_align(v, LV_ALIGN_CENTER, 0, 1);
      if (f >= 0 && f < 4) {
        lv_obj_set_style_text_color(v, lv_color_hex(FACTION_TEXT_COLOR[f]), 0);
        char rep[32];
        snprintf(rep, sizeof(rep), "YOU: %s", REP_TIER_NAMES[snapshotReputationTier(gs, MY_SLOT, f)]);
        lv_obj_t *r = lv_label_create(t);
        lv_label_set_text(r, rep);
        lv_obj_set_style_text_font(r, &sf_font_montserrat_12, 0);
        lv_obj_set_style_text_color(r, lv_color_hex(UI_DIM), 0);
        lv_obj_align(r, LV_ALIGN_BOTTOM_MID, 0, -5);
      }
    } else {
      lv_obj_align(key, LV_ALIGN_TOP_MID, 0, 8);
      lv_obj_align(v, LV_ALIGN_CENTER, 0, 8);
    }
    return;
  }

  lv_obj_align(key, LV_ALIGN_CENTER, 0, -11);
  lv_obj_t *v = lv_label_create(t);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
  lv_obj_align(v, LV_ALIGN_CENTER, 0, 11);
  if (canReveal) {
    char txt[24];
    if (cost == 0) snprintf(txt, sizeof(txt), "REVEAL - FREE");
    else snprintf(txt, sizeof(txt), "REVEAL - %d CR", cost);
    lv_label_set_text(v, txt);
    lv_obj_set_style_text_color(v, lv_color_hex(afford ? 0xFFC933 : UI_FAINT), 0);
    if (afford) lv_obj_add_event_cb(t, revealCb, LV_EVENT_CLICKED, (void *)(intptr_t)(p * 10 + c));
    else lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
  } else {
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(v, "???");
    lv_obj_set_style_text_color(v, lv_color_hex(UI_FAINT), 0);
  }
}

// ---- Resolution (v0.21.6; mockup "Planet + approach"): the planet page IS
// the approach screen now -- FIGHT / NEGOTIATE / PAY sit right on it, and
// the separate CHOOSE APPROACH screen is gone (one tap fewer to any
// takeover). Each button opens the takeover session and goes straight to
// its next step: FIGHT -> battle briefing, NEGOTIATE -> the offers, PAY ->
// the price confirm. Backing out of the briefing / pay confirm closes the
// (uncommitted, so free) session and returns here; a refusal (e.g. another
// battle running, not enough Credits) comes back here with the reason.
// Intel shows as four read-only chips. Circles 96px at (130,354) /
// (233,366) / (336,354): visible edge <= ~207px from center, ~8px apart.
static void commitFightCb(lv_event_t *e);
static void planetFightCb(lv_event_t *e) {
  gameStateBeginTakeover(MY_SLOT, currentPlanetIdx);
  commitFightCb(e);  // -> briefing (defined with it below)
}

static void planetNegotiateCb(lv_event_t *e) {
  (void)e;
  gameStateReputationScores(MY_SLOT, rep_base);  // v0.30.0: the result card's arrows compare against this
  rep_base_set = true;
  gameStateBeginTakeover(MY_SLOT, currentPlanetIdx);
  gameStateCommitTakeover(MY_SLOT, TAKEOVER_NEGOTIATE);
  showNegotiate();
}

static void planetPayCb(lv_event_t *e) {
  (void)e;
  gameStateBeginTakeover(MY_SLOT, currentPlanetIdx);
  showConfirmPay();
}

// Uppercased copy of a characteristic value for the small chips.
static void chipValue(char *out, size_t n, const char *s) {
  size_t i = 0;
  for (; s[i] && i + 1 < n; i++) out[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
  out[i] = '\0';
}

static void planetIntelChip(const GameState &gs, int p, int c, int x) {
  static const char *KEYS[NUM_CHARACTERISTICS] = { "SYMP.", "RES.", "TRAIT", "DEF." };  // 2.0 wording
  static const char *CHIP_FACTION[4] = { "REBEL", "EMPIRE", "PYKE", "CRIMSON" };
  const bool rev = gs.players[MY_SLOT].revealed[p][c];
  lv_obj_t *t = lv_obj_create(screen_root);
  uiStyleButton(t, rev ? UI_LINE : UI_RING, 1, UI_BG, 10);  // style first (Convention 13)
  lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(t, 76, 48);
  lv_obj_set_pos(t, x, 204);
  lv_obj_t *k = lv_label_create(t);
  lv_label_set_text(k, KEYS[c]);
  lv_obj_set_style_text_font(k, &sf_font_montserrat_12, 0);
  lv_obj_set_style_text_color(k, lv_color_hex(UI_CYAN), 0);
  lv_obj_align(k, LV_ALIGN_TOP_MID, 0, 5);
  lv_obj_t *v = lv_label_create(t);
  lv_obj_set_style_text_font(v, &lv_font_montserrat_14, 0);
  lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, -5);
  char buf[24];
  uint32_t col = UI_TEXT;
  if (!rev) { snprintf(buf, sizeof(buf), "???"); col = UI_FAINT; }
  else if (c == 0) {
    int f = gameStateFactionForPlanet(p);
    snprintf(buf, sizeof(buf), "%s", (f >= 0 && f < 4) ? CHIP_FACTION[f] : "?");
    if (f >= 0 && f < 4) col = FACTION_TEXT_COLOR[f];
  } else if (c == 2) snprintf(buf, sizeof(buf), "KNOWN");  // full text is too long for a chip; it's on the briefing
  else chipValue(buf, sizeof(buf), planetCharValueNow(p, c));
  lv_label_set_text(v, buf);
  lv_obj_set_style_text_color(v, lv_color_hex(col), 0);
}

static void planetResolutionBody(const GameState &gs, int p, bool mine, uint32_t accent) {
  const Player &me = gs.players[MY_SLOT];
  for (int c = 0; c < NUM_CHARACTERISTICS; c++) planetIntelChip(gs, p, c, 72 + c * 82);

  // Status line: a refusal from the last attempt here, else your standing
  // with the planet's faction (once you know who they are).
  lv_obj_t *st = lv_label_create(screen_root);
  lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(st, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(st, 320);
  lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 262);
  const int f = gameStateFactionForPlanet(p);
  if (gs.takeoverPlanet[MY_SLOT] == p && gs.takeoverResult[MY_SLOT][0]) {
    lv_label_set_text(st, gs.takeoverResult[MY_SLOT]);
    lv_obj_set_style_text_color(st, lv_color_hex(UI_RED), 0);
  } else if (!mine && me.revealed[p][0] && f >= 0 && f < 4) {
    char b[48];
    snprintf(b, sizeof(b), "%s see you as %s", FACTION_SHORT[f], REP_TIER_NAMES[snapshotReputationTier(gs, MY_SLOT, f)]);
    lv_label_set_text(st, b);
    lv_obj_set_style_text_color(st, lv_color_hex(UI_DIM), 0);
  } else {
    lv_label_set_text(st, "");
  }

  if (mine) {
    uiLabel(screen_root, "YOU HOLD THIS WORLD", accent, nullptr, 300, 2);
    { char w[48]; if (gs.planetVpPending[p]) snprintf(w, sizeof(w), "Its VP waits on your promise"); else snprintf(w, sizeof(w), "Worth %d VP while you hold it", planetVPNow(p, MY_SLOT));
      uiLabel(screen_root, w, UI_DIM, &lv_font_montserrat_14, 332, 0); }
    return;
  }
  if (me.takeoverLocked[p]) {
    uiLabel(screen_root, "You've made your move here this round.\nTry again next round.", 0xFFC933,
            &lv_font_montserrat_14, 318, 0);
    return;
  }
  // 2.0 deals: a promise to stay away closes the world to you; one not to
  // attack a player's worlds greys out FIGHT there (the rules refuse it too).
  char word[96];
  if (snapshotDealBlock(gs, MY_SLOT, p, TAKEOVER_PAY, word, sizeof(word)) >= 0) {
    lv_obj_t *w = uiLabel(screen_root, word, accent, &lv_font_montserrat_14, 318, 0);
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w, 320);
    return;
  }
  const bool noFight = snapshotDealBlock(gs, MY_SLOT, p, TAKEOVER_FIGHT, word, sizeof(word)) >= 0;
  if (noFight && !(gs.takeoverPlanet[MY_SLOT] == p && gs.takeoverResult[MY_SLOT][0])) {
    lv_label_set_text(st, word);
    lv_obj_set_style_text_color(st, lv_color_hex(accent), 0);
  }
  // v0.28.3: another commander's world can only be taken by force.
  if (gs.planetOwner[p] >= 0 && gs.planetOwner[p] != MY_SLOT) {
    lv_obj_t *fb = uiCircleButton(screen_root, 233, 360, 104, "FIGHT", UI_RED, true, noFight ? nullptr : planetFightCb);
    if (noFight) uiDimColors(fb, LV_OPA_40);
    return;
  }
  uiCircleButton(screen_root, 130, 354, 96, "FIGHT", UI_RED, true, planetFightCb);
  lv_obj_t *neg = uiCircleButton(screen_root, 233, 366, 96, "NEGOTIATE", UI_CYAN, true, planetNegotiateCb);
  lv_obj_set_style_text_font(lv_obj_get_child(neg, 0), &lv_font_montserrat_14, 0);
  char pay[24];
  snprintf(pay, sizeof(pay), "PAY\n%d CR", snapshotPayPrice(gs, MY_SLOT, p));
  uiCircleButton(screen_root, 336, 354, 96, pay, 0xFFC933, true, planetPayCb);
}

static void showPlanetPage(const GameState &gs) {
  const int p = currentPlanetIdx;
  const Player &me = gs.players[MY_SLOT];
  const bool mine = gs.planetOwner[p] == MY_SLOT;
  const bool invest = gs.phase == PHASE_INVEST;
  const uint32_t accent = uiAccentFor(me.chosenFaction);
  if (!mine) planet_page_intel = false;
  const bool defenseView = mine && invest && !planet_page_intel;
  uiAddRim(gs.roundLimit, gs.round, accent);

  uiCircleButton(screen_root, 112, 120, 96, LV_SYMBOL_LEFT, UI_DIM, false, planetBackCb);
  if (mine && invest) {
    lv_obj_t *tog = uiCircleButton(screen_root, 354, 120, 96, planet_page_intel ? "DEFENSE" : "INTEL", UI_CYAN, false, planetPageToggleCb);
    lv_obj_set_style_text_font(lv_obj_get_child(tog, 0), &lv_font_montserrat_14, 0);
  }

  lv_obj_t *art = lv_obj_create(screen_root);
  lv_obj_remove_style_all(art);
  lv_obj_set_size(art, 112, 112);  // 28x28 planet grid at 4px blocks
  lv_obj_set_pos(art, 233 - 56, 28);
  lv_obj_clear_flag(art, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(art, spinPlanetDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)p);
  trackAnimated(art);

  uiLabel(screen_root, PLANETS[p].name, UI_TEXT, nullptr, 146, 3);

  // Who holds it. v0.21.11 (user): no garrison shown at all unless it's
  // yours (then the exact count) -- not even a tier or "???".
  char sub[64];
  uint32_t subColor = UI_DIM;
  const int owner = gs.planetOwner[p];
  if (mine) {
    if (defenseView && gs.planetVpPending[p]) snprintf(sub, sizeof(sub), "YOURS - ITS VP WAITS ON YOUR PROMISE");
    else if (defenseView) snprintf(sub, sizeof(sub), "YOURS - WORTH %d VP WHILE YOU HOLD IT", planetVPNow(p, MY_SLOT));
    else snprintf(sub, sizeof(sub), "YOURS - GARRISON %d / %d", gs.planetDefense[p], MAX_PLANET_DEFENSE);
    subColor = accent;
  } else {
    char held[32];
    int f = owner >= 0 ? gs.players[owner].chosenFaction : -1;
    if (f >= 0 && f < 4) { snprintf(held, sizeof(held), "Held by %s", FACTION_SHORT[f]); subColor = FACTION_TEXT_COLOR[f]; }
    else snprintf(held, sizeof(held), owner >= 0 ? "Held by a rival" : gs.planetUnbound[p] ? "Held by THE UNBOUND - no Pay" : "Sworn to no one");  // 2.0
    snprintf(sub, sizeof(sub), "%s", held);
  }
  uiLabel(screen_root, sub, subColor, &lv_font_montserrat_14, 176, 0);
  if (gs.phase == PHASE_RESOLUTION) { planetResolutionBody(gs, p, mine, accent); return; }

  char foot[48];
  if (defenseView) {
    char g[32];
    snprintf(g, sizeof(g), "GARRISON  %d / %d", gs.planetDefense[p], MAX_PLANET_DEFENSE);
    uiLabel(screen_root, g, UI_TEXT, nullptr, 212, 2);
    const bool full = gs.planetDefense[p] >= MAX_PLANET_DEFENSE;
    const bool afford = me.credits >= 2;
    lv_obj_t *pill = uiPill(screen_root, 103, 252, 260, 88, full ? "GARRISON FULL" : "INVEST DEFENSE",
                            full ? "8 ships is the most a planet holds" : (afford ? "+1 garrison ship - 2 CR" : "needs 2 CR"),
                            accent, (full || !afford) ? nullptr : investDefenseCb);
    if (full || !afford) uiDimColors(pill, LV_OPA_50);
    snprintf(foot, sizeof(foot), "PREPARE PHASE - %d CR", me.credits);
    uiLabel(screen_root, foot, UI_FAINT, &sf_font_montserrat_12, 356, 1);
    return;
  }

  uiLabel(screen_root, invest ? "DISCOVER INTEL" : "INTEL", UI_CYAN, &sf_font_montserrat_12, 202, 3);
  for (int c = 0; c < NUM_CHARACTERISTICS; c++)
    planetIntelTile(gs, p, c, (c % 2) ? 238 : 78, (c / 2) ? 302 : 220, invest);
  // 2.0: the diplomat sits in the top-right circle on worlds you don't hold
  // (circle visible edge ~213 px from center).
  if (invest && !mine) {
    const bool here = me.diplomatPlanet == p, away = me.diplomatPlanet >= 0;
    const bool can = snapshotCanSendDiplomat(gs, MY_SLOT, p);
    lv_obj_t *dip = uiCircleButton(screen_root, 354, 120, 96, here ? "DIPLOMAT\nHERE" : away ? "DIPLOMAT\nAWAY" : "SEND\nDIPLOMAT",
                                   0x8CE7A0, can, can ? planetDiplomatCb : nullptr);
    lv_obj_set_style_text_font(lv_obj_get_child(dip, 0), &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_align(lv_obj_get_child(dip, 0), LV_TEXT_ALIGN_CENTER, 0);
    if (!can) uiDimColors(dip, LV_OPA_50);
  }
  // 2.0: when this world's market is open, the footer is its BUY pill
  // (corners ~217 px from center).
  if (snapshotMarketIsOpen(gs, p)) {
    const bool can = snapshotCanBuy(gs, MY_SLOT, p);
    char b[40];
    if (gs.marketStock[p] == 0) snprintf(b, sizeof(b), "%s - SOLD OUT", goodName(gs.planetGood[p]));
    else if (!invest) snprintf(b, sizeof(b), "%s - BUY IN PREPARE", goodName(gs.planetGood[p]));
    else snprintf(b, sizeof(b), "BUY %s - %d CR", goodName(gs.planetGood[p]), snapshotMarketPrice(gs, MY_SLOT, p));
    lv_obj_t *buy = uiPill(screen_root, 123, 380, 220, 46, b, nullptr, 0xFFD166, can ? planetMarketCb : nullptr);
    lv_obj_set_style_text_font(lv_obj_get_child(buy, 0), &sf_font_montserrat_12, 0);
    if (!can) uiDimColors(buy, LV_OPA_50);
    return;
  }
  if (invest) snprintf(foot, sizeof(foot), "PREPARE PHASE - %d CR", me.credits);
  else snprintf(foot, sizeof(foot), "MOVE YOUR FLEETS ON THE BOARD");
  uiLabel(screen_root, foot, UI_FAINT, &sf_font_montserrat_12, 386, 1);
}

static void showPlanetDetail() {
  currentScreen = SCR_PLANET_DETAIL;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  // v0.30.0: a conversation still open here (e.g. the round banner took you
  // Home mid-way) -- this planet's page is that conversation.
  if (MY_SLOT >= 0 && gs.takeoverPlanet[MY_SLOT] == currentPlanetIdx && snapshotNegotiation(gs, MY_SLOT).open) { showNegotiate(); return; }
  showPlanetPage(gs);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// BATTLE BRIEFING (v0.20.1; the Battle Forecast since v0.21.7): FIGHT on the
// planet page lands here first. Shows what you already know -- garrison,
// both sides' roll formulas with their faction sources, the odds -- and lets
// you pick the attacking fleet. BACK is free (nothing new is revealed here);
// ENGAGE commits. Phone twin: the briefing card in web_server.cpp.
// ---------------------------------------------------------------------------
static int briefing_fleet = -1;  // v0.28.0: a MASK of the fleets sent in (bit per fleet); -1 = the first ready fleet
static void showBriefing();

// Short faction words for roll sources ("+1 Rebel support").
static const char *FACTION_WORDS[4] = { "Rebel", "Imperial", "Pyke", "Crimson Dawn" };

static void commitFightCb(lv_event_t *e) {
  (void)e;
  forecast_pvp = false;
  briefing_fleet = -1;
  showBriefing();
}

static void confirmFightCb(lv_event_t *e) {
  (void)e;
  gameStateCommitFight(MY_SLOT, briefing_fleet);
  GameState gs = gameStateRead();
  if (gs.battleActive && gs.battleKind == BATTLE_PLANET && gs.battleAttackerSlot == MY_SLOT) showBattle();
  else showPlanetDetail();  // refused: the planet page shows why (v0.21.6)
}

static void briefingFleetCb(lv_event_t *e) {  // v0.28.0: toggles the fleet in or out (combined assault)
  const int f = (int)(intptr_t)lv_event_get_user_data(e);
  briefing_fleet = (briefing_fleet < 0 ? 0 : briefing_fleet) ^ (1 << f);
  showBriefing();
}

static void backFromBriefingCb(lv_event_t *e) {
  (void)e;
  gameStateCancelTakeover(MY_SLOT);  // nothing committed yet -- free (v0.21.6: no approach screen)
  showPlanetDetail();
}

// Exact exchange odds for d6+atk vs d6+def (higher wins, equal = both
// miss), as whole percentages -- the Fire Emblem-style forecast (v0.21.7).
static void exchangeOdds(int atk, int def, int &win, int &tie, int &lose) {
  int w = 0, l = 0;
  for (int a = 1; a <= 6; a++)
    for (int d = 1; d <= 6; d++) {
      if (a + atk > d + def) w++;
      else if (a + atk < d + def) l++;
    }
  // Round WIN and LOSE; TIE absorbs the rounding so an even fight reads
  // 42 / 16 / 42, not a lopsided 42 / 17 / 41.
  win = (w * 100 + 18) / 36;
  lose = (l * 100 + 18) / 36;
  tie = 100 - win - lose;
}

static void forecastLabel(const char *txt, uint32_t color, const lv_font_t *font, int cx, int y, int w) {
  lv_obj_t *l = lv_label_create(screen_root);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, w);
  lv_obj_set_pos(l, cx - w / 2, y);
}

static void forecastBar(int x, int y, int w, int h, uint32_t color) {
  if (w <= 0) return;
  lv_obj_t *b = lv_obj_create(screen_root);
  lv_obj_remove_style_all(b);  // style first (Convention 13)
  lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, 3, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, x, y);
}

// Ship strip (v0.21.9), drawn centered in its object: `count` triangles in
// `color`, facing right (yours) or left (the enemy, PvP). user_data =
// kind << 30 | count << 24 | 0xRRGGBB. (The garrison squares / strength
// meter kinds went away in v0.21.11: garrison size is never shown.)
enum { UNITS_SHIPS = 0, UNITS_SHIPS_LEFT = 1 };
static void forecastUnitsDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const uint32_t ud = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
  const int kind = ud >> 30, n = (ud >> 24) & 0x3F;
  const lv_color_t colFull = lv_color_hex(ud & 0xFFFFFF);
  const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
  {
    // v0.28.0: more than six ships (fleets sent in together) -> rows of six, smaller.
    const int rows = (n + 5) / 6 > 0 ? (n + 5) / 6 : 1;
    const int w = rows == 1 ? 14 : rows == 2 ? 8 : 6, h = w, step = rows == 1 ? 19 : 12, perRow = n < 6 ? n : 6;
    const int total = perRow * step - (step - w), rowH = rows == 1 ? 0 : (rows == 2 ? 10 : 7);
    for (int i = 0; i < n; i++) {
      const int x = cx - total / 2 + (i % 6) * step;
      const int cy = (a.y1 + a.y2) / 2 + ((i / 6) - (rows - 1) / 2.0f) * rowH;
      lv_draw_triangle_dsc_t d;
      lv_draw_triangle_dsc_init(&d);
      d.color = colFull;
      d.opa = LV_OPA_COVER;
      const int back = (kind == UNITS_SHIPS_LEFT) ? x + w : x, nose = (kind == UNITS_SHIPS_LEFT) ? x : x + w;
      d.p[0].x = back;      d.p[0].y = cy - h / 2;
      d.p[1].x = nose;      d.p[1].y = cy;
      d.p[2].x = back;      d.p[2].y = cy + h / 2;
      lv_draw_triangle(layer, &d);
    }
  }
}

static void forecastUnits(int cx, int y, int kind, int n, uint32_t color) {
  lv_obj_t *o = lv_obj_create(screen_root);
  lv_obj_remove_style_all(o);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(o, 130, 20);
  lv_obj_set_pos(o, cx - 65, y);
  lv_obj_add_event_cb(o, forecastUnitsDrawCb, LV_EVENT_DRAW_MAIN,
                      (void *)(uintptr_t)(((uint32_t)kind << 30) | ((uint32_t)(n & 0x3F) << 24) | (color & 0xFFFFFF)));
}

// The big die for one side: "d6" in a rounded square in that side's color,
// a "+N" badge on its corner when a modifier applies, and the faction whose
// support / resentment grants it shown as its insignia underneath.
static void forecastDie(int cx, uint32_t color, int mod, int sourceFaction) {
  lv_obj_t *die = lv_obj_create(screen_root);
  uiStyleButton(die, color, 3, uiTint(color), 14);  // style first (Convention 13)
  lv_obj_clear_flag(die, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(die, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_set_size(die, 62, 62);
  lv_obj_set_pos(die, cx - 31, 122);
  lv_obj_t *l = lv_label_create(die);
  lv_label_set_text(l, "d6");
  lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
  lv_obj_center(l);
  if (mod > 0) {
    lv_obj_t *badge = lv_obj_create(die);
    uiStyleButton(badge, color, 0, color, LV_RADIUS_CIRCLE);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(badge, 30, 30);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, 14, -14);
    lv_obj_t *bl = lv_label_create(badge);
    char t[8];
    snprintf(t, sizeof(t), "+%d", mod);
    lv_label_set_text(bl, t);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(UI_BG), 0);
    lv_obj_center(bl);
    if (sourceFaction >= 0 && sourceFaction < 4) {
      lv_obj_t *ins = uiInsignia(screen_root, sourceFaction, 28, UI_BG);
      lv_obj_set_pos(ins, cx - 14, 192);
    }
  }
}

// BATTLE FORECAST (v0.21.7; v0.21.9 redesign -- user: "a little small...
// lots of words"). Pictures instead of sentences: your fleet as a row of
// ship triangles in your color vs the garrison as squares (exact count),
// a 3-bar strength meter (tier known) or ??? (Defense not revealed); each
// side's roll as a big die with a +N badge, the faction behind the bonus as
// its insignia under the die; the odds as big WIN / LOSE numbers over a
// thicker bar. Then the fleet picker and ENGAGE. Same data as before --
// nothing new revealed. Touch: fleet buttons 96x52 at y294 (corners
// ~188px), back circle 64 at (140,388) (edge ~213px), ENGAGE pill
// 150x62 at (184,358) (far corner ~212px).
static void showBriefing() {
  currentScreen = SCR_BRIEFING;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  const int p = currentPlanetIdx;
  GameState gs = gameStateRead();
  const Player &me = gs.players[MY_SLOT];
  const uint32_t accent = uiAccentFor(me.chosenFaction);
  const uint32_t GARRISON_ORANGE = 0xFF7A45;
  uiAddRim(gs.roundLimit, gs.round, UI_RED);

  int strength[NUM_FLEETS];
  bool ready[NUM_FLEETS];  // has ships and hasn't already fought this round
  for (int f = 0; f < NUM_FLEETS; f++) {
    strength[f] = 0;
    for (int s = 0; s < FLEET_SHIPS; s++) if (me.fleetShipAlive[f][s]) strength[f]++;
    ready[f] = strength[f] > 0 && !me.fleetSpent[f];
  }
  // v0.28.0: briefing_fleet is a mask -- drop fleets that can't go; -1 = the first ready one.
  if (briefing_fleet < 0) {
    briefing_fleet = 0;
    for (int f = 0; f < NUM_FLEETS; f++) if (ready[f]) { briefing_fleet = 1 << f; break; }
  }
  int sent = 0;
  for (int f = 0; f < NUM_FLEETS; f++) {
    if (!ready[f]) briefing_fleet &= ~(1 << f);
    if (briefing_fleet & (1 << f)) sent += strength[f];
  }

  // PvP mode (v0.21.10): same screen, their fleet on the right, plain d6
  // both sides (no faction modifiers in PvP), refusals in the top line.
  const bool pvp = forecast_pvp && pvpTargetSlot >= 0 && pvpTargetSlot < MAX_PLAYERS
                   && pvpDefenderFleet >= 0 && pvpDefenderFleet < NUM_FLEETS;
  const int tf = pvp ? gs.players[pvpTargetSlot].chosenFaction : -1;
  const uint32_t themColor = (tf >= 0 && tf < 4) ? FACTION_RING_COLOR[tf] : UI_RED;
  if (pvp && pvp_refusal) uiLabel(screen_root, pvp_refusal, UI_RED, &sf_font_montserrat_12, 34, 0);
  else uiLabel(screen_root, "FORECAST", UI_RED, &sf_font_montserrat_12, 34, 4);
  char name[40];
  if (pvp) snprintf(name, sizeof(name), "%s FLEET %d", (tf >= 0 && tf < 4) ? FACTION_SHORT[tf] : "RIVAL", pvpDefenderFleet + 1);
  else snprintf(name, sizeof(name), "%s", PLANETS[p].name);
  uiLabel(screen_root, name, UI_TEXT, nullptr, 50, 3);

  BattleMods m;
  if (!pvp) m = snapshotPlanetBattleMods(gs, MY_SLOT, p);
  const int LX = 150, RX = 316;
  // Units.
  if (briefing_fleet > 0) forecastUnits(LX, 84, UNITS_SHIPS, sent, accent);
  else forecastLabel("NO FLEET", UI_FAINT, &lv_font_montserrat_14, LX, 86, 130);
  if (pvp)
    forecastUnits(RX, 84, UNITS_SHIPS_LEFT, fleetAlive(gs.players[pvpTargetSlot], pvpDefenderFleet), themColor);
  else  // v0.21.11: the garrison's size is never shown -- just who you face
    forecastLabel("GARRISON", GARRISON_ORANGE, &sf_font_montserrat_12, RX, 88, 130);
  // Dice + sources.
  forecastDie(LX, accent, m.atkMod, m.atkSourceFaction);
  forecastDie(RX, UI_RED, m.defMod, m.defSourceFaction);
  lv_obj_t *vs = lv_obj_create(screen_root);
  uiStyleButton(vs, UI_LINE, 1, UI_BG, LV_RADIUS_CIRCLE);
  lv_obj_clear_flag(vs, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(vs, 34, 34);
  lv_obj_set_pos(vs, 216, 136);
  lv_obj_t *vsl = lv_label_create(vs);
  lv_label_set_text(vsl, "VS");
  lv_obj_set_style_text_font(vsl, &sf_font_montserrat_12, 0);
  lv_obj_set_style_text_color(vsl, lv_color_hex(UI_DIM), 0);
  lv_obj_center(vsl);

  // Odds for one exchange: big WIN / LOSE, small TIE, thick bar.
  int win, tie, lose;
  exchangeOdds(m.atkMod, m.defMod, win, tie, lose);
  char buf[16];
  forecastLabel("WIN", accent, &sf_font_montserrat_12, 118, 224, 80);
  snprintf(buf, sizeof(buf), "%d%%", win);
  forecastLabel(buf, accent, nullptr, 118, 238, 80);
  snprintf(buf, sizeof(buf), "TIE %d%%", tie);
  forecastLabel(buf, UI_DIM, &sf_font_montserrat_12, 233, 244, 80);
  forecastLabel("LOSE", UI_RED, &sf_font_montserrat_12, 348, 224, 80);
  snprintf(buf, sizeof(buf), "%d%%", lose);
  forecastLabel(buf, UI_RED, nullptr, 348, 238, 80);
  const int barX = 83, barW = 300, gap = 2;
  int ww = (barW - 2 * gap) * win / 100, tw = (barW - 2 * gap) * tie / 100;
  int lw = (barW - 2 * gap) - ww - tw;
  forecastBar(barX, 268, ww, 12, accent);
  forecastBar(barX + ww + gap, 268, tw, 12, 0x3A4B5E);
  forecastBar(barX + ww + tw + 2 * gap, 268, lw, 12, UI_RED);

  // Fleet picker.
  for (int f = 0; f < NUM_FLEETS; f++) {
    const bool sel = (briefing_fleet & (1 << f)) != 0;
    lv_obj_t *btn = lv_btn_create(screen_root);
    uiStyleButton(btn, sel ? accent : (ready[f] ? UI_LINE : UI_RING), sel ? 2 : 1, sel ? uiTint(accent) : UI_BG, 16);
    lv_obj_set_size(btn, 96, 52);
    lv_obj_set_pos(btn, 80 + f * 105, 294);
    lv_obj_t *l = lv_label_create(btn);
    char t[24];
    if (ready[f]) snprintf(t, sizeof(t), "FLEET %d\n%d ships", f + 1, strength[f]);
    else if (strength[f] > 0) snprintf(t, sizeof(t), "FLEET %d\nspent", f + 1);
    else snprintf(t, sizeof(t), "FLEET %d\n%s", f + 1, me.fleetShipCount[f] > 0 ? "destroyed" : "not built");
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(sel ? accent : (ready[f] ? UI_TEXT : UI_FAINT)), 0);
    lv_obj_center(l);
    if (ready[f]) lv_obj_add_event_cb(btn, briefingFleetCb, LV_EVENT_CLICKED, (void *)(intptr_t)f);
    else lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  }

  lv_obj_t *back = lv_btn_create(screen_root);
  uiStyleButton(back, UI_LINE, 1, UI_BG, LV_RADIUS_CIRCLE);
  lv_obj_set_size(back, 64, 64);
  lv_obj_set_pos(back, 140 - 32, 388 - 32);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(bl, lv_color_hex(UI_DIM), 0);
  lv_obj_center(bl);
  lv_obj_add_event_cb(back, pvp ? backFromPvpForecastCb : backFromBriefingCb, LV_EVENT_CLICKED, nullptr);
  uiMarkBack(back);
  // 2.0-dev.9: centered now the back button is hidden; corners (148,420)/(318,420) ~205 px
  lv_obj_t *engage = uiPill(screen_root, 148, 358, 170, 62, briefing_fleet > 0 ? "ENGAGE" : "NO FLEET", nullptr,
                            UI_RED, briefing_fleet > 0 ? (pvp ? pvpEngageCb : confirmFightCb) : nullptr);
  if (briefing_fleet <= 0) uiDimColors(engage, LV_OPA_40);
}

// v0.29.0 PURCHASE card: the planet turning, "<PLANET> IS YOURS", and the reputation
// arrows; CONTINUE plays any transmission, then the planet page.
static void purchasedContinueCb(lv_event_t *e) {
  (void)e;
  rep_base_set = false;
  if (!showPendingTransmission()) showPlanetDetail();
}
static void showPurchased(int p) {
  currentScreen = SCR_CONFIRM_PAY;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  uiAddRim(gs.roundLimit, gs.round, accent);
  uiLabel(screen_root, "PURCHASE", UI_DIM, &sf_font_montserrat_12, 52, 4);
  lv_obj_t *pl = lv_obj_create(screen_root);
  lv_obj_remove_style_all(pl);
  lv_obj_clear_flag(pl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(pl, 112, 112);
  lv_obj_set_pos(pl, 233 - 56, 80);
  lv_obj_add_event_cb(pl, spinPlanetDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)p);
  trackAnimated(pl);
  char t[40];
  snprintf(t, sizeof(t), "%s IS YOURS", PLANETS[p].name);
  uiLabel(screen_root, t, accent, nullptr, 214, 2);
  { char w[40]; snprintf(w, sizeof(w), "+%d VP while you hold it", planetVPNow(p, MY_SLOT)); uiLabel(screen_root, w, UI_DIM, &lv_font_montserrat_14, 244, 0); }
  uiRepRow(gs, 276);
  uiPill(screen_root, 143, 368, 180, 62, "CONTINUE", nullptr, accent, purchasedContinueCb);
}

static void confirmPayCb(lv_event_t *e) {
  (void)e;
  gameStateReputationScores(MY_SLOT, rep_base);  // v0.29.0: the purchase card compares against this
  rep_base_set = true;
  gameStateCommitTakeover(MY_SLOT, TAKEOVER_PAY);
  // Pay resolves immediately. A successful purchase closes the session:
  // show any reputation fallout first, then bounce back to the planet
  // detail. A failed one (can't afford) leaves it open with the reason,
  // which the planet page shows (v0.21.6).
  int next;  // 0 = planet page, 1 = purchase card, 2 = transmission check (v0.30.2: decided before the next screen reads its own copy)
  {
    GameState gs = gameStateRead();
    if (gs.takeoverPlanet[MY_SLOT] >= 0) next = 0;
    else if (currentPlanetIdx >= 0 && currentPlanetIdx < NUM_PLANETS && gs.planetOwner[currentPlanetIdx] == MY_SLOT) next = 1;
    else next = 2;
  }
  if (next == 0) { rep_base_set = false; showPlanetDetail(); }
  else if (next == 1) showPurchased(currentPlanetIdx);
  else if (!showPendingTransmission()) showPlanetDetail();
}

static void backFromConfirmPayCb(lv_event_t *e) {
  (void)e;
  gameStateCancelTakeover(MY_SLOT);  // Pay not committed yet -- free
  showPlanetDetail();
}

static void cancelTakeoverCb(lv_event_t *e) {
  (void)e;
  gameStateCancelTakeover(MY_SLOT);
  if (!showPendingTransmission()) showPlanetDetail();
}

// CONFIRM PURCHASE (v0.25.1 restyle -- the last old-style takeover screen,
// found by the user 2026-10-15): the planet turning, the price big in gold,
// what you have, the reputation it earns (only once Affiliation is revealed,
// so nothing leaks), PAY + the back circle. Touch: PAY pill 180x72 at
// (143,300) (far corner ~165px); back circle at (233,408) (edge ~211px).
// Which faction the player can see behind this planet, or -1 (unrevealed).
static int negotiationFaction(const GameState &gs, int planet) {
  if (planet < 0 || planet >= NUM_PLANETS || !gs.players[MY_SLOT].revealed[planet][0]) return -1;
  const int f = gameStateFactionForPlanet(planet);
  return (f >= 0 && f < 4) ? f : -1;
}

static void showConfirmPay() {
  currentScreen = SCR_CONFIRM_PAY;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  const int p = currentPlanetIdx;
  const int price = gameStatePayPrice(MY_SLOT, p);
  GameState gs = gameStateRead();
  const int have = gs.players[MY_SLOT].credits;
  const bool afford = have >= price;
  const uint32_t GOLD = 0xFFD166;
  uiAddRim(gs.roundLimit, gs.round, GOLD);

  uiLabel(screen_root, "PURCHASE", UI_DIM, &sf_font_montserrat_12, 50, 4);
  uiLabel(screen_root, PLANETS[p].name, UI_TEXT, nullptr, 68, 3);

  lv_obj_t *pl = lv_obj_create(screen_root);
  lv_obj_remove_style_all(pl);
  lv_obj_clear_flag(pl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(pl, 96, 96);
  lv_obj_set_pos(pl, 233 - 48, 100);
  lv_obj_add_event_cb(pl, spinPlanetDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)p);
  trackAnimated(pl);

  char buf[48];
  snprintf(buf, sizeof(buf), "%d CR", price);
  uiLabel(screen_root, buf, afford ? GOLD : UI_RED, &sf_font_montserrat_36, 204, 2);
  snprintf(buf, sizeof(buf), afford ? "You have %d CR" : "You have %d CR - not enough", have);
  uiLabel(screen_root, buf, afford ? UI_DIM : UI_RED, &lv_font_montserrat_14, 250, 0);
  const int f = negotiationFaction(gs, p);
  if (f >= 0) {
    snprintf(buf, sizeof(buf), "THE %s WILL APPROVE", FACTION_SHORT[f]);
    uiLabel(screen_root, buf, FACTION_TEXT_COLOR[f], &sf_font_montserrat_12, 274, 2);
  }

  lv_obj_t *pay = uiPill(screen_root, 143, 300, 180, 72, "PAY", nullptr, GOLD, afford ? confirmPayCb : nullptr);
  if (!afford) uiDimColors(pay, LV_OPA_40);
  uiBackButton(screen_root, 233, 408, backFromConfirmPayCb);
}

// ---------------------------------------------------------------------------
// v0.30.0 NEGOTIATION = A CONVERSATION (mockup
// https://claude.ai/artifact/4KsxPW9rF5B8BHdSZiyLz1; twin of the phone's s.neg
// screen). Top: the speaker's 3x hologram (an UNKNOWN AMBASSADOR in torn gray
// until you've revealed Affiliation -- a face picked by planet, so it never
// gives the faction away), who it is, a pip per question. Then the question
// and its replies, or the favors, or the result card.
// Touch: reply rows 50 tall at y 238/294/350, 380/360/300 wide (far corners
// 198/211/224 px); LEAVE 120x40 at (173,406) (~221 px); CONTINUE 180x56 at
// (143,368) (~211 px); favor rows 52 tall at 226/284/342, 380/360/300 wide
// (<= ~220 px); NOTHING MORE 180x38 at (143,400) (~224 px).
// ---------------------------------------------------------------------------
static lv_obj_t *uiWrapLabel(lv_obj_t *parent, const char *text, uint32_t color, const lv_font_t *font, int y, int w) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  if (font) lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, w);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
  return l;
}

static void negAnswerCb(lv_event_t *e) {
  gameStateNegotiationAnswer(MY_SLOT, (int)(intptr_t)lv_event_get_user_data(e));
  showNegotiate();
}
static void negContinueCb(lv_event_t *e) {
  (void)e;
  gameStateNegotiationClose(MY_SLOT);
  rep_base_set = false;
  if (!showPendingTransmission()) showPlanetDetail();
}

// v0.30.1: they're hostile -- FIGHT reopens the planet to a fight and goes to the forecast.
static void negFightCb(lv_event_t *e) {
  gameStateNegotiationFight(MY_SLOT);
  rep_base_set = false;
  commitFightCb(e);  // -> the battle briefing for the reopened session
}

// One answer row: a button `w` wide at y, its text wrapped inside.
static void negRow(int y, int w, int h, const char *text, uint32_t color, int choice) {
  lv_obj_t *b = lv_btn_create(screen_root);
  uiStyleButton(b, color, 2, uiTint(color), 16);  // style first (Convention 13)
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, 233 - w / 2, y);
  lv_obj_add_event_cb(b, negAnswerCb, LV_EVENT_CLICKED, (void *)(intptr_t)choice);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(UI_TEXT), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(l, w - 28);
  lv_obj_center(l);
}

static void showNegotiate() {
  GameState gs = gameStateRead();
  const NegView v = snapshotNegotiation(gs, MY_SLOT);
  if (!v.open) { showPlanetDetail(); return; }  // refused (the planet page says why), or closed
  currentScreen = SCR_NEGOTIATE;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  const int p = v.planet, f = v.known ? v.faction : -1;
  uiAddRim(gs.roundLimit, gs.round, f >= 0 ? FACTION_RING_COLOR[f] : UI_DIM);

  // The speaker.
  lv_obj_t *frame = lv_obj_create(screen_root);
  uiStyleButton(frame, f >= 0 ? 0x7FE3FF : 0x8B9199, 1, f >= 0 ? 0x061621 : 0x111418, 14);
  lv_obj_set_style_border_opa(frame, LV_OPA_50, 0);
  lv_obj_set_style_clip_corner(frame, true, 0);
  lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(frame, 84, 88);
  lv_obj_set_pos(frame, 233 - 42, 30);
  const int face = f >= 0 ? commanderVoiceFor(f, gs.players[MY_SLOT].commander) : p % NUM_COMMANDERS;
  lv_obj_t *img = uiPortrait(frame, face, f >= 0 ? CMD_IMG_HOLO3 : CMD_IMG_AMB);
  lv_obj_align(img, LV_ALIGN_BOTTOM_MID, 0, 2);
  char who[48];
  snprintf(who, sizeof(who), "%s - %s", PLANETS[p].name, f >= 0 ? FACTION_SHORT[f] : "UNKNOWN AMBASSADOR");
  uiLabel(screen_root, who, f >= 0 ? FACTION_TEXT_COLOR[f] : UI_DIM, &sf_font_montserrat_12, 122, 2);
  // A pip per question: green liked, gray neutral, red offended, ringed = now.
  for (int k = 0; k < v.count; k++) {
    lv_obj_t *d = lv_obj_create(screen_root);
    const uint32_t col = v.marks[k] == NEG_MARK_LIKED ? 0x8CE7A0 : v.marks[k] == NEG_MARK_NEUTRAL ? UI_DIM : v.marks[k] == NEG_MARK_OFFENDED ? UI_RED : UI_BG;
    const bool cur = v.result == NEG_TALKING && k == v.step;
    uiStyleButton(d, cur ? UI_CYAN : (v.marks[k] ? col : UI_LINE), cur ? 2 : 1, col, LV_RADIUS_CIRCLE);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(d, 10, 10);
    lv_obj_set_pos(d, 233 - (v.count * 18 - 8) / 2 + k * 18, 140);
  }

  char buf[200];
  if (v.result == NEG_HOSTILE) {  // v0.30.1: they're hostile -- FIGHT (the one allowed switch) or WALK AWAY
    uiLabel(screen_root, v.count ? "WE KNOW WHO YOU ARE" : "THEY WON'T TALK TO YOU", UI_RED, nullptr, 158, 2);
    snprintf(buf, sizeof(buf), "\"%s\"", v.hostileLine >= 0 ? NEG_HOSTILE_LINES[v.hostileLine] : "");
    uiWrapLabel(screen_root, buf, 0xCFF4FF, &lv_font_montserrat_14, 190, 330);
    snprintf(buf, sizeof(buf), "%sNo talks this round.\nYou can still fight for %s.", v.count ? "Affiliation revealed. No standing lost.\n" : "", PLANETS[p].name);
    uiWrapLabel(screen_root, buf, UI_DIM, &sf_font_montserrat_12, 262, 330);
    // Corners: FIGHT (78..218, 330..390) ~220px; WALK AWAY (248..388) ~220px.
    uiPill(screen_root, 78, 330, 140, 60, "FIGHT", nullptr, UI_RED, negFightCb);
    lv_obj_t *walk = uiPill(screen_root, 248, 330, 140, 60, "WALK AWAY", nullptr, UI_DIM, negContinueCb);
    lv_obj_set_style_text_font(lv_obj_get_child(walk, 0), &lv_font_montserrat_14, 0);
    return;
  }
  if (v.result != NEG_TALKING) {  // the result card
    const bool won = v.result == NEG_WON || v.result == NEG_WON_FAVOR;
    if (won) {
      const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
      snprintf(buf, sizeof(buf), "%s IS YOURS", PLANETS[p].name);
      uiLabel(screen_root, buf, accent, nullptr, 158, 2);
      lv_obj_t *pl = lv_obj_create(screen_root);
      lv_obj_remove_style_all(pl);
      lv_obj_clear_flag(pl, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(pl, 72, 72);
      lv_obj_set_pos(pl, 233 - 36, 186);
      lv_obj_add_event_cb(pl, spinPlanetDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)p);
      trackAnimated(pl);
      const bool paid = v.favorTaken >= 0 && NEG_FAVOR_BANK[v.favorTaken].check == NEG_FAVOR_CREDITS;
      if (v.result == NEG_WON || paid) {
        uiLabel(screen_root, paid ? "Paid in full." : "Every answer landed. No strings attached.", UI_DIM, &lv_font_montserrat_14, 266, 0);
        snprintf(buf, sizeof(buf), "+%d VP WHILE YOU HOLD IT", planetVPNow(p, MY_SLOT));
      } else {
        uiLabel(screen_root, "You owe them:", UI_DIM, &lv_font_montserrat_14, 264, 0);
        snapshotFavorText(gs, MY_SLOT, v.favorTaken, p, buf, sizeof(buf));
        uiWrapLabel(screen_root, buf, 0xFFD166, &sf_font_montserrat_12, 284, 320);
        snprintf(buf, sizeof(buf), v.late ? "ITS %d VP COME WHEN YOU DO IT - UNDONE, THEY TAKE IT BACK" : "ITS %d VP COME WHEN YOU DO IT", planetVPNow(p, MY_SLOT));
      }
      uiLabel(screen_root, buf, accent, &sf_font_montserrat_12, 318, 1);
      uiRepRow(gs, 340);
      uiPill(screen_root, 143, 368, 180, 56, "CONTINUE", nullptr, accent, negContinueCb);
    } else {
      uiLabel(screen_root, v.result == NEG_CUT_OFF ? "THEY CUT YOU OFF" : "THEY END THE CALL", UI_RED, nullptr, 166, 2);
      snprintf(buf, sizeof(buf), "%s%s is closed to you this round.", v.result == NEG_CUT_OFF ? "That's twice you insulted them.\n" : "",
               PLANETS[p].name);
      uiWrapLabel(screen_root, buf, UI_TEXT, &lv_font_montserrat_14, 204, 330);
      uiWrapLabel(screen_root, v.losses >= NEG_LOSSES_CLOSED ? "They'll never negotiate with you here again."
                                                             : "Lose here once more and they'll never talk to you here again.",
                  UI_RED, &sf_font_montserrat_12, 258, 330);
      uiRepRow(gs, 300);
      uiPill(screen_root, 143, 368, 180, 56, "CONTINUE", nullptr, UI_CYAN, negContinueCb);
    }
    return;
  }

  if (v.question >= 0) {  // a question
    const NegQuestion &q = NEG_BANK[v.question];
    static const char *TIER_WORD[3] = { "EASY", "HARDER", "HARD" };
    snprintf(buf, sizeof(buf), "%s - QUESTION %d OF %d", q.own >= 0 ? "THEIR OWN QUESTION" : TIER_WORD[q.tier % 3], v.step + 1, v.count);
    uiLabel(screen_root, buf, (q.own >= 0 && f >= 0) ? FACTION_TEXT_COLOR[f] : UI_DIM, &sf_font_montserrat_12, 156, 2);
    negFillPlanet(q.q, p, buf, sizeof(buf));
    uiWrapLabel(screen_root, buf, UI_TEXT, &lv_font_montserrat_14, 174, 340);
    static const int ROW_Y3[3] = { 238, 294, 350 }, ROW_W3[3] = { 380, 360, 300 };
    static const int ROW_Y2[2] = { 252, 310 }, ROW_W2[2] = { 380, 360 };
    if (v.tipped) uiLabel(screen_root, "Your contact steers you away from one answer", UI_CYAN, &sf_font_montserrat_12, 232, 0);
    for (int i = 0; i < v.nReplies; i++) {
      negFillPlanet(q.reply[v.replies[i]], p, buf, sizeof(buf));
      negRow(v.nReplies == 3 ? ROW_Y3[i] : ROW_Y2[i], v.nReplies == 3 ? ROW_W3[i] : ROW_W2[i], 50, buf, UI_CYAN, v.replies[i]);
    }
    lv_obj_t *leave = uiPill(screen_root, 173, 406, 120, 40, "LEAVE", nullptr, UI_DIM, askLeaveNegotiationCb);
    lv_obj_set_style_text_font(lv_obj_get_child(leave, 0), &sf_font_montserrat_12, 0);
    return;
  }

  // The favor stage: not convinced -- what will you do for this planet?
  // v0.30.1: drawn from the favor bank; the last rounds = Credits now (gold) or a deed due when the war ends.
  const uint32_t OWE = 0xFF9B70, GOLD = 0xFFD166;
  uiLabel(screen_root, v.hard ? "THEY HAVE ONE CONDITION" : "THEY'RE NOT CONVINCED YET", OWE, &sf_font_montserrat_12, 156, 2);
  negFillPlanet(v.hard ? "Then this is our price for {planet}. Take it or leave it."
                       : "Words are cheap, commander. What will you do for {planet}?", p, buf, sizeof(buf));
  uiWrapLabel(screen_root, buf, UI_TEXT, &lv_font_montserrat_14, 172, 340);
  if (gs.takeoverResult[MY_SLOT][0]) uiLabel(screen_root, gs.takeoverResult[MY_SLOT], UI_RED, &sf_font_montserrat_12, 208, 0);
  else if (v.late) {
    char left[24];
    if (v.roundsLeft == 0) snprintf(left, sizeof(left), "THE LAST ROUND");
    else snprintf(left, sizeof(left), "%d ROUND%s LEFT", v.roundsLeft, v.roundsLeft == 1 ? "" : "S");
    snprintf(buf, sizeof(buf), "WAR'S END: %s - UNDONE = THEY TAKE IT BACK", left);
    uiLabel(screen_root, buf, GOLD, &sf_font_montserrat_12, 208, 0);
  } else uiLabel(screen_root, "YOU'LL OWE WHAT YOU PICK - 3 ROUNDS - THE GAME CHECKS IT", OWE, &sf_font_montserrat_12, 208, 0);
  static const int FAV_Y[3] = { 226, 284, 342 }, FAV_W[3] = { 380, 360, 300 };
  for (int i = 0; i < v.nFavors; i++) {
    snapshotFavorText(gs, MY_SLOT, v.favors[i], p, buf, sizeof(buf));
    negRow(FAV_Y[i], FAV_W[i], 52, buf, NEG_FAVOR_BANK[v.favors[i]].check == NEG_FAVOR_CREDITS ? GOLD : OWE, i);
  }
  lv_obj_t *no = uiPill(screen_root, 143, 400, 180, 38, "NOTHING MORE", nullptr, UI_DIM, nullptr);
  lv_obj_set_style_text_font(lv_obj_get_child(no, 0), &sf_font_montserrat_12, 0);
  lv_obj_add_event_cb(no, negAnswerCb, LV_EVENT_CLICKED, (void *)(intptr_t)NEG_REFUSE);
}

// v0.20.2: walking away mid-conversation is a real decision -- a confirm that
// spells out the cost (v0.30.0: it also counts as a loss there).
static void stayInNegotiationCb(lv_event_t *e) {
  (void)e;
  showNegotiate();
}

static void askLeaveNegotiationCb(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_NEGOTIATE;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  const int p = gs.takeoverPlanet[MY_SLOT] >= 0 ? gs.takeoverPlanet[MY_SLOT] : currentPlanetIdx;
  uiAddRim(gs.roundLimit, gs.round, UI_RED);

  uiLabel(screen_root, "LEAVE?", UI_RED, nullptr, 72, 6);
  uiLabel(screen_root, PLANETS[p].name, UI_DIM, &lv_font_montserrat_14, 104, 2);
  char msg[160];
  snprintf(msg, sizeof(msg), "Walking out counts as a loss here,\nand they won't forget the snub.\n\nYou can't try another approach\non %s this round.", PLANETS[p].name);
  uiLabel(screen_root, msg, UI_TEXT, &lv_font_montserrat_14, 140, 0);

  // Corners: LEAVE (123..343, 250..322) ~141px; STAY (123..343, 336..408) ~207px.
  uiPill(screen_root, 123, 250, 220, 72, "LEAVE", nullptr, UI_RED, cancelTakeoverCb);
  uiMarkBack(uiPill(screen_root, 123, 336, 220, 72, "STAY", nullptr, UI_CYAN, stayInNegotiationCb), false);  // edge swipe = STAY
}

// ---------------------------------------------------------------------------
// PVP (v0.21.10 restyle): ATTACK (Home, Resolution) -> your fleet (Fleets
// screen in attack mode) -> opponent -> their fleet -> the Battle Forecast
// in PvP mode (plain d6 both sides) -> ENGAGE. A refusal (that fleet
// already fought, a battle is running...) shows in red on the forecast.
// Rows: 300x72 pills at y 124/208/292 (far corners ~199px); back circle 64
// at (233,410) (edge ~209px).
// ---------------------------------------------------------------------------
static int fleetAlive(const Player &pl, int f) {
  int n = 0;
  for (int s = 0; s < FLEET_SHIPS; s++) if (pl.fleetShipAlive[f][s]) n++;
  return n;
}

// One screen-wide row: title (FLEET n / a faction), a status line, and an
// optional ship strip on the right. Returns the button.
static lv_obj_t *uiRow(int y, const char *title, const char *status, uint32_t border, uint32_t titleColor,
                       int ships, uint32_t shipColor, lv_event_cb_t cb, void *ud) {
  lv_obj_t *btn = lv_btn_create(screen_root);
  uiStyleButton(btn, border, 2, cb ? uiTint(border) : UI_BG, 36);  // style first (Convention 13)
  lv_obj_set_size(btn, 300, 72);
  lv_obj_set_pos(btn, 83, y);
  lv_obj_t *t = lv_label_create(btn);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_color(t, lv_color_hex(titleColor), 0);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 24, -10);
  lv_obj_t *s = lv_label_create(btn);
  lv_label_set_text(s, status);
  lv_obj_set_style_text_font(s, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s, lv_color_hex(cb ? UI_DIM : UI_FAINT), 0);
  lv_obj_align(s, LV_ALIGN_LEFT_MID, 24, 14);
  if (ships > 0) {
    lv_obj_t *u = lv_obj_create(btn);
    lv_obj_remove_style_all(u);
    lv_obj_clear_flag(u, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(u, 120, 20);
    lv_obj_align(u, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_add_event_cb(u, forecastUnitsDrawCb, LV_EVENT_DRAW_MAIN,
                        (void *)(uintptr_t)(((uint32_t)UNITS_SHIPS << 30) | ((uint32_t)(ships & 0x3F) << 24) | (shipColor & 0xFFFFFF)));
  }
  if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
  else lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
  return btn;
}

static void uiSmallBack(lv_event_cb_t cb) {
  lv_obj_t *back = lv_btn_create(screen_root);
  uiStyleButton(back, UI_LINE, 1, UI_BG, LV_RADIUS_CIRCLE);
  lv_obj_set_size(back, 64, 64);
  lv_obj_set_pos(back, 233 - 32, 410 - 32);
  lv_obj_t *bl = lv_label_create(back);
  lv_label_set_text(bl, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(bl, lv_color_hex(UI_DIM), 0);
  lv_obj_center(bl);
  lv_obj_add_event_cb(back, cb, LV_EVENT_CLICKED, nullptr);
  uiMarkBack(back);
}

static void pvpTargetTapCb(lv_event_t *e) {
  pvpTargetSlot = (int)(intptr_t)lv_event_get_user_data(e);
  showPvpFleetPick();
}

static void backToFleetsCb(lv_event_t *e) { showFleets(e); }

static void showPvpTargets(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_FLEETS;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  uiAddRim(gs.roundLimit, gs.round, UI_RED);
  uiLabel(screen_root, "ATTACK WHO?", UI_RED, nullptr, 60, 4);
  char sub[40];
  snprintf(sub, sizeof(sub), "Fleet %d is ready to launch", pvpAttackerFleet + 1);
  uiLabel(screen_root, sub, UI_DIM, &lv_font_montserrat_14, 92, 0);

  int y = 124, shown = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) {
    if (!gs.players[i].active || i == MY_SLOT) continue;
    const int f = gs.players[i].chosenFaction;
    const uint32_t col = (f >= 0 && f < 4) ? FACTION_RING_COLOR[f] : UI_LINE;
    int ships = 0, fleets = 0;
    for (int k = 0; k < NUM_FLEETS; k++) { int a = fleetAlive(gs.players[i], k); ships += a; if (a) fleets++; }
    char st[40];
    snprintf(st, sizeof(st), "%d ship%s in %d fleet%s", ships, ships == 1 ? "" : "s", fleets, fleets == 1 ? "" : "s");
    lv_obj_t *row = uiRow(y, (f >= 0 && f < 4) ? FACTION_SHORT[f] : gs.players[i].name, st, col,
                          (f >= 0 && f < 4) ? FACTION_TEXT_COLOR[f] : UI_TEXT, 0, 0,
                          ships > 0 ? pvpTargetTapCb : nullptr, (void *)(intptr_t)i);
    if (f >= 0 && f < 4) {
      lv_obj_t *ins = uiInsignia(row, f, 36, uiTint(col));
      lv_obj_align(ins, LV_ALIGN_RIGHT_MID, -22, 0);
    }
    y += 84;
    shown++;
  }
  if (shown == 0) uiLabel(screen_root, "No other commanders have joined.", UI_DIM, &lv_font_montserrat_14, 200, 0);
  uiSmallBack(backToFleetsCb);
}

static void pvpDefFleetTapCb(lv_event_t *e) {
  pvpDefenderFleet = (int)(intptr_t)lv_event_get_user_data(e);
  forecast_pvp = true;
  briefing_fleet = 1 << pvpAttackerFleet;  // v0.28.0 mask: more fleets can be toggled in on the forecast
  pvp_refusal = nullptr;
  showBriefing();
}

static void backToPvpTargetsCb(lv_event_t *e) {
  (void)e;
  showPvpTargets(nullptr);
}

static void showPvpFleetPick() {
  currentScreen = SCR_FLEETS;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  uiAddRim(gs.roundLimit, gs.round, UI_RED);
  const Player &them = gs.players[pvpTargetSlot];
  const int f = them.chosenFaction;
  const uint32_t col = (f >= 0 && f < 4) ? FACTION_RING_COLOR[f] : UI_LINE;
  char title[40];
  snprintf(title, sizeof(title), "%s FLEETS", (f >= 0 && f < 4) ? FACTION_SHORT[f] : them.name);
  uiLabel(screen_root, title, (f >= 0 && f < 4) ? FACTION_TEXT_COLOR[f] : UI_TEXT, nullptr, 60, 3);
  uiLabel(screen_root, "Which one do you strike?", UI_DIM, &lv_font_montserrat_14, 92, 0);
  for (int k = 0; k < NUM_FLEETS; k++) {
    const int a = fleetAlive(them, k);
    char t[16], st[32];
    snprintf(t, sizeof(t), "FLEET %d", k + 1);
    if (a > 0) snprintf(st, sizeof(st), "%d ship%s", a, a == 1 ? "" : "s");
    else snprintf(st, sizeof(st), "%s", them.fleetShipCount[k] > 0 ? "destroyed" : "not built");
    uiRow(124 + k * 84, t, st, a > 0 ? col : UI_RING, a > 0 ? UI_TEXT : UI_FAINT, a, col,
          a > 0 ? pvpDefFleetTapCb : nullptr, (void *)(intptr_t)k);
  }
  uiSmallBack(backToPvpTargetsCb);
}

static void pvpEngageCb(lv_event_t *e) {
  (void)e;
  // Only enter the battle screen if the attack actually started (v0.20.2).
  pvp_refusal = gameStateStartPvpBattle(MY_SLOT, briefing_fleet, pvpTargetSlot, pvpDefenderFleet);
  if (!pvp_refusal) { forecast_pvp = false; showBattle(); }
  else showBriefing();
}

static void backFromPvpForecastCb(lv_event_t *e) {
  (void)e;
  forecast_pvp = false;
  showPvpFleetPick();
}

// ---------------------------------------------------------------------------
// FACTION TRANSMISSION -- the reputation "face moment"
//
// Standalone and self-contained on purpose: this is being tuned for feel
// BEFORE real reputation tier-crossing logic exists in game_state. Trigger
// it manually from Home (TEST COMMS button) to test timing/feel. Once the
// real reputation system is built, the same showFactionTransmission() call
// gets wired to actual tier-crossing events instead of a test button.
//
// ---------------------------------------------------------------------------

static lv_obj_t *transmission_body_label = nullptr;
static lv_obj_t *transmission_prompt_label = nullptr;
static lv_obj_t *transmission_shake_layer = nullptr;
static lv_timer_t *transmission_type_timer = nullptr;
static char transmission_full_text[220] = "";
static int transmission_reveal_len = 0;
static lv_obj_t *transmission_obligation_yes_btn = nullptr;
static lv_obj_t *transmission_obligation_no_btn = nullptr;

static lv_obj_t *transmission_static_overlay = nullptr;
static lv_timer_t *transmission_static_timer = nullptr;
static int transmission_static_ticks_left = 0;

// Cheap deterministic pseudo-random hash (classic "sin-noise" trick) --
// same shake pattern every time, which is fine for a repeatable UI beat.
static float shakeNoise(int32_t seed) {
  float x = sinf((float)seed * 12.9898f) * 43758.5453f;
  return x - floorf(x);  // fract() -> 0..1
}

static void shakeAnimExec(void *obj, int32_t v) {
  lv_obj_t *o = (lv_obj_t *)obj;
  float t = (float)v / 100.0f;         // 0..1 over the animation
  float decay = (1.0f - t) * (1.0f - t);  // squared falloff: front-loaded, punchy
  float nx = shakeNoise(v) * 2.0f - 1.0f;       // -1..1
  float ny = shakeNoise(v + 1000) * 2.0f - 1.0f;  // different seed offset than X
  int offsetX = (int)(nx * 18.0f * decay);
  int offsetY = (int)(ny * 10.0f * decay);
  lv_obj_set_style_translate_x(o, offsetX, 0);
  lv_obj_set_style_translate_y(o, offsetY, 0);
}

static void shakeAnimReady(lv_anim_t *a) {
  (void)a;
  if (transmission_shake_layer) {
    lv_obj_set_style_translate_x(transmission_shake_layer, 0, 0);
    lv_obj_set_style_translate_y(transmission_shake_layer, 0, 0);
  }
}

static void startScreenShake() {
  if (!transmission_shake_layer) return;
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, transmission_shake_layer);
  lv_anim_set_values(&a, 0, 100);
  lv_anim_set_time(&a, 260);  // faster: was 450ms, now a quick hit
  lv_anim_set_exec_cb(&a, shakeAnimExec);
  lv_anim_set_ready_cb(&a, shakeAnimReady);
  lv_anim_start(&a);
}

// Blocky "signal static" burst that plays right as the transmission opens,
// before the shake/typewriter kick in -- like tuning into a frequency. Uses
// the same live-redraw technique as the star field (a custom draw callback
// re-randomizing on each redraw) rather than a full-screen pixel buffer,
// which would be far too much RAM to spare alongside WiFi/display buffers.
// v0.23.4 diagnostics: timed wrapper (see [UI] 5s draw ms).
static void staticDrawCbImpl(lv_event_t *e);
static void staticDrawCb(lv_event_t *e) { const uint32_t t0 = micros(); staticDrawCbImpl(e); g_uiDrawUs[UI_T_STATIC] += micros() - t0; }
static void staticDrawCbImpl(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t area;
  lv_obj_get_coords(obj, &area);

  const int cell = 26;
  lv_draw_rect_dsc_t dsc;
  for (int y = area.y1; y < area.y2; y += cell) {
    if (!layerSees(layer, area.x1, y, area.x2, y + cell - 1)) continue;
    for (int x = area.x1; x < area.x2; x += cell) {
      if (!layerSees(layer, x, y, x + cell - 1, y + cell - 1)) continue;
      lv_draw_rect_dsc_init(&dsc);
      uint8_t v = esp_random() % 4;
      uint32_t color = (v == 0) ? 0x0A0F16 : (v == 1) ? 0x3A5568 : (v == 2) ? 0x8CA5B8 : 0xE8F4FF;
      dsc.bg_color = lv_color_hex(color);
      dsc.bg_opa = LV_OPA_COVER;
      lv_area_t a = { (lv_coord_t)x, (lv_coord_t)y, (lv_coord_t)(x + cell - 1), (lv_coord_t)(y + cell - 1) };
      lv_draw_rect(layer, &dsc, &a);
    }
  }
}

// Ends the static burst (whether it ran out naturally or was skipped by a
// tap) and kicks off the shake + typewriter that follow it.
static void factionTypeTick(lv_timer_t *timer);

static void finishStaticBurst() {
  if (transmission_static_timer) {
    lv_timer_del(transmission_static_timer);
    transmission_static_timer = nullptr;
  }
  if (transmission_static_overlay) {
    lv_obj_del(transmission_static_overlay);
    transmission_static_overlay = nullptr;
  }
  startScreenShake();
  if (transmission_type_timer) lv_timer_del(transmission_type_timer);
  transmission_type_timer = lv_timer_create(factionTypeTick, 28, nullptr);
}

static void staticTickCb(lv_timer_t *timer) {
  (void)timer;
  transmission_static_ticks_left--;
  if (transmission_static_overlay) lv_obj_invalidate(transmission_static_overlay);
  if (transmission_static_ticks_left <= 0) finishStaticBurst();
}

static void revealTransmissionPrompt() {
  if (transmission_is_obligation) {
    if (transmission_obligation_yes_btn) lv_obj_clear_flag(transmission_obligation_yes_btn, LV_OBJ_FLAG_HIDDEN);
    if (transmission_obligation_no_btn) lv_obj_clear_flag(transmission_obligation_no_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    if (transmission_prompt_label) lv_obj_clear_flag(transmission_prompt_label, LV_OBJ_FLAG_HIDDEN);
  }
}

static void factionTypeTick(lv_timer_t *timer) {
  (void)timer;
  int total = strlen(transmission_full_text);
  if (transmission_reveal_len < total) {
    transmission_reveal_len++;
    char buf[220];
    snprintf(buf, sizeof(buf), "%.*s", transmission_reveal_len, transmission_full_text);
    if (transmission_body_label) lv_label_set_text(transmission_body_label, buf);
  } else {
    if (transmission_type_timer) {
      lv_timer_del(transmission_type_timer);
      transmission_type_timer = nullptr;
    }
    revealTransmissionPrompt();
  }
}

static void transmissionTapCb(lv_event_t *e) {
  (void)e;
  if (transmission_static_overlay) {
    // Tapped during the static burst -- skip straight to the transmission.
    finishStaticBurst();
    return;
  }
  int total = strlen(transmission_full_text);
  if (transmission_reveal_len < total) {
    // First tap while typing: fast-forward to the full line.
    transmission_reveal_len = total;
    if (transmission_body_label) lv_label_set_text(transmission_body_label, transmission_full_text);
    if (transmission_type_timer) {
      lv_timer_del(transmission_type_timer);
      transmission_type_timer = nullptr;
    }
    revealTransmissionPrompt();
    return;
  }
  // Typing already finished. In obligation mode, a generic tap doesn't do
  // anything -- you have to actually answer via the two buttons below.
  if (transmission_is_obligation) return;
  showHome();
}

static void obligationFulfilledCb(lv_event_t *e) {
  (void)e;
  gameStateResolveObligation(MY_SLOT, true);
  showHome();
}

static void obligationBrokenCb(lv_event_t *e) {
  (void)e;
  gameStateResolveObligation(MY_SLOT, false);
  showHome();
}

// v0.21.21 hologram transmission (mockup "Hologram transmission"): the
// faction's speaker -- commanderVoiceFor(): its first commander unless
// that's YOUR commander -- as a 5x hologram portrait in an emitter frame,
// with an occasional flicker; name chip in the faction color; the typed
// quote. Static burst / shake / typewriter unchanged. The old placeholder
// box + letter (and its two transform_scale labels) are gone.
static lv_obj_t *transmission_holo_img = nullptr;
static lv_timer_t *transmission_flicker_timer = nullptr;

static void transmissionFlickerTick(lv_timer_t *timer) {
  (void)timer;
  if (currentScreen != SCR_TRANSMISSION || !transmission_holo_img) {
    lv_timer_del(transmission_flicker_timer);
    transmission_flicker_timer = nullptr;
    transmission_holo_img = nullptr;
    return;
  }
  // Mostly steady; now and then a quick dip, like the mockup's flicker.
  const uint32_t r = esp_random() % 100;
  lv_obj_set_style_opa(transmission_holo_img, r < 8 ? LV_OPA_50 : (r < 16 ? LV_OPA_80 : LV_OPA_90), 0);
}

// Projection beam under the frame: two faint lines fanning down + the
// emitter ring.
static void holoBeamDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  uiLine(layer, 173, 206, 128, 440, lv_color_hex(0x7FE3FF), 1);
  uiLine(layer, 293, 206, 338, 440, lv_color_hex(0x7FE3FF), 1);
}

static int tx_cause = -1;  // v0.28.3: the shown line's cause (PROMISE KEPT / BROKEN title)
static void showFactionTransmission(int factionIdx, const char *quote, int fromTier, int toTier, bool big, int cause, int delta, int myCmd) {
  tx_cause = cause;
  sfxPlay(SFX_CHIRP);  // v0.25.3 (a big moment's fanfare already plays from sfxWatch)
  if (factionIdx < 0 || factionIdx > 3) factionIdx = 0;
  currentScreen = SCR_TRANSMISSION;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  // v0.30.2: myCmd comes from the caller -- reading it here put a second ~4 KB
  // GameState on the LVGL stack under every caller's own copy (see showPendingTransmission).
  const int voice = commanderVoiceFor(factionIdx, myCmd);

  // Everything for this screen lives inside a dedicated full-screen
  // container so the shake animation has a normal object to transform,
  // rather than the LVGL screen object itself (which doesn't reliably
  // animate given this project's partial-buffer rendering setup).
  transmission_shake_layer = lv_obj_create(screen_root);
  lv_obj_remove_style_all(transmission_shake_layer);
  lv_obj_set_size(transmission_shake_layer, 466, 466);
  lv_obj_set_pos(transmission_shake_layer, 0, 0);
  lv_obj_t *layer = transmission_shake_layer;

  // Full-screen invisible tap target. Not SCROLLABLE: a scrollable target
  // turned slightly-moving taps into drags and swallowed CLICKED (the old
  // intermittent "tap to continue isn't working").
  lv_obj_t *tapCatcher = lv_obj_create(layer);
  lv_obj_remove_style_all(tapCatcher);
  lv_obj_set_size(tapCatcher, 466, 466);
  lv_obj_set_pos(tapCatcher, 0, 0);
  lv_obj_add_flag(tapCatcher, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(tapCatcher, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(tapCatcher, transmissionTapCb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *beam = lv_obj_create(layer);
  lv_obj_remove_style_all(beam);
  lv_obj_set_size(beam, 466, 466);
  lv_obj_set_pos(beam, 0, 0);
  lv_obj_clear_flag(beam, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(beam, LV_OPA_20, 0);
  lv_obj_add_event_cb(beam, holoBeamDrawCb, LV_EVENT_DRAW_MAIN, nullptr);

  // v0.23.0: big moments come in gold as PRIORITY; a tier change gets its
  // own line ("NEUTRAL > FAVORABLE") under the title -- the quote never says it.
  if (!big && tx_cause == TXC_KEPT) uiLabel(layer, "PROMISE KEPT", 0x8CE7A0, &sf_font_montserrat_12, 28, 3);  // v0.28.3
  else if (!big && tx_cause == TXC_BROKE) uiLabel(layer, "PROMISE BROKEN", UI_RED, &sf_font_montserrat_12, 28, 3);
  else uiLabel(layer, big ? "PRIORITY TRANSMISSION" : "INCOMING TRANSMISSION", big ? 0xFFC933 : 0x62C7F2, &sf_font_montserrat_12, 28, 3);
  const bool tierLine = fromTier >= 0 && toTier >= 0 && fromTier != toTier && fromTier <= 4 && toTier <= 4;
  const int dy = (tierLine || delta) ? 10 : 0;  // everything below moves down to make room
  // v0.29.0: the arrows for how far your standing moved, then the tier change beside them.
  const int aw = repArrowsWidth(delta);
  int tw = 0;
  char t[48] = "";
  if (tierLine) {
    snprintf(t, sizeof(t), "%s  >  %s", REP_TIER_NAMES[fromTier], REP_TIER_NAMES[toTier]);
    tw = uiTextWidth(t, &sf_font_montserrat_12, 2);
  }
  const int lineW = aw + (aw && tw ? 10 : 0) + tw, x0 = 233 - lineW / 2;
  if (aw) uiRepArrows(layer, x0, 49, delta);
  if (tierLine) {
    lv_obj_t *tl = uiLabel(layer, t, repTierColor(toTier), &sf_font_montserrat_12, 46, 2);
    if (aw) { lv_obj_set_align(tl, LV_ALIGN_TOP_LEFT); lv_obj_set_pos(tl, x0 + aw + 10, 46); }
  }

  // Emitter frame + hologram portrait (120x140, 5x art).
  lv_obj_t *frame = lv_obj_create(layer);
  uiStyleButton(frame, 0x7FE3FF, 1, 0x061621, 22);  // style first (Convention 13)
  lv_obj_set_style_border_opa(frame, LV_OPA_50, 0);
  lv_obj_set_style_clip_corner(frame, true, 0);
  lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(frame, 140, 152);
  lv_obj_set_pos(frame, 163, 52 + dy);
  transmission_holo_img = uiPortrait(frame, voice, CMD_IMG_HOLO);
  lv_obj_align(transmission_holo_img, LV_ALIGN_BOTTOM_MID, 0, 2);
  lv_obj_set_style_opa(transmission_holo_img, LV_OPA_90, 0);
  lv_obj_t *ring = lv_obj_create(layer);
  uiStyleButton(ring, 0x7FE3FF, 2, UI_BG, LV_RADIUS_CIRCLE);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(ring, LV_OPA_40, 0);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(ring, 128, 20);
  lv_obj_set_pos(ring, 233 - 64, 206 + dy);
  if (!transmission_flicker_timer) transmission_flicker_timer = lv_timer_create(transmissionFlickerTick, 140, nullptr);

  // Name chip: "GEN. TOBIN MARR - REBELS", in the faction color.
  char who[56];
  snprintf(who, sizeof(who), "%s - %s", voice >= 0 ? COMMANDER_INFO[voice].name : "", FACTION_SHORT[factionIdx]);
  lv_obj_t *chip = lv_obj_create(layer);
  uiStyleButton(chip, FACTION_RING_COLOR[factionIdx], 1, UI_BG, 12);
  lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(chip, LV_SIZE_CONTENT, 26);
  lv_obj_set_style_pad_hor(chip, 12, 0);
  lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 234 + dy);
  lv_obj_t *chipL = lv_label_create(chip);
  lv_label_set_text(chipL, who);
  lv_obj_set_style_text_font(chipL, &sf_font_montserrat_12, 0);
  lv_obj_set_style_text_letter_space(chipL, 2, 0);
  lv_obj_set_style_text_color(chipL, lv_color_hex(FACTION_TEXT_COLOR[factionIdx]), 0);
  lv_obj_center(chipL);

  // The quote, typed out. Obligation check-ins are longer and need room for
  // the two answer pills, so they use 14pt.
  transmission_body_label = lv_label_create(layer);
  lv_label_set_text(transmission_body_label, "");
  lv_label_set_long_mode(transmission_body_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(transmission_body_label, transmission_is_obligation ? 340 : 320);
  if (transmission_is_obligation) lv_obj_set_style_text_font(transmission_body_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(transmission_body_label, lv_color_hex(0xCFF4FF), 0);
  lv_obj_set_style_text_align(transmission_body_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(transmission_body_label, LV_ALIGN_TOP_MID, 0, 272 + dy);

  transmission_prompt_label = nullptr;
  transmission_obligation_yes_btn = nullptr;
  transmission_obligation_no_btn = nullptr;

  if (transmission_is_obligation) {
    // Corners: (86..226 / 240..380, 340..400) -> farthest ~222px.
    transmission_obligation_yes_btn = uiPill(layer, 86, 340, 140, 60, "I DID IT", nullptr, 0x8CE7A0, obligationFulfilledCb);
    lv_obj_add_flag(transmission_obligation_yes_btn, LV_OBJ_FLAG_HIDDEN);
    transmission_obligation_no_btn = uiPill(layer, 240, 340, 140, 60, "I DIDN'T", nullptr, UI_RED, obligationBrokenCb);
    lv_obj_add_flag(transmission_obligation_no_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    transmission_prompt_label = uiLabel(layer, "TAP TO CLOSE", 0x3D6275, &sf_font_montserrat_12, 404 + dy, 2);
    lv_obj_add_flag(transmission_prompt_label, LV_OBJ_FLAG_HIDDEN);
  }

  // Store the quote for the typewriter (starts once the static burst below
  // clears); everything is fully built now, just covered by static.
  snprintf(transmission_full_text, sizeof(transmission_full_text), "%s", quote);
  transmission_reveal_len = 0;
  if (transmission_type_timer) {
    lv_timer_del(transmission_type_timer);
    transmission_type_timer = nullptr;
  }

  // Static burst -- "tuning into" the transmission. Sits on top of the
  // already-built content (last child = highest z-order) and clears into
  // the shake + typewriter once it finishes.
  transmission_static_overlay = lv_obj_create(layer);
  lv_obj_remove_style_all(transmission_static_overlay);
  lv_obj_set_size(transmission_static_overlay, 466, 466);
  lv_obj_set_pos(transmission_static_overlay, 0, 0);
  lv_obj_add_event_cb(transmission_static_overlay, staticDrawCb, LV_EVENT_DRAW_MAIN, nullptr);

  transmission_static_ticks_left = big ? 16 : 6;  // ~270ms of static; a big moment ~720ms
  if (transmission_static_timer) lv_timer_del(transmission_static_timer);
  transmission_static_timer = lv_timer_create(staticTickCb, 45, nullptr);
}

// Checks whether a real reputation-driven transmission is pending for this
// player and shows it if so. Called right after actions that can move
// reputation (conquest, Pay, Negotiate accept/decline), instead of jumping
// straight to the next screen -- so the fallout of what you just did
// actually interrupts you, the way it's meant to. Returns true if a
// transmission was shown (caller should skip its normal navigation).
static bool checkAndShowTransmission(const GameState &gs) {
  if (MY_SLOT < 0) return false;  // not seated (Start/join screens, or the table host) -- index -1 before v0.22.0
  if (gs.transmissionPending[MY_SLOT]) {
    int faction = gs.transmissionFaction[MY_SLOT];
    bool isObligation = gs.transmissionIsObligation[MY_SLOT];
    char quoteCopy[220];
    snapshotTransmissionText(gs, MY_SLOT, quoteCopy, sizeof(quoteCopy));  // v0.23.0: from the bank
    const TxMsg &head = gs.txQueue[MY_SLOT][0];
    if (isObligation) {
      // Do NOT clear yet -- gameStateResolveObligation() needs
      // transmissionObligationIdx to still be set when the player answers
      // via the YES/NO buttons. Clearing here would lose which obligation
      // this check-in refers to.
      transmission_is_obligation = true;
    } else {
      transmission_is_obligation = false;
      gameStateClearTransmission(MY_SLOT);
    }
    showFactionTransmission(faction, quoteCopy, head.fromTier, head.toTier, head.big != 0, head.cause, head.delta, gs.players[MY_SLOT].commander);
    return true;
  }
  return false;
}

// v0.30.2 (playtest 2026-10-28: the 1.75" froze on the battle report's
// CONTINUE): CONTINUE handlers call this instead of holding their own
// GameState while the next screen reads another. Its copy is gone by the
// time the caller builds a fallback screen. Two copies (~8 KB) plus the
// transmission screen ran the 16 KB LVGL stack out (~10 KB spare at idle).
static bool showPendingTransmission() {
  GameState gs = gameStateRead();
  return checkAndShowTransmission(gs);
}

// ---------------------------------------------------------------------------
// FLEETS (static roster for now)
// ---------------------------------------------------------------------------
static void fleetTapCb(lv_event_t *e) {
  currentFleetIdx = (int)(intptr_t)lv_event_get_user_data(e);
  showFleetDetail();
}

static void fleetAttackTapCb(lv_event_t *e) {
  pvpAttackerFleet = (int)(intptr_t)lv_event_get_user_data(e);
  showPvpTargets(nullptr);
}

// v0.27.3 ATTACK WHAT? (playtest 2026-10-16: "attack is confusing in resolution,
// because it only shows players, when people click that for attacking
// planets"). Resolution's ATTACK circle first asks: a PLANET (the Galaxy
// map -- tap one, FIGHT, the forecast picks the fleet) or a PLAYER (pick a
// ready fleet, then a rival, as before). Rows 300x72 at y 150 / 250.
static void attackPlanetCb(lv_event_t *e) { showPlanets(e); }
static void attackPlayerCb(lv_event_t *e) { showFleets(e); }

static void showAttackChoice(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_FLEETS;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();
  GameState gs = gameStateRead();
  const Player &me = gs.players[MY_SLOT];
  uiAddRim(gs.roundLimit, gs.round, UI_RED);
  uiLabel(screen_root, "ATTACK", UI_RED, nullptr, 60, 4);
  int ready = 0;
  for (int f = 0; f < NUM_FLEETS; f++) if (fleetAlive(me, f) > 0 && !me.fleetSpent[f]) ready++;
  char sub[40];
  if (ready) snprintf(sub, sizeof(sub), "%d fleet%s ready", ready, ready == 1 ? "" : "s");
  else snprintf(sub, sizeof(sub), "No fleet is ready this round");
  uiLabel(screen_root, sub, UI_DIM, &lv_font_montserrat_14, 92, 0);
  // 2.0: rows at y 130 / 215 / 300 when the Ashen Fleet is a target too (farthest corner ~204 px).
  const bool ashen = snapshotAshenAlive(gs);
  const int y0 = ashen ? 130 : 150, step = ashen ? 85 : 100;
  uiRow(y0, "A PLANET", "Pick it on the galaxy map", ready ? UI_RED : UI_RING, ready ? UI_TEXT : UI_FAINT, 0, 0,
        ready ? attackPlanetCb : nullptr, nullptr);
  int rivals = 0;
  for (int i = 0; i < MAX_PLAYERS; i++) if (gs.players[i].active && i != MY_SLOT) rivals++;
  uiRow(y0 + step, "A PLAYER", rivals ? "Choose a fleet, then a rival" : "No other commanders", ready && rivals ? UI_RED : UI_RING,
        ready && rivals ? UI_TEXT : UI_FAINT, 0, 0, ready && rivals ? attackPlayerCb : nullptr, nullptr);
  if (ashen) {
    char hp[40];
    snprintf(hp, sizeof(hp), "A joint strike - %d / %d HP left", gs.ashenHp, gs.ashenMaxHp);
    uiRow(y0 + 2 * step, "THE ASHEN FLEET", hp, ready ? 0xFF8C42 : UI_RING, ready ? 0xFF8C42 : UI_FAINT, 0, 0, ready ? showAshen : nullptr, nullptr);
  }
  uiSmallBack(backToHome);
}

// FLEETS (v0.21.10 restyle). In Resolution this IS the attack screen (Home's
// FLEETS circle turns into a red ATTACK circle): tap a ready fleet to send
// it at a rival. Other phases: tap a fleet for its ships / investing.
static void showFleets(lv_event_t *e) {
  (void)e;
  currentScreen = SCR_FLEETS;
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  GameState gs = gameStateRead();
  const Player &me = gs.players[MY_SLOT];
  const bool attack = gs.phase == PHASE_RESOLUTION;
  const uint32_t accent = uiAccentFor(me.chosenFaction);
  uiAddRim(gs.roundLimit, gs.round, attack ? UI_RED : accent);
  uiLabel(screen_root, attack ? "ATTACK" : "FLEETS", attack ? UI_RED : accent, nullptr, 60, 4);
  uiLabel(screen_root, attack ? "Pick a fleet to send in" : "Tap a fleet for its ships", UI_DIM,
          &lv_font_montserrat_14, 92, 0);

  for (int f = 0; f < NUM_FLEETS; f++) {
    const int alive = fleetAlive(me, f);
    const int total = me.fleetShipCount[f];
    const bool ready = alive > 0 && !me.fleetSpent[f];
    char t[16], st[40];
    snprintf(t, sizeof(t), "FLEET %d", f + 1);
    if (alive > 0 && attack && ready) snprintf(st, sizeof(st), "ready - %d ship%s", alive, alive == 1 ? "" : "s");
    else if (alive > 0 && attack) snprintf(st, sizeof(st), "spent this round");
    else if (alive > 0) snprintf(st, sizeof(st), "%d / %d ships", alive, total);
    else snprintf(st, sizeof(st), "%s", total > 0 ? "destroyed" : "not built");
    lv_event_cb_t cb = attack ? (ready ? fleetAttackTapCb : nullptr) : fleetTapCb;
    uint32_t border = attack ? (ready ? UI_RED : UI_RING) : (alive > 0 ? accent : UI_LINE);
    uiRow(124 + f * 84, t, st, border, (cb && (!attack || ready)) ? UI_TEXT : UI_FAINT, alive, accent,
          cb, (void *)(intptr_t)f);
  }
  uiSmallBack(attack ? showAttackChoice : backToHome);  // v0.27.3: back to ATTACK WHAT?
}

static void investFleetCb(lv_event_t *e) {
  (void)e;
  gameStateInvestFleet(MY_SLOT, currentFleetIdx);
  showFleetDetail();
}

// FLEET DETAIL (v0.25.1 restyle -- was a text roster): six ship shapes in your
// faction color (destroyed = red cross, not built = faint outline) with their
// names, the fleet's status + your Credits on one line, then INVEST / FOUND /
// REBUILD as a pill, or the "control a planet" hint. Touch: pill 300x64 at
// (83,316) (far corner ~210px); back circle at (233,408) (edge ~211px).
struct FleetShipView { int state; uint32_t color; };  // state: 0 not built, 1 alive, 2 destroyed
static FleetShipView fleet_ship_view[FLEET_SHIPS];

static void fleetShipDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t a;
  lv_obj_get_coords(obj, &a);
  const FleetShipView &v = fleet_ship_view[(int)(intptr_t)lv_event_get_user_data(e)];
  const int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
  const lv_point_precise_t P[3] = { { (lv_value_precise_t)(cx + 22), (lv_value_precise_t)cy },
                                    { (lv_value_precise_t)(cx - 18), (lv_value_precise_t)(cy - 16) },
                                    { (lv_value_precise_t)(cx - 18), (lv_value_precise_t)(cy + 16) } };
  if (v.state == 1) {  // alive: a solid ship, pointing at the enemy
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    for (int i = 0; i < 3; i++) t.p[i] = P[i];
    t.color = lv_color_hex(v.color);
    lv_draw_triangle(layer, &t);
  } else {             // outline: faint if never built, red-crossed if destroyed
    const lv_color_t c = lv_color_hex(v.state == 2 ? 0x5A2A2A : 0x2A3B50);
    for (int i = 0; i < 3; i++)
      uiLine(layer, (int)P[i].x, (int)P[i].y, (int)P[(i + 1) % 3].x, (int)P[(i + 1) % 3].y, c, 2);
    if (v.state == 2) {
      uiLine(layer, cx - 14, cy - 14, cx + 14, cy + 14, lv_color_hex(UI_RED), 3);
      uiLine(layer, cx + 14, cy - 14, cx - 14, cy + 14, lv_color_hex(UI_RED), 3);
    }
  }
}

static void showFleetDetail() {
  onHomeScreen = false;
  onBattleScreen = false;
  clearScreen();

  GameState gs = gameStateRead();
  const Player &me = gs.players[MY_SLOT];
  const int f = currentFleetIdx;
  const uint32_t accent = uiAccentFor(me.chosenFaction);
  uiAddRim(gs.roundLimit, gs.round, accent);

  const int count = me.fleetShipCount[f];
  int alive = 0;
  for (int s = 0; s < FLEET_SHIPS; s++) if (me.fleetShipAlive[f][s]) alive++;
  const bool offBoard = alive == 0;  // never founded, or fully destroyed (v0.20.2)

  char buf[48];
  snprintf(buf, sizeof(buf), "FLEET %d", f + 1);
  uiLabel(screen_root, buf, accent, nullptr, 52, 4);
  const char *status = offBoard ? (count > 0 ? "DESTROYED" : "NOT FOUNDED")
                     : me.fleetSpent[f] ? "SPENT THIS ROUND" : "READY";
  snprintf(buf, sizeof(buf), "%s   -   %d CR", status, me.credits);
  uiLabel(screen_root, buf, offBoard ? UI_RED : UI_DIM, &sf_font_montserrat_12, 82, 2);

  // Two rows of three: ship shape + name (2.0-dev.20: the Empire's 7th starts a third row -- AMOLED pass).
  const int cap = snapshotFleetCapacity(gs, MY_SLOT);
  for (int s = 0; s < cap; s++) {
    fleet_ship_view[s].state = s >= count ? 0 : (me.fleetShipAlive[f][s] ? 1 : 2);
    fleet_ship_view[s].color = accent;
    const int cx = 133 + (s % 3) * 100, cy = 136 + (s / 3) * 92;
    lv_obj_t *o = lv_obj_create(screen_root);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(o, 60, 48);
    lv_obj_set_pos(o, cx - 30, cy - 24);
    lv_obj_add_event_cb(o, fleetShipDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)s);
    lv_obj_t *n = lv_label_create(screen_root);
    lv_label_set_text(n, ATK_SHIP_NAMES[s]);
    lv_obj_set_style_text_font(n, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(fleet_ship_view[s].state == 1 ? UI_TEXT : UI_FAINT), 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, cx - 233, cy + 26);
  }

  // v0.28.2 / v0.28.4 (user 2026-10-19): every fleet is founded or rebuilt at your
  // home hangar -- no planet needed ("if you lose your first fleet the game is
  // essentially over"; "you should be able to create 2-3 at your home base").
  if (gs.phase == PHASE_INVEST && (offBoard || count < cap || alive < count)) {
    const char *what = offBoard ? (count > 0 ? "REBUILD AT HANGAR" : "FOUND AT HANGAR") : alive < count ? "REPAIR A SHIP" : "INVEST NEW SHIP";  // v0.27.3 repair
    snprintf(buf, sizeof(buf), "%d CR", FLEET_INVEST_COST);
    const bool afford = me.credits >= FLEET_INVEST_COST;
    lv_obj_t *pill = uiPill(screen_root, 83, 316, 300, 64, what, buf, 0xFFD166, afford ? investFleetCb : nullptr);
    if (!afford) uiDimColors(pill, LV_OPA_40);
  }

  uiBackButton(screen_root, 233, 408, showFleets);
}

// ---------------------------------------------------------------------------
// BATTLE (planet Fight or PvP) -- v0.20.0 animated scene
//
// The whole scene (dice, ships, shots, explosion, pixel planet, result
// banner) is ONE plain object painted by an LV_EVENT_DRAW_MAIN callback --
// the same technique as the star field, transmission static and planet
// wedges: no per-widget transform_scale (Convention 7), no big pixel
// buffer. A 33ms timer invalidates it while something is moving.
//
// Every device animates when it sees gs.battleExchange change. Each
// exchange plays for BA_TOTAL ms; tapping the scene (or FIGHT) skips to
// the end. Drawing reads only battle_view -- a copy of the battle fields
// taken when the exchange was detected -- so an animation stays
// consistent even if the next exchange lands while it's still playing.
// ---------------------------------------------------------------------------
static const int SCENE_X = 43, SCENE_Y = 84, SCENE_W = 380, SCENE_H = 200;

// Exchange timeline, ms from the moment a new exchange is seen.
static const uint32_t BA_DICE = 600;      // dice tumble, then land
static const uint32_t BA_SHOTS = 900;     // shots in flight
static const uint32_t BA_FLASH = 1000;    // hit ship blinks white
static const uint32_t BA_BOOM = 1400;     // fragments fly (+ screen shake)
static const uint32_t BA_TOTAL = 1500;    // settle; result text appears
static const uint32_t BA_SHAKE_MS = 260;  // same punch as the transmission shake

// Scene layout, in scene-local pixels. "Your" side is always on the left.
static const int SHIP_W = 26, SHIP_H = 16;
static const int SHIP_LEFT_X = 26, SHIP_RIGHT_X = SCENE_W - 26 - SHIP_W;
static const int SHIP_TOP_Y = 22, SHIP_STEP = 23;  // 2.0-dev.20: 23 (was 26) so an Empire column of 7 fits
static const int GARRISON_X = SCENE_W - 44, GARRISON_TOP_Y = 20, GARRISON_STEP = 20, GARRISON_SIZE = 14;
static const uint32_t GARRISON_COLOR = 0xFF7A45;  // neutral -- affiliation is hidden intel
static const int DIE_SIZE = 44, DIE_Y = 6;

// Pixel planet: a PLANET_GRID x PLANET_GRID disc of PLANET_CELL px blocks,
// sitting low in the scene so the fleets fight "in orbit" above it.
static const int PLANET_GRID = 28, PLANET_CELL = 5;
static const int PLANET_CX = SCENE_W / 2, PLANET_CY = 178;
static const uint32_t PLANET_STEP_MS = 120;  // rotation step

// Four shades per planet, darkest first. Index order matches PLANETS[] in
// game_state.cpp; the phone's PLANET_PALETTES in web_server.cpp must match.
static const uint32_t PLANET_PALETTES[NUM_PLANETS][4] = {
  { 0x7A5230, 0xC08A4A, 0xE3C07A, 0xF5E1A8 },  // TATOOINE  - desert sand
  { 0x5A7A9A, 0x9EC3DE, 0xDDEFFA, 0xFFFFFF },  // HOTH      - ice
  { 0x8A4A3A, 0xD08858, 0xF0B888, 0xFFE0C0 },  // BESPIN    - gas giant
  { 0x1E3A22, 0x3E6B35, 0x6E8F4A, 0x9CB06A },  // DAGOBAH   - swamp
  { 0x2A2A38, 0x55556A, 0x8A8A9E, 0xFFD166 },  // CORUSCANT - city lights
  { 0x3A2A1A, 0x6B4A2A, 0x9A7A4A, 0xE0C080 },  // KESSEL    - spice rock
  { 0x3A2440, 0x6A4A6A, 0x9A7A8A, 0xC0A0B0 },  // OBA DIAH  - dusky violet
  { 0x3A0A0A, 0x7A1A1A, 0xB03030, 0xE06050 },  // DATHOMIR  - blood red
  { 0x1A3A6A, 0x2A6AAA, 0x4AA04A, 0xE0E0D0 },  // CORELLIA  - ocean + land
  { 0x1A4A8A, 0x3A8AC8, 0x5AB05A, 0xF0F0F0 },  // NABOO     - lakes + plains
  { 0x10301A, 0x1E5A2A, 0x3A8A3A, 0x6AB04A },  // ENDOR     - forest moon
  { 0x4A5A6A, 0x8A9AAA, 0xC0D0DA, 0xFFFFFF },  // KIJIMI    - snowy city
  { 0x0E2A2A, 0x1E5A44, 0x3E9A5A, 0xC8D890 },  // YAVIN 4   - jungle moon
  { 0x140606, 0x3A1410, 0xC0401A, 0xFFA040 },  // MUSTAFAR  - lava
  { 0x2A2A10, 0x5A5A20, 0x8A8A3A, 0xC8B860 },  // NAL HUTTA - murky swamp
  { 0x0A2A4A, 0x1A5A8A, 0x3A9AC8, 0xF0D080 },  // CANTO BIGHT - sea + gold
  { 0x141820, 0x3A4250, 0x7A8698, 0xE8B850 },  // KUAT      - shipyard steel
};

struct BattleView {
  bool pvp;
  bool leftIsAttacker;
  bool atkAlive[BATTLE_SHIPS];
  bool defAlive[BATTLE_SHIPS];
  int atkFleets, defFleets;  // v0.28.0: fleets on each side (bit per fleet)
  int defRemaining;   // planet garrison
  int atkRoll, defRoll;  // totals, INCLUDING the faction modifiers below
  int atkMod, defMod;    // faction roll modifiers (planet battles; 0 in PvP)
  int retreat;           // GameState.lastRetreatSide: 1/2 = attacker/defender withdrew under fire
  int lossIdx;        // ship destroyed this exchange, -1 = none / garrison
  int exchange;
  bool over;
  int planetIdx;
  bool captured;      // planet battle: attacker now owns it
  uint32_t atkColor, defColor;
};
static BattleView battle_view;
static lv_obj_t *battle_scene = nullptr;
static lv_timer_t *battle_anim_timer = nullptr;
static bool battle_anim_active = false;
static uint32_t battle_anim_start = 0;
// Two-tap WITHDRAW (v0.20.2).
static bool withdraw_armed = false;
static uint32_t withdraw_armed_at = 0;
static const uint32_t WITHDRAW_ARM_MS = 4000;

// Frame-timing diagnostics, printed to Serial after each exchange so the
// first hardware test gives real numbers (Convention 9). Note the draw
// callback only QUEUES draw tasks in LVGL 9; "frames" (callbacks per
// animation, ideal ~45 at 33ms) is the real smoothness signal.
static uint32_t battle_anim_frames = 0;
static uint32_t battle_draw_us_total = 0, battle_draw_us_max = 0;

static uint8_t planet_px[PLANET_GRID][PLANET_GRID];  // palette index, 255 = off the disc
static int planet_px_for = -1;                       // planet the cache holds
static uint32_t planet_px_step = 0xFFFFFFFF;
static uint32_t planet_compute_us_max = 0;

static uint32_t slotColor(const GameState &gs, int slot) {
  if (slot < 0 || slot >= MAX_PLAYERS) return 0x38BDF8;
  int f = gs.players[slot].chosenFaction;
  return (f >= 0 && f < 4) ? FACTION_VISUALS[f].color : 0x38BDF8;
}

static void fillBattleView(const GameState &gs) {
  BattleView &v = battle_view;
  v.pvp = (gs.battleKind == BATTLE_PVP);
  v.leftIsAttacker = !(v.pvp && gs.battleDefenderSlot == MY_SLOT);
  for (int i = 0; i < BATTLE_SHIPS; i++) {
    v.atkAlive[i] = gs.atkAlive[i];
    v.defAlive[i] = gs.defAlive[i];
  }
  v.atkFleets = gs.battleAttackerFleets ? gs.battleAttackerFleets : 1;
  v.defFleets = gs.battleDefenderFleets ? gs.battleDefenderFleets : 1;
  v.defRemaining = gs.defRemaining;
  v.atkRoll = gs.lastAtkRoll;
  v.defRoll = gs.lastDefRoll;
  v.atkMod = gs.battleAtkMod;
  v.defMod = gs.battleDefMod;
  v.retreat = gs.lastRetreatSide;
  v.lossIdx = gs.lastLossIdx;
  v.exchange = gs.battleExchange;
  v.over = gs.battleOver;
  v.planetIdx = gs.battleTargetPlanet;
  v.captured = !v.pvp && v.planetIdx >= 0 && v.planetIdx < NUM_PLANETS
               && gs.planetOwner[v.planetIdx] == gs.battleAttackerSlot;
  v.atkColor = slotColor(gs, gs.battleAttackerSlot);
  v.defColor = v.pvp ? slotColor(gs, gs.battleDefenderSlot) : GARRISON_COLOR;
}

// Rebuilds the planet's pixel grid for one rotation step. A sphere is
// faked per block: longitude (which shifts with `step`, so the surface
// drifts sideways like rotation) warps latitude bands into swirls, and a
// light from the upper-left darkens the far limb.
static void computePlanetPixels(int planetIdx, uint32_t step) {
  uint32_t t0 = micros();
  float phase = (float)(step % 10000) * 0.06f;
  float seed = (float)planetIdx * 1.7f + 0.3f;
  const float half = PLANET_GRID / 2.0f;
  for (int j = 0; j < PLANET_GRID; j++) {
    float ny = ((float)j + 0.5f - half) / half;
    for (int i = 0; i < PLANET_GRID; i++) {
      float nx = ((float)i + 0.5f - half) / half;
      float r2 = nx * nx + ny * ny;
      if (r2 > 1.0f) { planet_px[j][i] = 255; continue; }
      float z = sqrtf(1.0f - r2);
      float lon = atan2f(nx, z) + phase;
      float v = sinf(ny * 5.0f + seed + 1.4f * sinf(lon * 2.0f + ny * 3.0f + seed));
      v += 0.35f * sinf(lon * 5.0f + seed * 2.0f) * sinf(ny * 9.0f);
      int idx = (v < -0.6f) ? 0 : (v < 0.1f) ? 1 : (v < 0.7f) ? 2 : 3;
      float light = -0.55f * nx - 0.35f * ny + 0.6f * z;
      if (light < 0.15f && idx > 0) idx--;
      if (light < -0.2f && idx > 0) idx--;
      planet_px[j][i] = (uint8_t)idx;
    }
  }
  planet_px_for = planetIdx;
  planet_px_step = step;
  uint32_t dt = micros() - t0;
  if (dt > planet_compute_us_max) planet_compute_us_max = dt;
}

#if SF_ROLE == SF_ROLE_TABLE
// v0.26.0: the glow renderer's per-frame hook -- turn the planet in the center.
static void tablePlanetPre() {
  TrScene &s = tr_scene;
  if (s.planet < 0 || s.planet >= NUM_PLANETS) { s.planetPx = nullptr; return; }
  const uint32_t step = lv_tick_get() / PLANET_STEP_MS;
  if (planet_px_for != s.planet || planet_px_step != step) computePlanetPixels(s.planet, step);
  s.planetPx = &planet_px[0][0];
}
#endif

static void sceneRect(lv_layer_t *layer, int x1, int y1, int x2, int y2,
                      uint32_t color, lv_opa_t opa, int radius) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_hex(color);
  d.bg_opa = opa;
  d.radius = radius;
  lv_area_t a = { (lv_coord_t)x1, (lv_coord_t)y1, (lv_coord_t)x2, (lv_coord_t)y2 };
  if (!layerSees(layer, x1, y1, x2, y2)) return;
  lv_draw_rect(layer, &d, &a);
}
// A turning pixel planet filling its object (Home's GALAXY button at 2px
// blocks, the planet page at 4px -- v0.21.4): the battle scene's planet
// renderer and shared pixel cache. user_data = planet index; block size =
// object width / PLANET_GRID.
// v0.23.4 diagnostics: timed wrapper (see [UI] 5s draw ms).
static void spinPlanetDrawCbImpl(lv_event_t *e);
static void spinPlanetDrawCb(lv_event_t *e) { const uint32_t t0 = micros(); spinPlanetDrawCbImpl(e); g_uiDrawUs[UI_T_PLANET] += micros() - t0; }
static void spinPlanetDrawCbImpl(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t area;
  lv_obj_get_coords(obj, &area);
  int p = (int)(intptr_t)lv_event_get_user_data(e);
  if (p < 0 || p >= NUM_PLANETS) p = 0;
  uint32_t step = lv_tick_get() / PLANET_STEP_MS;
  if (planet_px_for != p || planet_px_step != step) computePlanetPixels(p, step);
  const uint32_t *pal = PLANET_PALETTES[p];
  const int cell = lv_area_get_width(&area) / PLANET_GRID > 0 ? lv_area_get_width(&area) / PLANET_GRID : 1;
  // 2.0 frame rate: one image per (planet, block size), repainted only when
  // the rotation step changes -- instead of ~150-300 block draws every frame.
  {
    struct SpinImg { int p, cell; uint32_t step; uint8_t *buf; lv_image_dsc_t img; };
    static SpinImg spin[3] = { { -1 }, { -1 }, { -1 } };
    static int spinNext = 0;
    SpinImg *s = nullptr;
    for (auto &c : spin) if (c.p == p && c.cell == cell) s = &c;
    if (!s && cell <= 6) {
      s = &spin[spinNext];
      spinNext = (spinNext + 1) % 3;
      if (s->buf && s->cell != cell) { heap_caps_free(s->buf); s->buf = nullptr; }
      const int size = PLANET_GRID * cell;
      if (!s->buf) s->buf = (uint8_t *)heap_caps_malloc((size_t)size * size * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      s->p = s->buf ? p : -1;
      s->cell = cell;
      s->step = step + 1;  // force a paint
      if (!s->buf) s = nullptr;
    }
    if (s) {
      const int size = PLANET_GRID * cell;
      if (s->step != step) {
        for (int y = 0; y < size; y++) {
          for (int x = 0; x < size; x++) {
            const uint8_t c = planet_px[y / cell][x / cell];
            uint8_t *px = s->buf + ((size_t)y * size + x) * 4;  // ARGB8888 is B,G,R,A in memory
            const uint32_t rgb = (c == 255) ? 0 : pal[c];
            px[0] = rgb & 0xFF; px[1] = (rgb >> 8) & 0xFF; px[2] = (rgb >> 16) & 0xFF; px[3] = (c == 255) ? 0 : 0xFF;
          }
        }
        lv_image_dsc_t &img = s->img;
        memset(&img, 0, sizeof(img));
        img.header.magic = LV_IMAGE_HEADER_MAGIC;
        img.header.cf = LV_COLOR_FORMAT_ARGB8888;
        img.header.w = size;
        img.header.h = size;
        img.header.stride = size * 4;
        img.data_size = (uint32_t)size * size * 4;
        img.data = s->buf;
        s->step = step;
      }
      const lv_area_t at = { area.x1, area.y1, area.x1 + size - 1, area.y1 + size - 1 };
      if (!layerSees(layer, at.x1, at.y1, at.x2, at.y2)) return;
      lv_draw_image_dsc_t d;
      lv_draw_image_dsc_init(&d);
      d.src = &s->img;
      lv_draw_image(layer, &d, &at);
      return;
    }
  }
  for (int j = 0; j < PLANET_GRID; j++) {
    int y = area.y1 + j * cell;
    if (!layerSees(layer, area.x1, y, area.x2, y + cell - 1)) continue;
    int i = 0;
    while (i < PLANET_GRID) {
      uint8_t c = planet_px[j][i];
      int runStart = i;
      while (i < PLANET_GRID && planet_px[j][i] == c) i++;
      if (c == 255) continue;
      sceneRect(layer, area.x1 + runStart * cell, y, area.x1 + i * cell - 1, y + cell - 1, pal[c], LV_OPA_COVER, 0);
    }
  }
}

// Galaxy ring art (v0.21.2): each planet baked ONCE into a small ARGB8888
// image (frozen at one rotation step) -- 12 planets redrawn block-by-block
// on every star tick would be ~1500 rect draws per frame. v0.21.3: two
// sizes to match the ring's circle size (64px for 11-12 planets, 70px for
// fewer), nearest-neighbor from the 28x28 grid. Each (planet, size) gets its
// own descriptor + PSRAM buffer, baked on first use and never rewritten --
// LVGL's image cache keys on the descriptor, so reusing one for new pixels
// or a new size would need a cache drop.
static const int RING_IMG_SIZES[4] = { 40, 52, 64, 70 };  // 52 = the v0.21.14 board map; 40 = its core worlds beside the Ashen Fleet
static lv_image_dsc_t ring_img_dsc[4][NUM_PLANETS];
static uint8_t *ring_img_data[4][NUM_PLANETS];

static const lv_image_dsc_t *ringPlanetImage(int p, int size) {
  if (p < 0 || p >= NUM_PLANETS) p = 0;
  const int si = (size >= RING_IMG_SIZES[3]) ? 3 : (size >= RING_IMG_SIZES[2]) ? 2 : (size >= RING_IMG_SIZES[1] ? 1 : 0);
  size = RING_IMG_SIZES[si];
  if (!ring_img_data[si][p]) {
    const size_t bytes = (size_t)size * size * 4;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t *)malloc(bytes);
    if (!buf) return nullptr;
    computePlanetPixels(p, (uint32_t)p * 37);  // per-planet phase so they don't all match
    const uint32_t *pal = PLANET_PALETTES[p];
    for (int y = 0; y < size; y++) {
      const int gy = y * PLANET_GRID / size;
      for (int x = 0; x < size; x++) {
        uint8_t c = planet_px[gy][x * PLANET_GRID / size];
        uint8_t *px = buf + ((size_t)y * size + x) * 4;  // ARGB8888 is B,G,R,A in memory
        uint32_t rgb = (c == 255) ? 0 : pal[c];
        px[0] = rgb & 0xFF;
        px[1] = (rgb >> 8) & 0xFF;
        px[2] = (rgb >> 16) & 0xFF;
        px[3] = (c == 255) ? 0 : 0xFF;
      }
    }
    lv_image_dsc_t &d = ring_img_dsc[si][p];
    memset(&d, 0, sizeof(d));
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_ARGB8888;
    d.header.w = size;
    d.header.h = size;
    d.header.stride = size * 4;
    d.data_size = bytes;
    d.data = buf;
    ring_img_data[si][p] = buf;
  }
  return &ring_img_dsc[si][p];
}

static void sceneBox(lv_layer_t *layer, int x1, int y1, int x2, int y2, uint32_t fill,
                     lv_opa_t fillOpa, uint32_t border, int borderW, int radius) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_hex(fill);
  d.bg_opa = fillOpa;
  d.border_color = lv_color_hex(border);
  d.border_width = borderW;
  d.border_opa = LV_OPA_COVER;
  d.radius = radius;
  lv_area_t a = { (lv_coord_t)x1, (lv_coord_t)y1, (lv_coord_t)x2, (lv_coord_t)y2 };
  if (!layerSees(layer, x1, y1, x2, y2)) return;
  lv_draw_rect(layer, &d, &a);
}

static void sceneText(lv_layer_t *layer, int x1, int y1, int x2, int y2, const char *txt,
                      uint32_t color, const lv_font_t *font) {
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = txt;
  d.text_local = 1;  // draw tasks run after this callback returns -- copy stack text
  d.color = lv_color_hex(color);
  d.font = font;
  d.align = LV_TEXT_ALIGN_CENTER;
  lv_area_t a = { (lv_coord_t)x1, (lv_coord_t)y1, (lv_coord_t)x2, (lv_coord_t)y2 };
  lv_draw_label(layer, &d, &a);
}

static void sceneShip(lv_layer_t *layer, int x, int y, bool facingRight, uint32_t color) {
  lv_draw_triangle_dsc_t d;
  lv_draw_triangle_dsc_init(&d);
  d.color = lv_color_hex(color);
  d.opa = LV_OPA_COVER;
  int back = facingRight ? x : x + SHIP_W;
  int nose = facingRight ? x + SHIP_W : x;
  d.p[0].x = back; d.p[0].y = y;
  d.p[1].x = nose; d.p[1].y = y + SHIP_H / 2;
  d.p[2].x = back; d.p[2].y = y + SHIP_H;
  lv_draw_triangle(layer, &d);
  // Engine glow at the back.
  int ex = facingRight ? x - 4 : x + SHIP_W + 1;
  sceneRect(layer, ex, y + SHIP_H / 2 - 2, ex + 3, y + SHIP_H / 2 + 2, 0x8CE7FF, LV_OPA_COVER, 1);
}

// Scene-local center of a unit: ship index on a side, or garrison marker.
// v0.28.0: idx = fleet * FLEET_SHIPS + slot. Each fleet in the battle gets its
// own column (stacking toward the middle), in fleet order with no gaps.
static int scene_left_mask = 1, scene_right_mask = 1;  // fleets on each side (set per draw)
static void shipCenter(bool left, int idx, int *cx, int *cy) {
  const int f = idx / FLEET_SHIPS, mask = left ? scene_left_mask : scene_right_mask;
  int col = 0;
  for (int k = 0; k < f; k++) if (mask & (1 << k)) col++;
  *cx = (left ? SHIP_LEFT_X + col * 30 : SHIP_RIGHT_X - col * 30) + SHIP_W / 2;
  *cy = SHIP_TOP_Y + (idx % FLEET_SHIPS) * SHIP_STEP + SHIP_H / 2;
}
static void garrisonCenter(int idx, int *cx, int *cy) {
  *cx = GARRISON_X + GARRISON_SIZE / 2;
  *cy = GARRISON_TOP_Y + idx * GARRISON_STEP + GARRISON_SIZE / 2;
}

static void battleSceneDrawCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
  uint32_t t0 = micros();
  lv_layer_t *layer = lv_event_get_layer(e);
  lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
  lv_area_t area;
  lv_obj_get_coords(obj, &area);
  const BattleView &v = battle_view;

  // t = where we are in this exchange's animation (BA_TOTAL = at rest).
  bool anim = battle_anim_active && v.exchange > 0;
  uint32_t t = anim ? (lv_tick_get() - battle_anim_start) : BA_TOTAL;
  if (t > BA_TOTAL) t = BA_TOTAL;

  bool rolled = v.exchange > 0;
  bool atkWon = rolled && v.atkRoll > v.defRoll;
  bool defWon = rolled && v.defRoll > v.atkRoll;
  // Retreat under fire: the withdrawing side can't score -- if it out-rolls
  // the pursuer it simply escapes (drawn as the pursuer's shots missing).
  if (v.retreat == 1) atkWon = false;
  else if (v.retreat == 2) defWon = false;
  bool hit = atkWon || defWon;
  bool garrisonHit = !v.pvp && atkWon;  // attacker knocked out a garrison ship

  // Screen shake right as the hit unit blows up.
  int ox = area.x1, oy = area.y1;
  if (anim && hit && t >= BA_FLASH && t < BA_FLASH + BA_SHAKE_MS) {
    int32_t s = (int32_t)((t - BA_FLASH) * 100 / BA_SHAKE_MS);
    float decay = (1.0f - s / 100.0f) * (1.0f - s / 100.0f);
    ox += (int)((shakeNoise(s) * 2.0f - 1.0f) * 10.0f * decay);
    oy += (int)((shakeNoise(s + 1000) * 2.0f - 1.0f) * 6.0f * decay);
  }

  // --- Pixel planet (planet battles only) ---
  if (!v.pvp && v.planetIdx >= 0 && v.planetIdx < NUM_PLANETS) {
    uint32_t step = lv_tick_get() / PLANET_STEP_MS;
    if (planet_px_for != v.planetIdx || planet_px_step != step) computePlanetPixels(v.planetIdx, step);
    const uint32_t *pal = PLANET_PALETTES[v.planetIdx];
    int px0 = ox + PLANET_CX - (PLANET_GRID * PLANET_CELL) / 2;
    int py0 = oy + PLANET_CY - (PLANET_GRID * PLANET_CELL) / 2;
    for (int j = 0; j < PLANET_GRID; j++) {
      int y = py0 + j * PLANET_CELL;
      if (y > area.y2 + 12) break;  // below the scene (allowing for shake)
      // Merge runs of same-colored blocks into one rect each.
      int i = 0;
      while (i < PLANET_GRID) {
        uint8_t c = planet_px[j][i];
        int runStart = i;
        while (i < PLANET_GRID && planet_px[j][i] == c) i++;
        if (c == 255) continue;
        sceneRect(layer, px0 + runStart * PLANET_CELL, y, px0 + i * PLANET_CELL - 1,
                  y + PLANET_CELL - 1, pal[c], LV_OPA_COVER, 0);
      }
    }
  }

  // --- Units. Left = attacker unless I'm the PvP defender. ---
  bool atkLeft = v.leftIsAttacker;
  scene_left_mask = atkLeft ? v.atkFleets : v.defFleets;
  scene_right_mask = atkLeft ? v.defFleets : v.atkFleets;
  bool blinkOn = ((t / 50) % 2) == 0;
  bool inFlash = anim && t >= BA_SHOTS && t < BA_FLASH;
  bool stillShowLoss = anim && t < BA_FLASH;  // the hit unit only vanishes once it explodes

  // Attacker ships.
  for (int i = 0; i < BATTLE_SHIPS; i++) {
    bool isLoss = defWon && i == v.lossIdx;
    if (!(v.atkAlive[i] || (isLoss && stillShowLoss))) continue;
    int cx, cy;
    shipCenter(atkLeft, i, &cx, &cy);
    uint32_t col = (isLoss && inFlash && blinkOn) ? 0xFFFFFF : v.atkColor;
    sceneShip(layer, ox + cx - SHIP_W / 2, oy + cy - SHIP_H / 2, atkLeft, col);
  }
  // Defender: ships (PvP) or garrison markers (planet).
  int garrisonShown = v.defRemaining + ((garrisonHit && stillShowLoss) ? 1 : 0);
  if (v.pvp) {
    for (int i = 0; i < BATTLE_SHIPS; i++) {
      bool isLoss = atkWon && i == v.lossIdx;
      if (!(v.defAlive[i] || (isLoss && stillShowLoss))) continue;
      int cx, cy;
      shipCenter(!atkLeft, i, &cx, &cy);
      uint32_t col = (isLoss && inFlash && blinkOn) ? 0xFFFFFF : v.defColor;
      sceneShip(layer, ox + cx - SHIP_W / 2, oy + cy - SHIP_H / 2, !atkLeft, col);
    }
  } else {
    for (int i = 0; i < garrisonShown && i < MAX_PLANET_DEFENSE; i++) {
      bool isLoss = garrisonHit && i == garrisonShown - 1;
      int cx, cy;
      garrisonCenter(i, &cx, &cy);
      uint32_t col = (isLoss && inFlash && blinkOn) ? 0xFFFFFF : GARRISON_COLOR;
      sceneBox(layer, ox + cx - GARRISON_SIZE / 2, oy + cy - GARRISON_SIZE / 2,
               ox + cx + GARRISON_SIZE / 2, oy + cy + GARRISON_SIZE / 2,
               col, LV_OPA_60, col, 2, 3);
    }
  }

  // Where each side's shots come from / land: first unit still showing,
  // and the specific unit that was hit.
  auto firstUnit = [&](bool attackerSide, int *cx, int *cy) {
    if (attackerSide || v.pvp) {
      const bool *alive = attackerSide ? v.atkAlive : v.defAlive;
      bool left = attackerSide ? atkLeft : !atkLeft;
      bool lostHere = attackerSide ? defWon : atkWon;  // this side took the hit
      for (int i = 0; i < BATTLE_SHIPS; i++) {
        if (alive[i] || (lostHere && i == v.lossIdx)) { shipCenter(left, i, cx, cy); return; }
      }
      shipCenter(left, 2, cx, cy);
    } else {
      garrisonCenter(0, cx, cy);
    }
  };
  int tx = 0, ty = 0;  // hit target
  if (garrisonHit) garrisonCenter(garrisonShown - 1 < 0 ? 0 : garrisonShown - 1, &tx, &ty);
  else if (atkWon && v.pvp) shipCenter(!atkLeft, v.lossIdx < 0 ? 0 : v.lossIdx, &tx, &ty);
  else if (defWon) shipCenter(atkLeft, v.lossIdx < 0 ? 0 : v.lossIdx, &tx, &ty);

  // --- Shots: three per shooter, staggered. Tie = both sides fire and
  // the shots fizzle out mid-way (a miss). ---
  if (anim && rolled && t >= BA_DICE && t < BA_SHOTS + 60) {
    for (int side = 0; side < 2; side++) {  // 0 = attacker fires, 1 = defender fires
      // Normal miss (tie): both sides fire. Retreat: only the pursuer fires.
      bool fires = hit ? (side == 0 ? atkWon : defWon)
                       : (v.retreat == 0 || (side == 0) == (v.retreat == 2));
      if (!fires) continue;
      int sx, sy, ex, ey;
      firstUnit(side == 0, &sx, &sy);
      if (hit) { ex = tx; ey = ty; } else firstUnit(side != 0, &ex, &ey);
      uint32_t col = (side == 0) ? v.atkColor : v.defColor;
      for (int k = 0; k < 3; k++) {
        int32_t dt = (int32_t)t - (int32_t)BA_DICE - k * 60;
        if (dt < 0) continue;
        float p = dt / 240.0f;
        if (p > 1.0f) continue;
        if (!hit && p > 0.45f) continue;  // missed -- fizzled out
        int x = sx + (int)((ex - sx) * p);
        int y = sy + (int)((ey - sy) * p) + (k - 1) * 4;
        // Laser bolt: 18x5 in the shooter's color with a bright 10x2 core.
        sceneRect(layer, ox + x - 9, oy + y - 2, ox + x + 9, oy + y + 2, col, LV_OPA_COVER, 2);
        sceneRect(layer, ox + x - 5, oy + y - 1, ox + x + 5, oy + y, 0xFFFFFF, LV_OPA_COVER, 1);
      }
    }
  }

  // --- Explosion: a flash, then fragments flying out and fading. ---
  if (anim && hit && t >= BA_FLASH && t < BA_BOOM) {
    float q = (float)(t - BA_FLASH) / (float)(BA_BOOM - BA_FLASH);
    if (q < 0.3f) {
      int r = (int)(16.0f * (1.0f - q / 0.3f)) + 4;
      sceneRect(layer, ox + tx - r, oy + ty - r, ox + tx + r, oy + ty + r, 0xFFE8B0,
                (lv_opa_t)(255 * (1.0f - q)), LV_RADIUS_CIRCLE);
    }
    static const uint32_t FRAG_COLORS[3] = { 0xFFFFFF, 0xFFD166, 0xFF8A3D };
    for (int k = 0; k < 10; k++) {
      float ang = k * 0.628f + shakeNoise(k + v.exchange * 13) * 0.5f;
      float dist = q * (30.0f + shakeNoise(k * 7 + v.exchange) * 20.0f);
      int fx = tx + (int)(cosf(ang) * dist);
      int fy = ty + (int)(sinf(ang) * dist);
      int sz = 4 - (int)(q * 2.5f);
      sceneRect(layer, ox + fx - sz, oy + fy - sz, ox + fx + sz, oy + fy + sz,
                FRAG_COLORS[k % 3], (lv_opa_t)(255 * (1.0f - q)), 1);
    }
  }

  // --- Dice: left die = left side's roll. Tumble, then land; the winning
  // die gets a gold border. A side with a faction modifier shows the raw
  // die face plus a "+N" tag (sources are spelled out on the briefing). ---
  for (int d = 0; d < 2; d++) {
    bool isAtk = (d == 0) == atkLeft;
    int roll = isAtk ? v.atkRoll : v.defRoll;
    int mod = isAtk ? v.atkMod : v.defMod;
    int face = roll - mod;
    int dx = SCENE_W / 2 + (d == 0 ? -DIE_SIZE - 8 : 8);
    int dy = DIE_Y;
    char txt[4];
    uint32_t border = 0x3A5568;
    if (!rolled) {
      snprintf(txt, sizeof(txt), "?");
    } else if (anim && t < BA_DICE) {
      int32_t tick = (int32_t)(t / 70);
      face = 1 + (int)(shakeNoise(tick * 3 + d * 31 + v.exchange) * 5.99f);
      dy += (int)((shakeNoise(tick + d * 7) - 0.5f) * 8.0f);
      snprintf(txt, sizeof(txt), "%d", face);
    } else {
      snprintf(txt, sizeof(txt), "%d", face);
      bool won = isAtk ? atkWon : defWon;
      border = won ? 0xFFD166 : 0x8A97A6;
    }
    int x1 = ox + dx, y1 = oy + dy;
    sceneBox(layer, x1, y1, x1 + DIE_SIZE, y1 + DIE_SIZE, 0x081725, LV_OPA_COVER, border, 3, 8);
    sceneText(layer, x1, y1 + 10, x1 + DIE_SIZE, y1 + 36, txt, 0xD9F5FF, LV_FONT_DEFAULT);
    if (mod > 0 && rolled && !(anim && t < BA_DICE)) {
      char tag[6];
      snprintf(tag, sizeof(tag), "+%d", mod);
      sceneText(layer, x1, y1 + DIE_SIZE + 2, x1 + DIE_SIZE, y1 + DIE_SIZE + 20, tag, 0xFFD166,
                &lv_font_montserrat_14);
    }
  }

  // --- Result banner once the battle is over and the last hit has played. ---
  if (v.over && !battle_anim_active) {
    const bool *mine = atkLeft ? v.atkAlive : v.defAlive;
    const bool *theirs = atkLeft ? v.defAlive : v.atkAlive;
    bool mineAny = false, theirsAny = false;
    for (int i = 0; i < BATTLE_SHIPS; i++) { mineAny |= mine[i]; theirsAny |= theirs[i]; }
    const char *txt;
    uint32_t col;
    if (!v.pvp) {
      if (v.captured) { txt = "PLANET CAPTURED"; col = 0xFFD166; }
      else if (!mineAny) { txt = "FLEET DESTROYED"; col = 0xFF6B6B; }
      else { txt = "WITHDREW"; col = 0x8CE7FF; }
    } else {
      if (!theirsAny) { txt = "VICTORY"; col = 0xFFD166; }
      else if (!mineAny) { txt = "DEFEAT"; col = 0xFF6B6B; }
      else { txt = "WITHDREW"; col = 0x8CE7FF; }
    }
    int bx1 = ox + SCENE_W / 2 - 120, by1 = oy + 76;
    sceneBox(layer, bx1, by1, bx1 + 240, by1 + 44, 0x02060D, LV_OPA_80, col, 2, 10);
    sceneText(layer, bx1, by1 + 10, bx1 + 240, by1 + 36, txt, col, LV_FONT_DEFAULT);
  }

  if (battle_anim_active) {
    uint32_t dt = micros() - t0;
    battle_anim_frames++;
    battle_draw_us_total += dt;
    if (dt > battle_draw_us_max) battle_draw_us_max = dt;
  }
}

static void startBattleAnim() {
  battle_anim_active = true;
  battle_anim_start = lv_tick_get();
  battle_anim_frames = 0;
  battle_draw_us_total = 0;
  battle_draw_us_max = 0;
}

static void skipBattleAnim() {
  if (battle_anim_active) battle_anim_start = lv_tick_get() - BA_TOTAL;
}

static void updateBattleUI();

static void battleAnimTick(lv_timer_t *timer) {
  (void)timer;
  if (currentScreen != SCR_BATTLE || !battle_scene) return;
  if (battle_anim_active) {
    uint32_t t = lv_tick_get() - battle_anim_start;
    lv_obj_invalidate(battle_scene);
    if (t >= BA_TOTAL) {
      battle_anim_active = false;
      Serial.printf("[BATTLE] exchange %d: %u frames in %u ms, draw cb avg %u us / max %u us, planet calc max %u us\n",
                    battle_view.exchange, (unsigned)battle_anim_frames, (unsigned)t,
                    (unsigned)(battle_anim_frames ? battle_draw_us_total / battle_anim_frames : 0),
                    (unsigned)battle_draw_us_max, (unsigned)planet_compute_us_max);
      updateBattleUI();  // now reveal the exchange's result text
    }
  } else if (!battle_view.pvp && lv_tick_get() / PLANET_STEP_MS != planet_px_step) {
    lv_obj_invalidate(battle_scene);  // keep the planet turning
  }
}

static void battleSceneTapCb(lv_event_t *e) {
  (void)e;
  skipBattleAnim();
}

static void updateBattleUI() {
  if (currentScreen != SCR_BATTLE || !battle_scene || !battle_label || !battle_button
      || !withdraw_button) return;

  // The other commander pressed CONTINUE first and closed the battle:
  // go straight to this device's battle report (v0.21.13). v0.30.2: checked
  // with a scoped copy so it's gone before showVictory() / showHome() read theirs.
  bool closed;
  {
    GameState gs = gameStateRead();
    // v0.28.0: the reinforce offer lasts until the first exchange.
    if (reinforce_button && snapshotReinforceMask(gs, MY_SLOT) == 0) { lv_obj_delete(reinforce_button); reinforce_button = nullptr; }
    closed = !gs.battleActive;
  }
  if (closed) {
    if (currentScreen == SCR_BATTLE && !battle_anim_active) {
      if (battle_view.over) showVictory();
      else showHome();  // ended before this device ever saw the result -- don't strand it here
    }
    return;
  }
  GameState gs = gameStateRead();

  // New exchange? Snapshot it and play it. Otherwise keep the view fresh
  // while idle (e.g. the battle just ended by a withdraw).
  if (gs.battleExchange != battle_view.exchange) {
    fillBattleView(gs);
    if (battle_view.exchange > 0) startBattleAnim();
    lv_obj_invalidate(battle_scene);
  } else if (!battle_anim_active) {
    bool wasOver = battle_view.over;
    fillBattleView(gs);
    if (battle_view.over != wasOver) lv_obj_invalidate(battle_scene);
  }

  bool pvp = (gs.battleKind == BATTLE_PVP);
  bool iAmDefender = pvp && (gs.battleDefenderSlot == MY_SLOT);
  lv_obj_t *fightLabel = lv_obj_get_child(battle_button, 0);

  if (battle_anim_active) {
    // Don't spoil the exchange -- its result text appears when it lands.
    lv_label_set_text(battle_label, "");
  } else if (gs.battleOver) {
    lv_label_set_text(battle_label, gs.resultText);
    lv_obj_set_style_text_color(battle_label, lv_color_hex(0xFFD166), 0);
  }

  if (gs.battleOver) {
    lv_obj_add_flag(withdraw_button, LV_OBJ_FLAG_HIDDEN);
    if (fightLabel) lv_label_set_text(fightLabel, "CONTINUE");
  } else {
    // In PvP, if I've already submitted my action this exchange, show waiting.
    bool iActed = pvp && ((iAmDefender && gs.defAction != 0) || (!iAmDefender && gs.atkAction != 0));
    if (iActed) {
      lv_obj_add_flag(withdraw_button, LV_OBJ_FLAG_HIDDEN);
      if (fightLabel) lv_label_set_text(fightLabel, "WAITING...");
      if (!battle_anim_active) {
        lv_label_set_text(battle_label, "Waiting for the other commander...");
        lv_obj_set_style_text_color(battle_label, lv_color_hex(0x8CE7FF), 0);
      }
    } else {
      lv_obj_clear_flag(withdraw_button, LV_OBJ_FLAG_HIDDEN);
      if (fightLabel) lv_label_set_text(fightLabel, "FIGHT");
      // WITHDRAW is a two-tap confirm (v0.20.2): the first tap arms it and
      // spells out the cost; it disarms itself after WITHDRAW_ARM_MS.
      if (withdraw_armed && lv_tick_get() - withdraw_armed_at > WITHDRAW_ARM_MS) withdraw_armed = false;
      lv_obj_t *wLabel = lv_obj_get_child(withdraw_button, 0);
      if (wLabel) lv_label_set_text(wLabel, withdraw_armed ? "CONFIRM?" : "WITHDRAW");
      lv_obj_set_style_border_color(withdraw_button, lv_color_hex(withdraw_armed ? 0xFF6B6B : 0x38BDF8), 0);
      if (withdraw_armed && !battle_anim_active) {
        lv_label_set_text(battle_label, pvp
            ? "Withdrawing gives the enemy a parting shot at your fleet. Tap CONFIRM? to go."
            : "Withdrawing gives the garrison a parting shot, and this planet stays closed to you this round.");
        lv_obj_set_style_text_color(battle_label, lv_color_hex(0xFF6B6B), 0);
      } else if (!battle_anim_active) {
        const char *msg = gs.resultText[0] ? gs.resultText
                         : (pvp ? "Choose your action for this exchange."
                                : "The garrison will fight to the death.");
        lv_label_set_text(battle_label, msg);
        lv_obj_set_style_text_color(battle_label, lv_color_hex(0xD7F7FF), 0);
      }
    }
  }
}

static void battleTick(lv_timer_t *timer) {
  (void)timer;
  updateBattleUI();
}

// ---------------------------------------------------------------------------
// VICTORY / DEFEAT (v0.21.13 UI refresh; mockup "Victory -> comms"). After
// any battle the combatants get a battle report instead of being dropped
// straight back to a list: a big VICTORY / DEFEAT / WITHDREW badge, the
// planet (or the two fleets in PvP), what it got you (planet captured, +2 VP
// for destroying a fleet) or cost you, and -- if the battle shifted a
// faction's opinion of you -- an "INCOMING TRANSMISSION" chip. The
// transmission now plays after CONTINUE instead of cutting into the battle
// (starTick holds transmissions while SCR_BATTLE / SCR_VICTORY are up).
// Built from battle_view (this device's snapshot of the final exchange), so
// it still works when the other commander ended a PvP battle first.
// Touch: CONTINUE pill (143..323, 368..430), far corner ~217px.
// ---------------------------------------------------------------------------
static int countAlive(const bool *a) {
  int n = 0;
  for (int i = 0; i < BATTLE_SHIPS; i++) if (a[i]) n++;
  return n;
}

// (of() is a static member, not a free function: the Arduino preprocessor
// hoists free-function prototypes above this struct.)
struct BattleOutcome {
  int result;       // 1 = won, 0 = lost, 2 = withdrew
  bool decisive;    // a fleet was destroyed (+2 VP to the PvP winner)
  int myShips;      // my ships still alive
  int theirShips;   // enemy ships still alive (PvP)

  // Same rules as game_state.cpp's battle resolution, read from the
  // snapshot. Phone twin: battleOutcome() in web_server.cpp.
  static BattleOutcome of(const BattleView &v) {
  BattleOutcome o;
  const bool iAttack = !v.pvp || v.leftIsAttacker;
  const int atk = countAlive(v.atkAlive), def = countAlive(v.defAlive);
  o.myShips = iAttack ? atk : def;
  o.theirShips = iAttack ? def : atk;
  o.decisive = false;
  if (!v.pvp) {
    if (v.captured) o.result = 1;
    else if (v.retreat == 1 && atk > 0) o.result = 2;
    else o.result = 0;
    return o;
  }
  int winnerIsAttacker;  // 1 attacker won, 0 defender won
  if (def == 0) { winnerIsAttacker = 1; o.decisive = true; }
  else if (atk == 0) { winnerIsAttacker = 0; o.decisive = true; }
  else winnerIsAttacker = (v.retreat == 2) ? 1 : 0;  // the side that stayed wins
  const bool iWithdrew = (iAttack && v.retreat == 1) || (!iAttack && v.retreat == 2);
  if (iWithdrew && o.myShips > 0) o.result = 2;
  else o.result = ((winnerIsAttacker == 1) == iAttack) ? 1 : 0;
  return o;
  }
};

static void victoryChipOpaCb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

static void victoryContinueCb(lv_event_t *e) {
  (void)e;
  rep_base_set = false;  // v0.29.0
  if (showPendingTransmission()) return;
  if (battle_view.pvp) showHome();
  else showPlanets(nullptr);
}

static void showVictory() {
  currentScreen = SCR_VICTORY;
  onHomeScreen = false;
  onBattleScreen = false;
  battle_anim_active = false;
  clearScreen();

  GameState gs = gameStateRead();
  const BattleView &v = battle_view;
  const BattleOutcome o = BattleOutcome::of(v);
  const uint32_t accent = uiAccentFor(gs.players[MY_SLOT].chosenFaction);
  const uint32_t tone = (o.result == 1) ? accent : (o.result == 0 ? UI_RED : UI_DIM);
  uiAddRim(gs.roundLimit, gs.round, tone);

  uiLabel(screen_root, "BATTLE REPORT", UI_DIM, &sf_font_montserrat_12, 62, 4);
  lv_obj_t *badge = lv_obj_create(screen_root);
  uiStyleButton(badge, tone, 3, uiTint(tone), 28);  // style first (Convention 13)
  lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(badge, 260, 56);
  lv_obj_set_pos(badge, 103, 86);
  lv_obj_t *bl = lv_label_create(badge);
  lv_label_set_text(bl, o.result == 1 ? "VICTORY" : (o.result == 0 ? "DEFEAT" : "WITHDREW"));
  lv_obj_set_style_text_color(bl, lv_color_hex(tone), 0);
  lv_obj_set_style_text_letter_space(bl, 8, 0);
  lv_obj_center(bl);

  // The prize: the planet turning, or the two fleets that are left.
  const bool planetOk = !v.pvp && v.planetIdx >= 0 && v.planetIdx < NUM_PLANETS;
  if (planetOk) {
    lv_obj_t *art = lv_obj_create(screen_root);
    lv_obj_remove_style_all(art);
    lv_obj_set_size(art, 112, 112);
    lv_obj_set_pos(art, 233 - 56, 158);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(art, spinPlanetDrawCb, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)v.planetIdx);
    trackAnimated(art);
  } else {
    const uint32_t mine = (v.leftIsAttacker ? v.atkColor : v.defColor);
    const uint32_t theirs = (v.leftIsAttacker ? v.defColor : v.atkColor);
    if (o.myShips > 0) forecastUnits(233, 186, UNITS_SHIPS, o.myShips, mine);
    else uiLabel(screen_root, "NO SHIPS LEFT", UI_FAINT, &lv_font_montserrat_14, 186, 1);
    uiLabel(screen_root, "VS", UI_FAINT, &sf_font_montserrat_12, 214, 2);
    if (o.theirShips > 0) forecastUnits(233, 236, UNITS_SHIPS_LEFT, o.theirShips, theirs);
    else uiLabel(screen_root, "ENEMY FLEET GONE", UI_FAINT, &lv_font_montserrat_14, 236, 1);
  }

  // What it got you / cost you.
  char l1[48], l2[64];
  const char *pname = planetOk ? PLANETS[v.planetIdx].name : "";
  if (!v.pvp) {
    if (o.result == 1) { snprintf(l1, sizeof(l1), "%s IS YOURS", pname); snprintf(l2, sizeof(l2), "+%d VP while you hold it", planetVPNow(v.planetIdx, MY_SLOT)); }
    else if (o.result == 2) { snprintf(l1, sizeof(l1), "You pulled back from %s", pname); snprintf(l2, sizeof(l2), "%d ship%s made it out", o.myShips, o.myShips == 1 ? "" : "s"); }
    else { snprintf(l1, sizeof(l1), "%s HOLDS", pname); snprintf(l2, sizeof(l2), "Fleet lost - rebuild it at your hangar"); }
  } else {
    if (o.result == 1 && o.decisive) { snprintf(l1, sizeof(l1), "ENEMY FLEET DESTROYED"); snprintf(l2, sizeof(l2), "+%d VP", VP_PVP_DECISIVE_WIN); }
    else if (o.result == 1) { snprintf(l1, sizeof(l1), "THE ENEMY WITHDREW"); snprintf(l2, sizeof(l2), "%d ship%s still with you", o.myShips, o.myShips == 1 ? "" : "s"); }
    else if (o.result == 2) { snprintf(l1, sizeof(l1), "You withdrew"); snprintf(l2, sizeof(l2), "%d ship%s made it out", o.myShips, o.myShips == 1 ? "" : "s"); }
    else if (o.decisive) { snprintf(l1, sizeof(l1), "YOUR FLEET WAS DESTROYED"); snprintf(l2, sizeof(l2), "Rebuild it at a planet you hold"); }
    else { snprintf(l1, sizeof(l1), "THEY HELD"); snprintf(l2, sizeof(l2), "%d ship%s still with you", o.myShips, o.myShips == 1 ? "" : "s"); }
  }
  uiLabel(screen_root, l1, tone == UI_DIM ? UI_TEXT : tone, &lv_font_montserrat_14, 280, 1);
  uiLabel(screen_root, l2, UI_DIM, &lv_font_montserrat_14, 300, 0);

  // v0.29.0: whose standing the battle moved, as arrows.
  const bool repRow = uiRepRow(gs, 322);

  // A faction reacted: say so here, play it after CONTINUE.
  if (gs.transmissionPending[MY_SLOT]) {
    const int tf = gs.transmissionFaction[MY_SLOT];
    lv_obj_t *chip = lv_obj_create(screen_root);
    uiStyleButton(chip, UI_CYAN, 1, uiTint(UI_CYAN), 16);
    // v0.30.2: it pulses like a button, so tapping it does what CONTINUE does.
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(chip, victoryContinueCb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_size(chip, 240, repRow ? 24 : 32);
    lv_obj_set_pos(chip, 113, repRow ? 341 : 326);
    lv_obj_t *cl = lv_label_create(chip);
    char ct[48];
    snprintf(ct, sizeof(ct), "INCOMING TRANSMISSION%s%s", (tf >= 0 && tf < 4) ? " - " : "", (tf >= 0 && tf < 4) ? FACTION_SHORT[tf] : "");
    lv_label_set_text(cl, ct);
    lv_obj_set_style_text_font(cl, &sf_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(cl, 1, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(UI_CYAN), 0);
    lv_obj_center(cl);
    lv_anim_t a;  // gentle pulse; deleted with the chip
    lv_anim_init(&a);
    lv_anim_set_var(&a, chip);
    lv_anim_set_exec_cb(&a, victoryChipOpaCb);
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_reverse_duration(&a, 700);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
  }

  uiPill(screen_root, 143, 368, 180, 62, "CONTINUE", nullptr, tone == UI_DIM ? UI_CYAN : tone, victoryContinueCb);
}

static void battleFightCb(lv_event_t *e) {
  (void)e;
  // CONTINUE once it's over (or the other commander already ended a PvP
  // battle): close the battle kind that's still open, then the battle
  // report (v0.21.13) -- any reputation transmission waits for it.
  // v0.30.2: the copy is scoped so it's gone before showVictory() reads its own.
  bool over, closeOpen, pvp;
  {
    GameState gs = gameStateRead();
    over = gs.battleOver || (!gs.battleActive && battle_view.over);
    closeOpen = gs.battleActive && gs.battleOver;
    pvp = gs.battleKind == BATTLE_PVP;
  }
  if (over) {
    battle_anim_active = false;
    if (closeOpen) {
      if (pvp) gameStateEndPvpBattle();
      else gameStateEndTakeoverAfterBattle(MY_SLOT);
    }
    showVictory();
    return;
  }
  withdraw_armed = false;
  skipBattleAnim();  // FIGHT mid-animation = skip to the end, then fight
  gameStateBattleAction(MY_SLOT, 1);  // fight this exchange
  updateBattleUI();
}

static void battleWithdrawCb(lv_event_t *e) {
  (void)e;
  skipBattleAnim();
  if (!withdraw_armed) {
    withdraw_armed = true;  // first tap: arm + explain (updateBattleUI shows it)
    withdraw_armed_at = lv_tick_get();
  } else {
    withdraw_armed = false;
    gameStateBattleAction(MY_SLOT, 2);  // withdraw -- under fire
  }
  updateBattleUI();
}

static void showBattle() {
  currentScreen = SCR_BATTLE;
  onHomeScreen = false;
  onBattleScreen = true;
  battle_scene = nullptr;
  clearScreen();

  GameState gsHdr = gameStateRead();
  char sub[40];
  if (gsHdr.battleKind == BATTLE_PVP) {
    int otherSlot = (gsHdr.battleDefenderSlot == MY_SLOT) ? gsHdr.battleAttackerSlot : gsHdr.battleDefenderSlot;
    const char *otherName = (otherSlot >= 0 && otherSlot < MAX_PLAYERS) ? gsHdr.players[otherSlot].name : "ENEMY";
    snprintf(sub, sizeof(sub), "VS %s", otherName);
  } else {
    int p = gsHdr.battleTargetPlanet;
    snprintf(sub, sizeof(sub), "CONTESTING: %s", (p >= 0 && p < NUM_PLANETS) ? PLANETS[p].name : "?");
  }
  addHeader("BATTLE", sub);
  if (!rep_base_set) repBaseTake(gsHdr);  // v0.29.0: the battle report compares against this

  // Joining mid-battle shows the current state without replaying anything.
  fillBattleView(gsHdr);
  battle_anim_active = false;
  withdraw_armed = false;

  battle_scene = lv_obj_create(screen_root);
  lv_obj_remove_style_all(battle_scene);
  lv_obj_set_pos(battle_scene, SCENE_X, SCENE_Y);
  lv_obj_set_size(battle_scene, SCENE_W, SCENE_H);
  lv_obj_clear_flag(battle_scene, LV_OBJ_FLAG_SCROLLABLE);  // scrollable-by-default swallowed taps once
  lv_obj_add_flag(battle_scene, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(battle_scene, battleSceneDrawCb, LV_EVENT_DRAW_MAIN, nullptr);
  lv_obj_add_event_cb(battle_scene, battleSceneTapCb, LV_EVENT_CLICKED, nullptr);

  // Result text: Montserrat 14 so a full result sentence wraps to <= 2
  // lines in the gap between the scene and the buttons.
  battle_label = lv_label_create(screen_root);
  lv_label_set_text(battle_label, "");
  lv_obj_set_style_text_font(battle_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(battle_label, lv_color_hex(0xD7F7FF), 0);
  lv_obj_set_style_text_align(battle_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_width(battle_label, 360);
  lv_obj_align(battle_label, LV_ALIGN_TOP_MID, 0, SCENE_Y + SCENE_H + 4);

  // Buttons: farthest corners (73,376)/(393,376) are ~215px from center,
  // inside the ~227px round-screen touch limit (Convention 5).
  battle_button = lv_btn_create(screen_root);
  lv_obj_set_pos(battle_button, 73, 326);
  lv_obj_set_size(battle_button, 150, 50);
  styleButton(battle_button);
  lv_obj_t *bl = lv_label_create(battle_button);
  lv_label_set_text(bl, "FIGHT");
  lv_obj_center(bl);
  lv_obj_add_event_cb(battle_button, battleFightCb, LV_EVENT_CLICKED, nullptr);

  withdraw_button = lv_btn_create(screen_root);
  lv_obj_set_pos(withdraw_button, 243, 326);
  lv_obj_set_size(withdraw_button, 150, 50);
  styleButton(withdraw_button);
  lv_obj_t *wl = lv_label_create(withdraw_button);
  lv_label_set_text(wl, "WITHDRAW");
  lv_obj_center(wl);
  lv_obj_add_event_cb(withdraw_button, battleWithdrawCb, LV_EVENT_CLICKED, nullptr);

  // v0.28.0 (playtest: "If I get attacked by a player who picked to use two
  // fleets, i should be able to too"): the PvP defender may bring another
  // fleet that shares the space, before the first exchange. Pill 200x36 at
  // (133,386): corners ~214 px from center.
  reinforce_button = nullptr;
  {
    const int mask = snapshotReinforceMask(gsHdr, MY_SLOT);
    int f = -1;
    for (int k = 0; k < NUM_FLEETS; k++) if (mask & (1 << k)) { f = k; break; }
    if (f >= 0) {
      char t[32];
      snprintf(t, sizeof(t), "+ BRING FLEET %d", f + 1);
      reinforce_button = uiPill(screen_root, 133, 386, 200, 36, t, nullptr, 0x8CE7A0, reinforceCb);
      lv_obj_set_user_data(reinforce_button, (void *)(intptr_t)f);
    }
  }
  if (!battle_timer) battle_timer = lv_timer_create(battleTick, 150, nullptr);
  if (!battle_anim_timer) battle_anim_timer = lv_timer_create(battleAnimTick, 33, nullptr);
  updateBattleUI();
}

static void backToHome(lv_event_t *e) {
  (void)e;
  showHome();
}

static void reinforceCb(lv_event_t *e) {  // v0.28.0
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_current_target(e);
  gameStateReinforce(MY_SLOT, (int)(intptr_t)lv_obj_get_user_data(btn));
  showBattle();
}

// ---------------------------------------------------------------------------
// 3.1-dev.110 THE BOOT WATCHDOG (2026-11-21: three times after a USB flash the table drew its screen but never finished
// starting Wi-Fi / the web server -- before loop(), where the loop watchdog only begins). If loop() hasn't started
// BOOT_WATCH_S seconds after power-up, restart (the save resumes the game); the next boot says so on serial.
#include <esp_timer.h>
#define BOOT_WATCH_S 120  // 3.1-dev.111: room for the login retries
static volatile bool g_bootDone = false;
static RTC_NOINIT_ATTR uint32_t g_bootStuck;
static void bootWatchFire(void *) {
  if (g_bootDone) return;
  g_bootStuck = 0xB007DEAD;
  esp_restart();
}
static void bootWatchStart() {
  if (g_bootStuck == 0xB007DEAD) { Serial.println("[BOOT] the last start got stuck before the web server came up -- the boot watchdog restarted it"); }
  g_bootStuck = 0;
  esp_timer_create_args_t a = {}; a.callback = bootWatchFire; a.name = "bootwatch";
  esp_timer_handle_t h;
  if (esp_timer_create(&a, &h) == ESP_OK) esp_timer_start_once(h, (uint64_t)BOOT_WATCH_S * 1000000ULL);
}

void setup() {
  Serial.begin(115200);
  { Preferences dp; dp.begin("sfdisp", true); bsp_screen_off = dp.getBool("off", false); dp.end(); }  // 2026-11-14 SCREENOFF / SCREENON
  // 3.1-dev.74 (two crashes at another house, task tiT: newlib abort in lock_init_generic from uart_write): the console's
  // write lock is created lazily by the FIRST printf -- if that first print comes from the Wi-Fi stack while internal RAM is at
  // 0 KB, creating the lock fails and the chip aborts. Print once now so the lock exists before any phone connects.
  printf("\n"); fflush(stdout); fprintf(stderr, "\n"); fflush(stderr);
  // ESP32-S3's native USB CDC has a well-documented issue: without this,
  // Serial.print()/println() BLOCK indefinitely once the TX buffer fills,
  // if no host (like the Arduino Serial Monitor) is attached to drain it --
  // hanging the entire board on boot until you open a serial connection.
  // This makes Serial writes non-blocking (silently dropped instead of
  // hanging) when nothing is listening. Official fix, added to arduino-esp32
  // specifically for this failure mode.
  Serial.setTxTimeoutMs(0);
  delay(1000);
  Serial.println("=====================================");
  Serial.print("Shadow Fleet Prototype 02  ");
  Serial.print(FIRMWARE_VERSION);
  Serial.print("  (");
  Serial.print(FIRMWARE_DATE);
  Serial.println(")");
  // Every gameStateRead() stack-copies this whole struct, and some paths
  // hold two at once against 16KB task stacks -- watch this number as the
  // state grows (also the ESP-NOW sync-format budget, see the design notes).
  Serial.println("Board: " SF_BOARD_NAME "  Role: " SF_ROLE_NAME);
  Serial.printf("sizeof(GameState) = %u bytes\n", (unsigned)sizeof(GameState));
  Serial.println("=====================================");
  healthBoot();  // 2.0-dev.6: why the last run ended + any crash record (sf_health.h)
  bootWatchStart();  // 3.1-dev.110: a start that never reaches loop() (stuck joining Wi-Fi) restarts itself

  randomSeed((uint32_t)esp_random());
  randomizeStars();

  gameStateInit();
#if SF_ROLE == SF_ROLE_TABLE
  // v0.30.3: a game that was running when the table went down picks up where
  // it left off; phones and consoles rejoin their seats.
  gameStateRestoreSaved();
#endif

  bsp_broolesia_display_init();
#if SF_ROLE != SF_ROLE_TABLE
  bsp_set_touch_hook(edgeSwipeHook);  // 2.0-dev.7: swipe in from the left edge = back
#endif

  if (bsp_display_lock(-1) == ESP_OK) {
    screen_root = lv_scr_act();
#if SF_ROLE == SF_ROLE_TABLE
    { GameState gs0 = gameStateRead(); showTable(gs0); }
#else
    showStart();
#endif
    bsp_display_unlock();
  }

  delay(400);
  // v0.30.4: under the display lock. The panel IO isn't thread-safe: a resumed
  // game (v0.30.3) has the table's glow renderer streaming frames from the
  // LVGL task right away, and this command from setup() collided with them --
  // setup() hung here ("[BSP] brightness_set" never printed, Wi-Fi never
  // started) and every reboot resumed into the same hang.
  if (bsp_display_lock(-1) == ESP_OK) {
    bsp_display_brightness_set(100);
    bsp_display_unlock();
  }
  sfAudioBegin();  // v0.25.3: the 1.75's speaker (a no-op on the 1.43")

  {  // console id from the MAC (see MY_CLIENT_ID)
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(MY_CLIENT_ID, sizeof(MY_CLIENT_ID), "CONSOLE-%02X%02X%02X", mac[3], mac[4], mac[5]);
  }
#if SF_ROLE == SF_ROLE_TABLE
  networkInit(true);                    // the table always hosts: home Wi-Fi if saved + reachable, else its own network
  linkBeginTable(networkOnHomeWifi());  // ...and the ESP-NOW link for command consoles
  table_shown_key = 0xFFFFFFFFu;        // redraw the Lobby now that its QR code / Wi-Fi mode is known
#else
  // v0.22.1: a command console first listens for a table console. Found ->
  // join it as a player (no Wi-Fi network of its own; phones use the
  // table's). None -> host the game itself, as before (and let another
  // console join it the same way).
  if (!linkTryJoinTable(5000)) {   // v0.22.5: 5 s -- it may need to sweep the router's channels
    networkInit(false);
    linkBeginTable(false);
  }
#endif

  if (!star_timer) {
    star_timer = lv_timer_create(starTick, 180, nullptr);
  }
}

// 2.0 test tool, per-object draw profiler: "P" times every object on the
// screen (its own drawing, DRAW_MAIN_BEGIN..END -- children not included),
// "R" redraws the whole screen, "Q" prints the costliest objects.
static const int PROF_MAX = 96;
static lv_obj_t *prof_obj[PROF_MAX];
static uint32_t prof_us[PROF_MAX], prof_t0 = 0;
static int prof_n = 0;
static void profEventCb(lv_event_t *e) {
  const lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_DRAW_MAIN_BEGIN) prof_t0 = micros();
  else if (c == LV_EVENT_DRAW_MAIN_END) prof_us[(int)(intptr_t)lv_event_get_user_data(e)] += micros() - prof_t0;
}
static void profTag(lv_obj_t *o) {
  if (prof_n >= PROF_MAX) return;
  const int i = prof_n++;
  prof_obj[i] = o;
  prof_us[i] = 0;
  lv_obj_add_event_cb(o, profEventCb, LV_EVENT_DRAW_MAIN_BEGIN, (void *)(intptr_t)i);
  lv_obj_add_event_cb(o, profEventCb, LV_EVENT_DRAW_MAIN_END, (void *)(intptr_t)i);
  for (uint32_t k = 0; k < lv_obj_get_child_count(o); k++) profTag(lv_obj_get_child(o, k));
}
static void profReport() {
  Serial.printf("[PROF] %d objects\n", prof_n);
  for (int shown = 0; shown < 14; shown++) {
    int best = -1;
    for (int i = 0; i < prof_n; i++) if (prof_us[i] && (best < 0 || prof_us[i] > prof_us[best])) best = i;
    if (best < 0) break;
    lv_obj_t *o = prof_obj[best];
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    const lv_obj_class_t *k = lv_obj_get_class(o);
    const char *kind = k == &lv_label_class ? "label" : k == &lv_image_class ? "image" : k == &lv_button_class ? "button" : "obj";
    Serial.printf("[PROF] %6u us  %-6s (%d,%d)-(%d,%d)%s\n", (unsigned)prof_us[best], kind, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2,
                  k == &lv_label_class ? lv_label_get_text(o) : "");
    prof_us[best] = 0;
  }
  prof_n = 0;
}

// 2.0 test tool: "T x y" taps, "D x0 y0 x1 y1 ms" drags (bsp_inject_touch),
// typed into the serial port -- for walking screens and timing them remotely.
static void serialTouchPoll() {
  static char line[48];
  static int n = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c != '\n' && c != '\r') { if (n < (int)sizeof(line) - 1) line[n++] = c; continue; }
    line[n] = 0;
    n = 0;
    int a, b, c2, d, ms;
    if (sscanf(line, "T %d %d", &a, &b) == 2) { bsp_inject_touch(a, b, a, b, 80); Serial.printf("[TAP] %d %d\n", a, b); }
    else if (sscanf(line, "D %d %d %d %d %d", &a, &b, &c2, &d, &ms) == 5) { bsp_inject_touch(a, b, c2, d, ms); Serial.printf("[DRAG] %d %d -> %d %d in %d ms\n", a, b, c2, d, ms); }
    else if (strcmp(line, "CRASH!") == 0) { Serial.println("[HEALTH] crashing on purpose (test)"); delay(50); abort(); }  // checks the crash record + rejoin
    else if (strcmp(line, "FREEZE!") == 0) { Serial.println("[HEALTH] freezing on purpose (test: the watchdog should restart us)"); for (;;) delay(100); }
    else if (strcmp(line, "HEALTH") == 0) Serial.print(healthReport());
    else if (sscanf(line, "PVPWIN %d", &a) == 1 && a >= 0 && a < MAX_PLAYERS) { gameStateCreditPvpWin(a); Serial.printf("[PROMISE] credited a PvP win to P%d\n", a + 1); }  // 3.1-dev.113
    else if (strcmp(line, "TABLEWIFI") == 0) { Serial.println("[NET] switching to the table's own Wi-Fi (serial)"); networkSwitchMode(false); }  // 3.1-dev.31
    else if (strcmp(line, "SCREENOFF") == 0 || strcmp(line, "SCREENON") == 0) {  // 2026-11-14: saved on this board only
      const bool off = line[6] == 'O' && line[7] == 'F';
      Preferences dp; dp.begin("sfdisp", false); dp.putBool("off", off); dp.end();
      Serial.printf("[BSP] screen %s on this board -- restarting\n", off ? "OFF" : "ON");
      delay(100); ESP.restart();
    }
    else if (strcmp(line, "HOMEWIFI") == 0) { Serial.println("[NET] switching to home Wi-Fi (serial)"); networkSwitchMode(true); }
    else if (line[0] == 'P' || line[0] == 'R' || line[0] == 'Q') {
      if (bsp_display_lock(500) != ESP_OK) continue;
      if (line[0] == 'P') { prof_n = 0; profTag(lv_screen_active()); Serial.printf("[PROF] tagged %d objects\n", prof_n); }
      else if (line[0] == 'R') lv_obj_invalidate(lv_screen_active());
      else profReport();
      bsp_display_unlock();
    }
  }
}

void loop() {
  static bool watched = false;  // 2.0-dev.6: a loop stuck HEALTH_WDT_S seconds restarts the console
  if (!watched) { healthWatchThisTask(); watched = true; g_bootDone = true; }  // 3.1-dev.110: the boot watchdog stands down
  healthFeed();
  static uint32_t lastHealth = 0;
  if (millis() - lastHealth > 1000) { lastHealth = millis(); healthTick(); }
  serialTouchPoll();
  if (linkIsJoined()) {
    linkCommandLoop(MY_SLOT);   // mirror of a table console: no server here (2.0-dev.13: + "still here")
  } else {
    networkHandle();     // hosting (command consoles are fed by the link task, v0.23.2)
    static uint32_t lastTick = 0;  // v0.28.3: host housekeeping (stuck battles)
    if (millis() - lastTick > 500) { lastTick = millis(); gameStateTick(); }
#if SF_ROLE == SF_ROLE_TABLE
    gameStateSaveTaskStart();  // 2.0-dev.17: saves run on their own task (was gameStateSaveTick() here -- held up the web server)
#endif
  }

  // If the HOST becomes involved in a PvP battle (as attacker OR defender)
  // and isn't already on the battle screen, jump to it. This is how being
  // attacked by a phone player pulls the AMOLED into the fight.
  if (currentScreen != SCR_BATTLE) {
    GameState gs = gameStateRead();
    if (gs.battleKind == BATTLE_PVP && gs.battleActive
        && (gs.battleAttackerSlot == MY_SLOT || gs.battleDefenderSlot == MY_SLOT)) {
      if (bsp_display_lock(50) == ESP_OK) {
        showBattle();
        bsp_display_unlock();
      }
    }
  }

  delay(20);
}
