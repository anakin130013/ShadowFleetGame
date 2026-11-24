// sf_health.h -- crash capture + watchdog for both consoles (v2.0-dev.6).
//
// User 2026-10-30: the Table Console should be "more robust, crashes less,
// and recovers from crashes". What this gives:
//   * every boot prints WHY the last run ended (power, crash, watchdog,
//     brownout...) and, after a crash, the crashing task + address +
//     backtrace from the core dump the core already writes to flash
//     (the `coredump` partition); the last few are kept in NVS ("sfhealth")
//     so a crash with no laptop attached is still diagnosable -- decode with
//     xtensa-esp32s3-elf-addr2line -pfiaC -e build143/ShadowFleet_Prototype_02.ino.elf <addresses>
//   * breadcrumbs in RTC memory (survive a crash, not a power cut): how long
//     the last run was up and its lowest free internal RAM
//   * the LVGL, link and loop tasks report to the task watchdog: a task
//     stuck for HEALTH_WDT_S seconds restarts the console (the table then
//     resumes its saved game; consoles and phones rejoin)
//   * healthReport() -- the same as text, served by the table at /health
#pragma once
#include <Arduino.h>

static const int HEALTH_WDT_S = 30;  // 3.1-dev.34 (was 8): a slow phone on a weak Wi-Fi can hold the web task in ONE send (the ~280 KB page) longer than 8 s -- that read as a freeze and restarted the table (playtest 2026-11-08, a busy router)    // stuck this long -> restart (normal passes are < 0.5 s)
static const int HEALTH_KEEP = 4;     // crash records kept in NVS

void healthBoot();                    // first thing in setup()
void healthWatchThisTask();           // subscribe the calling task to the watchdog
void healthFeed();                    // call every pass of that task's loop
void healthTick();                    // ~once a second from loop(): breadcrumbs
const char *healthResetReason();      // this boot's reset reason, short
String healthReport();                // text for /health and serial
// 3.1-dev.44 RAM LOW WATCH (playtest 4: the table hit 0 KB free and nobody knew when): every new low of free internal RAM
// (>= 1 KB below the last) is recorded with the uptime and what the table was doing (the web request just served, or
// "background"), printed as "[RAM] new low ..." and listed on /health.
bool healthLowDropped();              // cheap: true when the minimum fell since the last call
void healthLowNote(const char *ctx);  // record it
