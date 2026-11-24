#ifndef BOARD_BSP_H
#define BOARD_BSP_H

#include <driver/i2c_master.h>
#include "lvgl.h"
#include "board_select.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  lv_indev_t *indev;
  lv_display_t *disp;
} bsp_lvgl_t;
void bsp_lcd_init(void);
void bsp_touch_init(i2c_master_bus_handle_t i2c_bus_);
esp_err_t bsp_display_brightness_init(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
extern bool bsp_screen_off;  // 2026-11-14: SCREENOFF over serial (saved on THIS board): panel dark, no glow renderer
int bsp_display_brightness_get(void);
esp_err_t bsp_display_lock(int32_t timeout_ms);
void bsp_display_unlock(void);
bsp_lvgl_t bsp_broolesia_display_init(void);
bsp_lvgl_t bsp_get_broolesia_display(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
void bsp_batt_init(void);
uint16_t bsp_batt_get_voltage(void);
uint8_t bsp_batt_get_status(void);
esp_err_t bsp_spiffs_mount(void);


#ifdef __cplusplus
}
#endif


#endif
// v0.23.4 diagnostics: microseconds spent in the UI's heaviest draw routines
// since the last "[UI] 5s" line (filled by timed wrappers in the .ino).
enum { UI_T_RIM, UI_T_STARS, UI_T_PLANET, UI_T_STATIC, UI_T_COUNT };
extern uint32_t g_uiDrawUs[UI_T_COUNT];

// v0.26.0 -- a custom full-screen renderer can take the display from LVGL
// (the Table Console's glow renderer, table_render.cpp). While a frame hook
// is set, the LVGL task calls it after each lv_timer_handler(); LVGL's own
// flushes are dropped (its buffers are reused for the renderer's strips).
// The hook renders a whole frame with bsp_frame_strip() and returns only when
// every strip has been sent.
#ifdef __cplusplus
extern "C" {
#endif
typedef uint32_t (*bsp_frame_hook_t)(void);       // returns ms to wait before the next frame
void bsp_inject_touch(int x0, int y0, int x1, int y1, uint32_t ms);  // test tool: press / drag x0,y0 -> x1,y1 for ms
typedef void (*bsp_touch_hook_t)(int x, int y, bool pressed);   // every touch sample, in the LVGL task
void bsp_set_touch_hook(bsp_touch_hook_t hook);
void bsp_set_frame_hook(bsp_frame_hook_t hook);   // nullptr = LVGL owns the screen again (it redraws everything)
// Strip buffers for the hook: two DMA buffers of BSP_STRIP_ROWS full-width rows
// (LVGL's own draw buffers). v0.27.3: 20 rows on the TABLE -- the first
// playtest of the glow renderer dipped its free internal RAM to 1 KB while
// phones polled; 20 rows frees ~37 KB. Command consoles keep 40.
#ifndef BSP_STRIP_ROWS  // -DBSP_STRIP_ROWS=n for frame-rate tests (tools/perf_ab.ps1)
#if SF_ROLE == SF_ROLE_TABLE
#define BSP_STRIP_ROWS 20
#else
#define BSP_STRIP_ROWS 40
#endif
#endif
uint16_t *bsp_strip_buffer(int k);                // k = 0 or 1
void bsp_frame_strip(int k, int y0, int rows);    // send buffer k (pre-byte-swapped RGB565) to rows y0.., returns at once
void bsp_frame_wait(int k);                       // block until buffer k is free again
#ifdef __cplusplus
}
#endif
