#pragma once

// ===========================================================================
// Which Waveshare round AMOLED board this build targets. Both are 466x466
// and run the same LVGL UI; only the display/touch driver layer differs
// (see bsp_config.h for pins, board_bsp.cpp for the #if SF_BOARD sections).
//
//   SF_BOARD_AMOLED_175  ESP32-S3-Touch-AMOLED-1.75(-B): CO5300 + CST9217
//   SF_BOARD_AMOLED_143  ESP32-S3-Touch-AMOLED-1.43:     SH8601 + FT-series
//
// Default is the 1.75" (the main board since 2026-09-28). Build for the
// 1.43" without editing this file by passing the flag on the command line:
//   arduino-cli compile ... --build-property "compiler.cpp.extra_flags=-DSF_BOARD=143"
// (In the Arduino IDE, change the default below instead.)
// ===========================================================================
#define SF_BOARD_AMOLED_143 143
#define SF_BOARD_AMOLED_175 175

#ifndef SF_BOARD
#define SF_BOARD SF_BOARD_AMOLED_175
#endif

#if SF_BOARD != SF_BOARD_AMOLED_143 && SF_BOARD != SF_BOARD_AMOLED_175
#error "SF_BOARD must be 143 or 175"
#endif

#if SF_BOARD == SF_BOARD_AMOLED_175
#define SF_BOARD_NAME "AMOLED-1.75"
#else
#define SF_BOARD_NAME "AMOLED-1.43"
#endif

// Role (v0.22.0): a PLAYER board is a seat at the table (and hosts when it's
// alone); the TABLE host owns the game for phones and shows the shared galaxy
// clock, never a seat. The 1.43" defaults to TABLE (user 2026-10-15: "1.43
// isn't going to be a player anymore"); override with -DSF_ROLE=1 or 2.
#define SF_ROLE_PLAYER 1
#define SF_ROLE_TABLE  2
#ifndef SF_ROLE
#if SF_BOARD == SF_BOARD_AMOLED_143
#define SF_ROLE SF_ROLE_TABLE
#else
#define SF_ROLE SF_ROLE_PLAYER
#endif
#endif
#if SF_ROLE == SF_ROLE_TABLE
#define SF_ROLE_NAME "TABLE HOST"
#else
#define SF_ROLE_NAME "PLAYER"
#endif
