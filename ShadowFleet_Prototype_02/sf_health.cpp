// sf_health.cpp -- see sf_health.h.
#include "sf_health.h"
#include "version.h"
#include <Preferences.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_core_dump.h>
#include <esp_heap_caps.h>

// Breadcrumbs: RTC slow memory isn't cleared by a crash / watchdog reset.
struct HealthCrumbs {
  uint32_t magic;
  uint32_t upS;        // seconds the run had been up at the last tick
  uint32_t minHeapKb;  // lowest free internal RAM it saw
};
static RTC_NOINIT_ATTR HealthCrumbs crumbs;
struct LowRec { uint32_t up; uint16_t kb; char ctx[48]; };
// 3.1-dev.46: the lows live in RTC memory, so a restart (flash, crash, watchdog -- not a power cut) keeps the last run's
static RTC_NOINIT_ATTR LowRec lows[6];
static RTC_NOINIT_ATTR int lowN;
static LowRec prevLows[6];
static int prevLowN = 0;
static const uint32_t CRUMB_MAGIC = 0x5F4C4554;
static uint32_t lastUpS = 0, lastMinHeapKb = 0;
static bool lastValid = false;
static esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;

const char *healthResetReason() {
  switch (bootReason) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "restart (software)";
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "FROZE (task watchdog)";
    case ESP_RST_WDT: return "CRASH (watchdog)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power dipped)";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb reset";
    case ESP_RST_JTAG: return "jtag";
    default: return "unknown";
  }
}

static bool isTrouble(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

// Keeps the last HEALTH_KEEP records: key "n" = how many ever, "r<k>" = record k % KEEP.
static void storeRecord(const String &rec) {
  Preferences p;
  if (!p.begin("sfhealth", false)) return;
  const uint32_t n = p.getUInt("n", 0);
  char key[8];
  snprintf(key, sizeof(key), "r%u", (unsigned)(n % HEALTH_KEEP));
  p.putString(key, rec);
  p.putUInt("n", n + 1);
  p.end();
}

void healthBoot() {
  bootReason = esp_reset_reason();
  if (crumbs.magic == CRUMB_MAGIC) {
    lastValid = true; lastUpS = crumbs.upS; lastMinHeapKb = crumbs.minHeapKb;
    if (lowN > 0 && lowN <= 6) { prevLowN = lowN; memcpy(prevLows, lows, sizeof(prevLows)); for (int k = 0; k < prevLowN; k++) prevLows[k].ctx[47] = 0; }
  }
  lowN = 0;
  crumbs.magic = CRUMB_MAGIC;
  crumbs.upS = 0;
  crumbs.minHeapKb = 0;

  Serial.printf("[HEALTH] last run ended: %s", healthResetReason());
  if (lastValid) Serial.printf(" (it was up %u s, lowest free internal RAM %u KB)", (unsigned)lastUpS, (unsigned)lastMinHeapKb);
  Serial.println();

  String rec;
  if (esp_core_dump_image_check() == ESP_OK) {
    esp_core_dump_summary_t *sum = (esp_core_dump_summary_t *)malloc(sizeof(esp_core_dump_summary_t));
    if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
      char buf[96];
      snprintf(buf, sizeof(buf), "task %s pc 0x%08x cause %u addr 0x%08x bt", sum->exc_task, (unsigned)sum->exc_pc,
               (unsigned)sum->ex_info.exc_cause, (unsigned)sum->ex_info.exc_vaddr);
      rec = buf;
      for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; i++) {
        snprintf(buf, sizeof(buf), " 0x%08x", (unsigned)sum->exc_bt_info.bt[i]);
        rec += buf;
      }
      if (sum->exc_bt_info.corrupted) rec += " (corrupted)";
    }
    free(sum);
    esp_core_dump_image_erase();  // report each crash once
  }
  if (isTrouble(bootReason) || rec.length()) {
    char head[120];
    snprintf(head, sizeof(head), "%s %s; up %u s, min RAM %u KB; ", FIRMWARE_VERSION, healthResetReason(),
             (unsigned)lastUpS, (unsigned)lastMinHeapKb);
    const String full = String(head) + (rec.length() ? rec : String("no core dump"));
    Serial.println("[HEALTH] " + full);
    storeRecord(full);
  }

  // Stricter than the core's default (5 s, idle tasks only): our own tasks
  // subscribe with healthWatchThisTask(); a stuck one restarts the console.
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = HEALTH_WDT_S * 1000;
  cfg.idle_core_mask = 1;  // keep watching core 0's idle task, as the core does
  cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&cfg);
}

void healthWatchThisTask() {
  if (esp_task_wdt_status(nullptr) != ESP_OK) esp_task_wdt_add(nullptr);
}

void healthFeed() { esp_task_wdt_reset(); }

static uint32_t lowSeen = 0xFFFFFFFF;
bool healthLowDropped() {
  const uint32_t m = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  if (lowSeen != 0xFFFFFFFF && m + 1024 > lowSeen) return false;
  if (lowSeen == 0xFFFFFFFF) { lowSeen = m; return false; }
  lowSeen = m;
  return true;
}
void healthLowNote(const char *ctx) {
  for (int k = min(lowN, 5); k > 0; k--) lows[k] = lows[k - 1];
  lows[0].up = millis() / 1000; lows[0].kb = (uint16_t)(lowSeen / 1024);
  snprintf(lows[0].ctx, sizeof(lows[0].ctx), "%s", ctx ? ctx : "");
  if (lowN < 6) lowN++;
  Serial.printf("[RAM] new low %u KB at %u s: %s\n", (unsigned)lows[0].kb, (unsigned)lows[0].up, lows[0].ctx);
}
void healthTick() {
  if (healthLowDropped()) healthLowNote("background");
  crumbs.upS = millis() / 1000;
  crumbs.minHeapKb = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
}

String healthReport() {
  String s = String("Shadow Fleet ") + FIRMWARE_VERSION + "\n";
  s += String("up ") + (millis() / 1000) + " s; free internal RAM " + (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) +
       " KB (lowest " + (heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024) + " KB)\n";
  s += String("this boot: ") + healthResetReason();
  if (lastValid) s += String(" (the run before was up ") + lastUpS + " s, lowest RAM " + lastMinHeapKb + " KB)";
  if (lowN) {
    s += "\nRAM lows this boot (newest first):";
    for (int k = 0; k < lowN; k++) s += String("\n  ") + lows[k].kb + " KB at " + lows[k].up + " s: " + lows[k].ctx;
  }
  if (prevLowN) {
    s += "\nRAM lows the run before (newest first):";
    for (int k = 0; k < prevLowN; k++) s += String("\n  ") + prevLows[k].kb + " KB at " + prevLows[k].up + " s: " + prevLows[k].ctx;
  }
  s += "\n\nlast problems (newest first):\n";
  Preferences p;
  if (p.begin("sfhealth", true)) {
    const uint32_t n = p.getUInt("n", 0);
    if (!n) s += "  none\n";
    for (uint32_t i = 0; i < n && i < (uint32_t)HEALTH_KEEP; i++) {
      char key[8];
      snprintf(key, sizeof(key), "r%u", (unsigned)((n - 1 - i) % HEALTH_KEEP));
      s += "  " + p.getString(key, "?") + "\n";
    }
    p.end();
  }
  return s;
}
