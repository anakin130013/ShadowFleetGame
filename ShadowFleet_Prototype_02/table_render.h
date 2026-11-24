// Table Console glow renderer (v0.26.0; hangar gates v0.27.0). The table's
// game screens (galaxy clock, big-moment alerts, who goes first, victory) are
// drawn by a small radial "shader" instead of LVGL -- the approach from the
// user's Clutch prop (clutch_core.ino, 2026-10-16): every pixel's radius and
// angle are precomputed once, each frame is built from 1-D lookup tables
// (radial glow, band profiles, angular masks) and ONE mood color whose tone
// map blooms to white. The sharp parts (symbols, numbers, words, dice, ships)
// are drawn by LVGL into an 8-bit overlay that the renderer lights in the
// same color. The Lobby stays LVGL (QR code + button). Table build only.
//
// v0.27.0 layout (user 2026-10-16 picked "hangar gates", mockup
// https://claude.ai/artifact/QcRRMgmmA7R5MiA8Nc8gNm): each player's seat is a
// GATE in a ring (r 158) -- two door arcs at r 134 / 182 that slide shut when
// they lock in -- with a standings bar at r 204 (shape only, no numbers), the
// round segments on the rim (r 222), the phase ring (r 98) and the round /
// phase / dice in the center.
#pragma once
#include <stdint.h>

enum TrMode : uint8_t { TR_CLOCK, TR_ALERT, TR_FIRST, TR_VICTORY };
enum TrSpotKind : uint8_t { TR_SPOT_BREATHE, TR_SPOT_PULSE, TR_SPOT_LAND };
enum TrSeatState : uint8_t { TR_SEAT_EMPTY, TR_SEAT_OPEN, TR_SEAT_WAITING, TR_SEAT_SHUT };

struct TrSpot { int16_t x, y; uint8_t kind; };   // a glow ring around a point
struct TrBolt { int16_t x1, y1, x2, y2; };       // blaster bolts flying x1,y1 -> x2,y2
struct TrSeat {
  int16_t angle;      // screen degrees (0 = 3 o'clock, clockwise) of the gate's middle
  uint8_t half;       // gate half-width, degrees
  uint8_t barHalf;    // standings bar half-length at the leader's score, degrees
  uint8_t barFrac;    // this player's score / the leader's, 0-255
  uint8_t state;      // TrSeatState (doors: open / breathing / shut)
  bool dim, fight, top, first;  // others while a battle runs / in the battle / the leader / lands the comet
  bool away;          // 2.0-dev.13: an AWAY player -- a dashed gate (as TR_SEAT_EMPTY) but their bar stays, dimmed
  uint32_t color;     // 2.0-dev.11: a locked gate glows in this (0xRRGGBB; 0 = the mood color)
};

struct TrScene {
  uint8_t mode;              // TrMode
  uint32_t color;            // the mood color (0xRRGGBB); the screen eases to it
  uint32_t modeStart;        // millis() when this mode began (intros, the comet)
  int8_t rimTotal, rimCurrent;
  bool rimFinal;             // the final round: its rim segment pulses
  int8_t phase;              // lit third of the phase ring (0..2), -1 = ring off
  bool noPhaseRing;          // 3.1-dev.23: phase still tracked (a change = the hyperspace jump) but the ring is not drawn
  bool ripple;               // rings spreading from the center (a planet under attack)
  bool fireworks;            // TR_VICTORY
  bool halves;               // 2 players: a divider between the two halves
  int16_t cometAngle;        // TR_FIRST: where the comet lands (screen deg)
  int8_t seatCount;
  TrSeat seat[4];
  int8_t spotCount;
  TrSpot spot[8];
  int8_t boltCount;
  TrBolt bolt[2];
  int8_t planet;             // >= 0: a pixel planet in the center
  uint8_t planetCell;        // its block size (28 blocks across)
  const uint8_t *planetPx;   // 28x28 palette indexes 0-3, 255 = off the disc (set by preFrame)
  void (*preFrame)(void);    // called at the start of every frame (e.g. turn the planet)
};
extern TrScene tr_scene;

void trStart();   // take the screen from LVGL (no-op if already running)
void trStop();    // give it back (the caller redraws its LVGL screen)
bool trActive();
// The overlay changed: 466 rows of `stride` bytes, 0 = nothing, 255 = white-hot.
void trOverlayChanged(const uint8_t *l8, int stride);
