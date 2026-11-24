// 3.1-dev.25 THE TABLE'S SCREENS (canvas https://claude.ai/artifact/7fLBrkn6uC5bPzA7ESFgWW), split
// out of the .ino so tools/table_harness can draw every screen on a PC and save it as a picture
// (user 2026-11-07: "is there any way to make a harness so you can see what it looks like??").
//
// How the table draws: the glow renderer (table_render.cpp) lights rings in ONE mood color; the
// sharp parts -- symbols, dice, numbers, words -- go into an 8-bit overlay (0 = nothing, ~120 = the
// mood color itself, 255 = white-hot). WORDS are drawn upright once by LVGL into a scratch buffer and
// then placed as a whole -- straight, turned to face a seat, or bent round the rim -- with smooth
// (bilinear) sampling: placing letters one by one gave uneven spacing ("TH ANE"). NUMBERS are the
// phones' 5x7 pixel numerals, written straight into the overlay.
#pragma once
#include "lvgl.h"
#include "game_state.h"
#include "table_render.h"

struct TableView {
  bool tumble;      // the battle dice are tumbling right now
  bool alert;       // a public moment is showing
  bool first;       // "who goes first" is playing
};

int tableAngle(int faction);              // screen degrees (0 = 3 o'clock, clockwise) toward a faction's hangar
lv_point_t tablePolar(float r, float deg);

// Fills tr_scene and the overlay for the game screens (everything but the Lobby and the Ashen
// Fleet); returns the TrMode shown, -1 if the overlay could not be allocated. The caller sets
// tr_scene.preFrame (the planet) and calls trStart().
int tableCompose(const GameState &gs, const TableView &v);
void tableComposeReset();                 // the next tableCompose restarts its intro (after the Lobby / Ashen screens)

// THE LOBBY: built from LVGL objects on `root` (the code, the Wi-Fi button) + one image of the words.
struct TableLobbyEnv {
  bool homeWifi, homeSaved, homeFailed;
  const char *gameUrl, *homeSsid;
  const char *failWhy;                    // 3.1-dev.62
  lv_event_cb_t wifiSwitchCb;             // the TABLE WI-FI / HOME WI-FI button
};
void tableLobbyBuild(lv_obj_t *root, const GameState &gs, const TableLobbyEnv &env);
