// 3.1-dev.25: small drawing helpers shared by the sketch (.ino) and the table's screens
// (table_scenes.cpp), split out so the table preview harness (tools/table_harness) can build
// the table's drawing code on a PC. Moved from the .ino unchanged.
#pragma once
#include <stdint.h>
#include "lvgl.h"

// Faction colors / names (moved from the .ino).
static const uint32_t FACTION_RING_COLOR[4] = { 0xFFC933, 0xE6EEF5, 0x9B5FE0, 0xD0343E };
static const uint32_t FACTION_TEXT_COLOR[4] = { 0xFFC933, 0xD4DEE8, 0xB98CF0, 0xE0707A };
static const char *const FACTION_SHORT[4] = { "REBELS", "EMPIRE", "PYKES", "CRIMSON DAWN" };

static inline bool layerSees(const lv_layer_t *layer, int x1, int y1, int x2, int y2) {
  const lv_area_t &c = layer->_clip_area;
  return x2 >= c.x1 && x1 <= c.x2 && y2 >= c.y1 && y1 <= c.y2;
}
void uiLine(lv_layer_t *layer, int x1, int y1, int x2, int y2, lv_color_t c, int w);
void uiDisc(lv_layer_t *layer, int cx, int cy, int r, lv_color_t c);
extern int ui_insignia_gray;  // >= 0: uiDrawInsignia draws in that gray (the table's glow overlay)
// `bg` = the color behind the insignia (the crescent's bite is drawn in it).
void uiDrawInsignia(lv_layer_t *layer, int faction, int cx, int cy, int r, uint32_t bg);
