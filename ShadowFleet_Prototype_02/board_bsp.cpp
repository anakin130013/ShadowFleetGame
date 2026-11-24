#include <esp_heap_caps.h>
#include <Arduino.h>
#include <string.h>
#include "board_bsp.h"
#include "sf_health.h"
#include "src/externLib/esp_lcd_sh8601.h"
#include "bsp_config.h"
#include "i2c_bsp.h"
#include "lvgl.h"
#if SF_BOARD == 143 && !(defined(SF_LV_MEM_PSRAM) && SF_LV_MEM_PSRAM)
#error "3.1-dev.112: the table builds with LVGL's memory in PSRAM -- keep the sketch's build_opt.h and copy arduino/lv_conf.h into your Arduino libraries folder (see README)"
#endif

#include <esp_adc/adc_oneshot.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_ops.h>
#include <esp_spiffs.h>
#include <esp_vfs_fat.h>
#include <esp_timer.h>

#define TAG "DISP_BSP"

static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_panel_handle_t panel_handle = NULL;
static uint8_t brightness;
static I2cMasterBus *i2c_bus = nullptr;
static i2c_master_dev_handle_t touch_dev_handle = NULL;
static bsp_lvgl_t result = { 0 };
static adc_cali_handle_t cali_handle;
static adc_oneshot_unit_handle_t adc1_handle;
static SemaphoreHandle_t lvgl_mux = NULL;


#define BYTES_PER_PIXEL (LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565))
#define BUFF_SIZE (LCD_WIDTH * BSP_STRIP_ROWS * BYTES_PER_PIXEL)  // v0.27.3: 20 rows on the table, 40 elsewhere
// v0.26.0: custom renderer hand-off (see bsp_set_frame_hook in board_bsp.h).
static volatile bsp_frame_hook_t frame_hook = nullptr;
static uint8_t *lvgl_buf[2];
static SemaphoreHandle_t strip_free[2];
static volatile int strip_fifo[4], strip_head = 0, strip_tail = 0;
static volatile int lvgl_inflight = 0;
// 2.0 frame-rate work: LVGL used to spin (while(disp->flushing);) until each
// strip had gone to the panel. Now it sleeps on this semaphore, given by the
// transfer-done interrupt, so the CPU is free for the link / touch meanwhile.
static SemaphoreHandle_t lvgl_flush_sem = nullptr;
// Every 5 s: how long LVGL's refreshes took, how much of that was waiting on
// the panel, and how many pixels went out (see lvglHealthReport).
static volatile uint32_t perf_px = 0;
static uint32_t perf_flush_n = 0, perf_flush_us = 0;  // flush_cb calls + time inside them
static uint32_t perf_refr = 0, perf_refr_us = 0, perf_wait_us = 0, perf_max_us = 0, perf_max_px = 0, perf_refr_t0 = 0, perf_refr_px0 = 0;
#ifndef BSP_LCD_PCLK_MHZ
#define BSP_LCD_PCLK_MHZ 40
#endif

// ===========================================================================
// Board layer (see board_select.h). Everything in this file is shared by
// both boards EXCEPT the sections marked SF_BOARD: panel init commands +
// column gap, the 1.43" panel-enable pin, touch protocol + calibration, and
// the 1.43" battery ADC.
// ===========================================================================
#if SF_BOARD == SF_BOARD_AMOLED_175
// CO5300 (1.75"), driven by the SH8601 QSPI driver -- same command framing.
// Sequence from Waveshare's 1.75 BSP (esp32_s3_touch_amoled_1_75.c), with
// 0x51 (brightness) started at 0 instead of FF, like the 1.43: the panel
// stays dark until the first frame is drawn, then ramps up.
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
  { 0xFE, (uint8_t[]){ 0x20 }, 1, 0 },
  { 0x19, (uint8_t[]){ 0x10 }, 1, 0 },
  { 0x1C, (uint8_t[]){ 0xA0 }, 1, 0 },
  { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
  { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },
  { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },
  { 0x51, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
  { 0x2A, (uint8_t[]){ 0x00, 0x06, 0x01, 0xD7 }, 4, 0 },
  { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xD1 }, 4, 600 },
  { 0x11, (uint8_t[]){ 0x00 }, 0, 600 },
  { 0x29, (uint8_t[]){ 0x00 }, 0, 0 },
};
#define LCD_COLUMN_GAP 0x06  // 466 px inside a 480-wide controller
#elif defined(SF_143_CO5300)
// 2026-11-14: Waveshare lists the 1.43"'s driver as "SH8601 / CO5300" -- a replacement table console may come with the
// CO5300 (the 1.75"'s chip). Build with -DSF_143_CO5300 (tools/table_check.py --co5300): the 1.75"'s CO5300 sequence +
// the 1.43"'s own MADCTL (0x36 = C0, the table's picture turned 180). UNTESTED until such a board turns up; if the picture
// is shifted sideways, try -DSF_143_GAP=8.
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
  { 0xFE, (uint8_t[]){ 0x20 }, 1, 0 },
  { 0x19, (uint8_t[]){ 0x10 }, 1, 0 },
  { 0x1C, (uint8_t[]){ 0xA0 }, 1, 0 },
  { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
  { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },
  { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },
  { 0x51, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0x36, (uint8_t[]){ 0xC0 }, 1, 0 },
  { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
  { 0x2A, (uint8_t[]){ 0x00, 0x06, 0x01, 0xD7 }, 4, 0 },
  { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xD1 }, 4, 600 },
  { 0x11, (uint8_t[]){ 0x00 }, 0, 600 },
  { 0x29, (uint8_t[]){ 0x00 }, 0, 0 },
};
#ifdef SF_143_GAP
#define LCD_COLUMN_GAP SF_143_GAP
#else
#define LCD_COLUMN_GAP 0x06
#endif
#else
#define LCD_COLUMN_GAP 0x08
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
  { 0xFE, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0xC4, (uint8_t[]){ 0x80 }, 1, 0 },
  { 0x3A, (uint8_t[]){ 0x55 }, 1, 0 },
  { 0x35, (uint8_t[]){ 0x00 }, 1, 0 },
  { 0x53, (uint8_t[]){ 0x20 }, 1, 0 },
  { 0x51, (uint8_t[]){ 0x00 }, 1, 0 },  // Start dark; ramp up via bsp_display_brightness_set() after first frame renders
  { 0x36, (uint8_t[]){ 0xC0 }, 1, 0 },
  { 0x63, (uint8_t[]){ 0xFF }, 1, 0 },
  { 0x2A, (uint8_t[]){ 0x00, 0x06, 0x01, 0xD7 }, 4, 0 },
  { 0x2B, (uint8_t[]){ 0x00, 0x00, 0x01, 0xD1 }, 4, 0 },
  { 0x11, (uint8_t[]){ 0x00 }, 0, 100 },
  { 0x29, (uint8_t[]){ 0x00 }, 0, 0 },
};
#endif

void bsp_lcd_init(void) {
  int ret = ESP_OK;

#ifdef LCD_EN_PIN
  // 1.43" only: power on the AMOLED panel. Without this the panel receives
  // no power at all, regardless of whether the SPI/QSPI signals are correct.
  gpio_config_t en_conf = {};
  en_conf.intr_type = GPIO_INTR_DISABLE;
  en_conf.mode = GPIO_MODE_OUTPUT;
  en_conf.pin_bit_mask = (0x1ULL << LCD_EN_PIN);
  en_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  en_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  gpio_config(&en_conf);
  gpio_set_level((gpio_num_t)LCD_EN_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(10));
  Serial.println("[BSP] AMOLED_EN set HIGH");
#endif

  spi_bus_config_t buscfg = {};
  buscfg.sclk_io_num = LCD_SCK_PIN;
  buscfg.data0_io_num = LCD_D0_PIN;
  buscfg.data1_io_num = LCD_D1_PIN;
  buscfg.data2_io_num = LCD_D2_PIN;
  buscfg.data3_io_num = LCD_D3_PIN;
  buscfg.max_transfer_sz = (LCD_WIDTH * LCD_HEIGHT * 2);
  ret = spi_bus_initialize(BSP_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
  Serial.printf("[BSP] spi_bus_initialize: %s\n", esp_err_to_name(ret));
  ESP_ERROR_CHECK(ret);

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = LCD_CS_PIN;
  io_config.dc_gpio_num = -1;
  io_config.spi_mode = 0;
  io_config.pclk_hz = BSP_LCD_PCLK_MHZ * 1000 * 1000;
  io_config.trans_queue_depth = 4;
  io_config.on_color_trans_done = NULL;
  io_config.user_ctx = NULL;
  io_config.lcd_cmd_bits = 32;
  io_config.lcd_param_bits = 8;
  io_config.flags.quad_mode = true;
  ret = esp_lcd_new_panel_io_spi(BSP_SPI_HOST, &io_config, &io_handle);
  Serial.printf("[BSP] esp_lcd_new_panel_io_spi: %s (io_handle=%p)\n", esp_err_to_name(ret), (void*)io_handle);
  ESP_ERROR_CHECK(ret);

  sh8601_vendor_config_t vendor_config = {};
  vendor_config.init_cmds = lcd_init_cmds;
  vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);
  vendor_config.flags.use_qspi_interface = 1;

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = LCD_RST_PIN;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = (16);
  panel_config.vendor_config = &vendor_config;
  ESP_LOGI(TAG, "Install SH8601 panel driver");
  ret = esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle);
  Serial.printf("[BSP] esp_lcd_new_panel_sh8601: %s (panel_handle=%p)\n", esp_err_to_name(ret), (void*)panel_handle);
  ESP_ERROR_CHECK(ret);

  ret = esp_lcd_panel_reset(panel_handle);
  Serial.printf("[BSP] esp_lcd_panel_reset: %s\n", esp_err_to_name(ret));
  ESP_ERROR_CHECK(ret);

  ret = esp_lcd_panel_init(panel_handle);
  Serial.printf("[BSP] esp_lcd_panel_init: %s\n", esp_err_to_name(ret));
  ESP_ERROR_CHECK(ret);

  ret = esp_lcd_panel_set_gap(panel_handle, LCD_COLUMN_GAP, 0);
  Serial.printf("[BSP] esp_lcd_panel_set_gap: %s\n", esp_err_to_name(ret));
  ESP_ERROR_CHECK(ret);
}

bool bsp_screen_off = false;
esp_err_t bsp_display_brightness_set(int brightness_percent) {
  if (bsp_screen_off) brightness_percent = 0;  // a cracked table screen stays dark
  if (panel_handle == NULL) {
    ESP_LOGE(TAG, "Panel handle is not initialized");
    return ESP_ERR_INVALID_STATE;
  }

  if (brightness_percent < 0 || brightness_percent > 100) {
    ESP_LOGE(TAG, "Invalid brightness percentage. Should be between 0 and 100.");
    return ESP_ERR_INVALID_ARG;
  }

  brightness = (uint8_t)(brightness_percent * 255 / 100);

  uint32_t lcd_cmd = 0x51;
  lcd_cmd &= 0xff;
  lcd_cmd <<= 8;
  lcd_cmd |= 0x02 << 24;
  uint8_t param = brightness;
  esp_err_t ret = esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &param, 1);
  Serial.printf("[BSP] brightness_set(%d): %s\n", brightness_percent, esp_err_to_name(ret));

  return ESP_OK;
}

int bsp_display_brightness_get(void) {
  if (panel_handle == NULL) {
    ESP_LOGE(TAG, "Panel handle is not initialized");
    return -1;
  }

  return brightness * 100 / 255;
}

esp_err_t bsp_display_brightness_init(void) {
  bsp_display_brightness_set(100);
  return ESP_OK;
}

esp_err_t bsp_display_lock(int32_t timeout_ms) {
  const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return ((xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE) ? ESP_OK : ESP_FAIL);
}

void bsp_display_unlock(void) {
  xSemaphoreGive(lvgl_mux);
}

void bsp_touch_init(i2c_master_bus_handle_t BusHandle) {
  i2c_device_config_t dev_cfg = {};
  dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dev_cfg.scl_speed_hz = 400000;
  dev_cfg.device_address = TOUCH_I2C_ADDR;
  esp_err_t ret = i2c_master_bus_add_device(BusHandle, &dev_cfg, &touch_dev_handle);
  Serial.printf("[BSP] touch i2c_master_bus_add_device: %s\n", esp_err_to_name(ret));
  ESP_ERROR_CHECK(ret);

  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = (0x1ULL << TP_RST_PIN);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;

  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));

  gpio_set_level((gpio_num_t)TP_RST_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
  gpio_set_level((gpio_num_t)TP_RST_PIN, 0);
  vTaskDelay(pdMS_TO_TICKS(200));
  gpio_set_level((gpio_num_t)TP_RST_PIN, 1);
  vTaskDelay(pdMS_TO_TICKS(200));
}

#if SF_BOARD == SF_BOARD_AMOLED_175
// 1.75": battery telemetry lives in the AXP2101 PMIC (I2C 0x34), not an ADC
// pin. Nothing in the game reads it yet -- stubs until it's needed.
void bsp_batt_init(void) {}
uint16_t bsp_batt_get_voltage(void) { return 0; }
uint8_t bsp_batt_get_status(void) { return 0; }
#else
void bsp_batt_init(void) {
  adc_cali_curve_fitting_config_t cali_config = {};
  cali_config.unit_id = ADC_UNIT_1;
  cali_config.atten = ADC_ATTEN_DB_12;
  cali_config.bitwidth = ADC_BITWIDTH_12;
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));

  adc_oneshot_unit_init_cfg_t init_config1 = {};
  init_config1.unit_id = ADC_UNIT_1;
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
  adc_oneshot_chan_cfg_t config = {};
  config.bitwidth = ADC_BITWIDTH_12;
  config.atten = ADC_ATTEN_DB_12;
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_3, &config));

  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_INPUT;
  gpio_conf.pin_bit_mask = (0x1ULL << GPIO_NUM_7);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;

  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
}

uint16_t bsp_batt_get_voltage(void) {
  int value;
  int tage = 0;
  float vol = 0;
  esp_err_t err;
  err = adc_oneshot_read(adc1_handle, ADC_CHANNEL_3, &value);
  if (err == ESP_OK) {
    adc_cali_raw_to_voltage(cali_handle, value, &tage);
    vol = tage * 2;
  }
  return vol;
}

/*1表示正在充电，0表示未充电*/
uint8_t bsp_batt_get_status(void) {
  int level = gpio_get_level(GPIO_NUM_7);
  return (level == 0) ? 1 : 0;  // 0表示正在充电，1表示未充电
}
#endif

#if SF_BOARD == SF_BOARD_AMOLED_175
// CST9217 protocol (from SensorLib's TouchDrvCST92xx, as shipped in
// Waveshare's 1.75 repo): write the 16-bit read command 0xD000, read 15
// bytes, then write 0xD000 + ACK 0xAB. The frame is valid when byte 6 is
// 0xAB; byte 5 & 0x7F = point count; first point = bytes 0..4 with
// byte0 low nibble 0x06 = pressed, x = b1<<4 | b3>>4, y = b2<<4 | b3&0x0F.
uint8_t GetCoords(uint16_t *x, uint16_t *y) {
  static uint32_t poll_count = 0;
  uint8_t cmd[3] = { 0xD0, 0x00, 0xAB };
  uint8_t buf[15] = { 0 };

  int ret1 = i2c_bus->i2c_master_write_read_dev(touch_dev_handle, cmd, 2, buf, sizeof(buf));
  int ret2 = i2c_bus->i2c_write_buff(touch_dev_handle, -1, cmd, 3);  // ack the frame

  if (poll_count < 20) {
    Serial.printf("[TOUCH] poll #%lu rd=%d ack=%d b5=0x%02X b6=0x%02X\n",
                  (unsigned long)poll_count, ret1, ret2, buf[5], buf[6]);
    poll_count++;
  }
  if (ret1 != 0 || buf[6] != 0xAB) return 0;
  uint8_t points = buf[5] & 0x7F;
  if (points == 0 || points > 2) return 0;
  if ((buf[0] & 0x0F) != 0x06) return 0;  // first finger not pressed

  *x = ((uint16_t)buf[1] << 4) | (buf[3] >> 4);
  *y = ((uint16_t)buf[2] << 4) | (buf[3] & 0x0F);
  return 1;
}
#else
uint8_t GetCoords(uint16_t *x, uint16_t *y) {
  // FocalTech FT3168-style register protocol:
  //   reg 0x02       = touch point count (low nibble)
  //   reg 0x03..0x06 = point 1: XH(bits3:0)+event(bits7:6), XL, YH(bits3:0), YL
  static uint32_t poll_count = 0;
  uint8_t touch_count = 0;
  uint8_t pos[4] = { 0 };

  int ret1 = i2c_bus->i2c_read_buff(touch_dev_handle, 0x02, &touch_count, 1);

  if (poll_count < 20) {
    Serial.printf("[TOUCH] poll #%lu i2c_ret=%d touch_count=0x%02X\n",
                  (unsigned long)poll_count, ret1, touch_count);
    poll_count++;
  }

  if ((touch_count & 0x0F) == 0) {
    return 0;
  }

  int ret2 = i2c_bus->i2c_read_buff(touch_dev_handle, 0x03, pos, 4);
  *x = (((uint16_t)pos[0] & 0x0F) << 8) | pos[1];
  *y = (((uint16_t)pos[2] & 0x0F) << 8) | pos[3];
  Serial.printf("[TOUCH] CONTACT i2c_ret=%d raw=[%02X %02X %02X %02X] -> x=%u y=%u\n",
                ret2, pos[0], pos[1], pos[2], pos[3], *x, *y);
  return 1;
}
#endif

// v0.23.3 diagnostics (user 2026-10-15: the command console "still gets slow
// by the second round" although its link taps take 7-33 ms). Every 5 s:
// how long LVGL's work took, plus anything that could pile up over a game.
uint32_t g_uiDrawUs[UI_T_COUNT] = {};

static uint32_t countObjs(lv_obj_t *o) {
  uint32_t n = 1;
  const uint32_t k = lv_obj_get_child_count(o);
  for (uint32_t i = 0; i < k; i++) n += countObjs(lv_obj_get_child(o, i));
  return n;
}

static void lvglHealthReport(uint32_t passes, uint32_t busyUs, uint32_t maxUs) {
  uint32_t timers = 0;
  for (lv_timer_t *t = lv_timer_get_next(nullptr); t; t = lv_timer_get_next(t)) timers++;
  Serial.printf("[UI] 5s: %u passes, busy %u ms (max pass %u ms); %u objects, %u timers; "
                "heap %u KB free (min %u, block %u), PSRAM %u KB free\n",
                (unsigned)passes, (unsigned)(busyUs / 1000), (unsigned)(maxUs / 1000),
                (unsigned)countObjs(lv_screen_active()), (unsigned)timers,
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  Serial.printf("[UI] 5s LVGL stack spare %u bytes\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  if (perf_refr)
    Serial.printf("[PERF] 5s: %u flushes, %u ms inside flush_cb\n", (unsigned)perf_flush_n, (unsigned)(perf_flush_us / 1000));
  perf_flush_n = perf_flush_us = 0;
  if (perf_refr)
    Serial.printf("[PERF] 5s: %u refreshes, %u ms drawing + %u ms waiting on the panel (%d MHz), %u Kpx sent; slowest %u ms (%u Kpx)\n",
                  (unsigned)perf_refr, (unsigned)((perf_refr_us - perf_wait_us) / 1000), (unsigned)(perf_wait_us / 1000), BSP_LCD_PCLK_MHZ,
                  (unsigned)(perf_px / 1000), (unsigned)(perf_max_us / 1000), (unsigned)(perf_max_px / 1000));
  perf_refr = perf_refr_us = perf_wait_us = perf_max_us = perf_max_px = 0;
  perf_px = 0;
  Serial.printf("[UI] 5s draw ms: rim %u, stars %u, planet %u, static %u\n",
                (unsigned)(g_uiDrawUs[UI_T_RIM] / 1000), (unsigned)(g_uiDrawUs[UI_T_STARS] / 1000),
                (unsigned)(g_uiDrawUs[UI_T_PLANET] / 1000), (unsigned)(g_uiDrawUs[UI_T_STATIC] / 1000));
  for (auto &u : g_uiDrawUs) u = 0;
}

void lvgl_port_task(void *arg) {
  Serial.println("[BSP] lvgl_port_task started");
  uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  uint32_t loop_count = 0;
  uint32_t statStart = millis(), statPasses = 0, statBusyUs = 0, statMaxUs = 0;
  healthWatchThisTask();  // 2.0-dev.6: a screen task stuck HEALTH_WDT_S seconds restarts the console
  while (1) {
    healthFeed();
    if (bsp_display_lock(-1) == ESP_OK) {
      const uint32_t t0 = micros();
      task_delay_ms = lv_timer_handler();
      const uint32_t us = micros() - t0;
      statPasses++;
      statBusyUs += us;
      if (us > statMaxUs) statMaxUs = us;
      if (millis() - statStart >= 5000) {
        lvglHealthReport(statPasses, statBusyUs, statMaxUs);
        statStart = millis();
        statPasses = statBusyUs = statMaxUs = 0;
      }
      // v0.26.0: a custom renderer draws its frame right after LVGL's pass, under
      // the same lock (LVGL's dropped flushes leave the buffers free; any LVGL
      // transfer still in flight finishes first).
      bsp_frame_hook_t hook = frame_hook;
      if (hook) {
        // 3.1-dev.23: bounded (the table froze twice -- task watchdog, LVGL task blocked with IDLE0
        // running -- most likely right here or on a strip semaphore when one transfer-done never came).
        const uint32_t w0 = millis();
        while (lvgl_inflight > 0) {
          vTaskDelay(1);
          if (millis() - w0 > 200) {
            Serial.printf("[BSP] LVGL transfer never finished (%d in flight) -- resync\n", (int)lvgl_inflight);
            lvgl_inflight = 0;
          }
        }
        task_delay_ms = hook();
      }
      bsp_display_unlock();
    } else {
      Serial.println("[BSP] lvgl_port_task: failed to acquire lock");
    }
    if (loop_count < 5) {
      Serial.printf("[BSP] lvgl_port_task loop %lu, next delay=%lu ms\n", (unsigned long)loop_count, (unsigned long)task_delay_ms);
      loop_count++;
    }
    if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS) {
      task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
    } else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS) {
      task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
    }
    vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
  }
}

static void rounder_event_cb(lv_event_t *e) {
  lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
  uint16_t x1 = area->x1;
  uint16_t x2 = area->x2;

  uint16_t y1 = area->y1;
  uint16_t y2 = area->y2;

  // round the start of coordinate down to the nearest 2M number
  area->x1 = (x1 >> 1) << 1;
  area->y1 = (y1 >> 1) << 1;
  // round the end of coordinate up to the nearest 2N+1 number
  area->x2 = ((x2 >> 1) << 1) + 1;
  area->y2 = ((y2 >> 1) << 1) + 1;
}

void increase_lvgl_tick(void *arg) {
  lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// Touch calibration: derived from REAL measured data, not another guess.
// Two rounds of a flat TOUCH_Y_OFFSET each only partially worked -- because
// this was never a constant-offset problem. Real Serial captures of raw
// touch readings at known reference points proved it:
//   tap the TOP edge    -> raw tp_y ~= 461  (should map to screen y = 0)
//   tap the BOTTOM edge -> raw tp_y ~= 18   (should map to screen y = 466)
// A single offset can't satisfy both (one needs a ~5px correction, the
// other needs a ~38px correction in the opposite direction) -- the touch
// sensor's native Y range doesn't map 1:1 onto the display's 0..466 range,
// it needs to be SCALED, not just shifted. Solving the linear relationship
// from those two points gives screen_Y = 485 - 1.05 * raw_Y, implemented
// below with integer math (21/20 = 1.05) to avoid floating point.
// X-axis data (right edge landed within 2px, center within 8px of correct
// using the existing plain LCD_WIDTH - tp_x mapping) didn't show the same
// problem, so X is left unscaled. If X still feels off after this, the
// next step would be gathering a clean LEFT-edge tap to check its scale
// the same way.
#define TOUCH_DEBUG_LOG 1  // set to 0 once calibration is confirmed good, to stop the Serial spam

// 2.0 test tool: a press (or a drag from one point to another) fed in from
// Serial ("T x y" / "D x0 y0 x1 y1 ms", see loop()), so screens can be walked
// and timed with nobody at the glass. Real touches still work meanwhile.
static volatile int inj_x0, inj_y0, inj_x1, inj_y1;
static volatile uint32_t inj_t0 = 0, inj_ms = 0;
void bsp_inject_touch(int x0, int y0, int x1, int y1, uint32_t ms) {
  inj_x0 = x0; inj_y0 = y0; inj_x1 = x1; inj_y1 = y1;
  inj_ms = ms ? ms : 1;
  inj_t0 = millis();
}

static bsp_touch_hook_t touch_hook = nullptr;
void bsp_set_touch_hook(bsp_touch_hook_t hook) { touch_hook = hook; }

static void indevReadRaw(lv_indev_t *indev, lv_indev_data_t *indevData);
void my_lvgl_indev_cb(lv_indev_t *indev, lv_indev_data_t *indevData) {
  indevReadRaw(indev, indevData);
  // 2.0: the sketch sees every sample (edge swipe = back, see the .ino)
  if (touch_hook) touch_hook(indevData->point.x, indevData->point.y, indevData->state == LV_INDEV_STATE_PRESSED);
}

static void indevReadRaw(lv_indev_t *indev, lv_indev_data_t *indevData) {
  uint16_t tp_x = 0x00;
  uint16_t tp_y = 0x00;
  if (inj_ms) {
    const uint32_t e = millis() - inj_t0;
    if (e <= inj_ms) {
      indevData->point.x = inj_x0 + (int32_t)(inj_x1 - inj_x0) * (int32_t)e / (int32_t)inj_ms;
      indevData->point.y = inj_y0 + (int32_t)(inj_y1 - inj_y0) * (int32_t)e / (int32_t)inj_ms;
      indevData->state = LV_INDEV_STATE_PRESSED;
      return;
    }
    inj_ms = 0;  // let go
    indevData->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  const uint32_t tc0 = millis();
  const uint8_t touched = GetCoords(&tp_x, &tp_y);
  if (millis() - tc0 > 50) Serial.printf("[TOUCH] slow read: %u ms\n", (unsigned)(millis() - tc0));
  if (touched) {
#if SF_BOARD == SF_BOARD_AMOLED_175
    // Waveshare's BSP mirrors both axes for this panel. User-verified on
    // hardware 2026-09-28 ("everything looks good, no need for screen touch
    // calibration") -- if taps near the rim ever feel off, capture raw vs
    // mapped from the Serial log below at known points, as done for the 1.43.
    indevData->point.x = (LCD_WIDTH - 1) - (int32_t)tp_x;
    indevData->point.y = (LCD_HEIGHT - 1) - (int32_t)tp_y;
#else
    indevData->point.x = LCD_WIDTH - tp_x;
    indevData->point.y = 485 - (21 * (int32_t)tp_y) / 20;
#endif
    if (indevData->point.x > LCD_WIDTH)
      indevData->point.x = LCD_WIDTH;
    if (indevData->point.x < 0)
      indevData->point.x = 0;
    if (indevData->point.y > LCD_HEIGHT)
      indevData->point.y = LCD_HEIGHT;
    if (indevData->point.y < 0)
      indevData->point.y = 0;
    indevData->state = LV_INDEV_STATE_PRESSED;
#if TOUCH_DEBUG_LOG
    Serial.printf("[TOUCH] raw=(%d,%d)  screen=(%d,%d)\n", tp_x, tp_y, indevData->point.x, indevData->point.y);
#endif
  } else {
    indevData->state = LV_INDEV_STATE_RELEASED;
  }
}

// v0.26.0: the custom renderer's strips (see board_bsp.h). Transfers finish in
// the order they were queued, so a FIFO of buffer ids tells which one is free.

static bool notifyFlushReady(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
  if (strip_head != strip_tail) {  // a renderer strip finished
    const int k = strip_fifo[strip_head];
    strip_head = (strip_head + 1) & 3;
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(strip_free[k], &woke);
    return woke == pdTRUE;
  }
  if (lvgl_inflight > 0) __atomic_fetch_sub(&lvgl_inflight, 1, __ATOMIC_SEQ_CST);  // atomic: the flush side may be on the other core
  lv_display_flush_ready((lv_display_t *)user_ctx);
  BaseType_t woke = pdFALSE;
  if (lvgl_flush_sem) xSemaphoreGiveFromISR(lvgl_flush_sem, &woke);
  return woke == pdTRUE;
}

static void lvglFlushWaitCb(lv_display_t *disp) {
  (void)disp;
  const uint32_t t0 = micros();
  while (lvgl_inflight > 0) {
    xSemaphoreTake(lvgl_flush_sem, 2);  // 2-tick timeout: never stuck on a missed give
    if (micros() - t0 > 200000) {  // a transfer never reported back: don't freeze the screen over it
      Serial.printf("[BSP] flush wait gave up (%d in flight)\n", (int)lvgl_inflight);
      lvgl_inflight = 0;
      lv_display_flush_ready(disp);
    }
  }
  perf_wait_us += micros() - t0;
}
static void lvglPerfEventCb(lv_event_t *e) {
  if (lv_event_get_code(e) == LV_EVENT_REFR_START) { perf_refr_t0 = micros(); perf_refr_px0 = perf_px; return; }
  const uint32_t us = micros() - perf_refr_t0, px = perf_px - perf_refr_px0;
  if (px == 0) return;  // nothing was dirty
  perf_refr++;
  perf_refr_us += us;
  if (us > perf_max_us) { perf_max_us = us; perf_max_px = px; }
  if (us > 60000) Serial.printf("[PERF] slow refresh %u ms (%u Kpx)\n", (unsigned)(us / 1000), (unsigned)(px / 1000));
}

void bsp_set_frame_hook(bsp_frame_hook_t hook) { frame_hook = hook; }
uint16_t *bsp_strip_buffer(int k) { return (uint16_t *)lvgl_buf[k & 1]; }
// 3.1-dev.23: a strip waits at most 150 ms for its transfer-done; a lost one resyncs the FIFO and the
// frame goes on (one strip may tear once) instead of blocking the LVGL task until the watchdog restarts.
static bool stripTake(int k) {
  if (xSemaphoreTake(strip_free[k], pdMS_TO_TICKS(150)) == pdTRUE) return true;
  Serial.printf("[BSP] strip %d transfer-done lost (fifo %d..%d, lvgl in flight %d) -- resync\n", k, strip_head, strip_tail, (int)lvgl_inflight);
  strip_head = strip_tail;
  return false;
}
void bsp_frame_wait(int k) { k &= 1; stripTake(k); xSemaphoreGive(strip_free[k]); }
void bsp_frame_strip(int k, int y0, int rows) {
  k &= 1;
  stripTake(k);  // given back by the transfer-done interrupt
  strip_fifo[strip_tail] = k;
  strip_tail = (strip_tail + 1) & 3;
  if (esp_lcd_panel_draw_bitmap(panel_handle, 0, y0, LCD_WIDTH, y0 + rows, lvgl_buf[k]) != ESP_OK) {
    strip_tail = (strip_tail + 3) & 3;  // nothing queued: undo
    xSemaphoreGive(strip_free[k]);
  }
}

void my_lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
  static uint32_t flush_count = 0;
  if (frame_hook) { lv_display_flush_ready(disp); return; }  // v0.26.0: the custom renderer owns the screen
  const uint32_t ft0 = micros();
  esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
  lv_draw_sw_rgb565_swap(color_p, lv_area_get_width(area) * lv_area_get_height(area));

  const int offsetx1 = area->x1;
  const int offsetx2 = area->x2;
  const int offsety1 = area->y1;
  const int offsety2 = area->y2;

  // Counted BEFORE queuing: a small area can finish (and its interrupt run)
  // before draw_bitmap returns -- counting after left the count stuck at 1
  // and LVGL waiting forever (2.0 partial redraws hit it on a screen change).
  __atomic_fetch_add(&lvgl_inflight, 1, __ATOMIC_SEQ_CST);
  esp_err_t ret = esp_lcd_panel_draw_bitmap(panel, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_p);
  if (ret != ESP_OK) __atomic_fetch_sub(&lvgl_inflight, 1, __ATOMIC_SEQ_CST);
  perf_flush_n++;
  perf_flush_us += micros() - ft0;
  perf_px += (uint32_t)(offsetx2 - offsetx1 + 1) * (uint32_t)(offsety2 - offsety1 + 1);
  if (flush_count < 10) {
    Serial.printf("[BSP] flush #%lu area=(%d,%d)-(%d,%d) panel=%p ret=%s\n",
                  (unsigned long)flush_count, offsetx1, offsety1, offsetx2, offsety2,
                  (void*)panel, esp_err_to_name(ret));
    flush_count++;
  }
  // v0.24.2: lv_display_flush_ready() now comes from notifyFlushReady() when
  // the transfer is done (calling it here, before the DMA finished, let LVGL
  // race ahead of the display -- suspected in a table crash in the SPI ISR).
  if (ret != ESP_OK) lv_display_flush_ready(disp);  // nothing was queued, so no "done" will come
}

bsp_lvgl_t bsp_broolesia_display_init(void) {
  lvgl_mux = xSemaphoreCreateMutex();
  assert(lvgl_mux);
  i2c_bus = I2cMasterBus::requestInstance(ESP32_I2C_SCL, ESP32_I2C_SDA, BSP_I2C_HOST);
  assert(i2c_bus);
  bsp_batt_init();
  bsp_lcd_init();
  /*lvgl*/
  lv_init();
  lv_display_t *disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_flush_cb(disp, my_lvgl_flush_cb);
  uint8_t *buffer1 = NULL;
  uint8_t *buffer2 = NULL;
  Serial.printf("[BSP] free DMA-capable heap before buffer alloc: %d bytes (need %d for both buffers)\n",
                heap_caps_get_free_size(MALLOC_CAP_DMA), BUFF_SIZE * 2);
  buffer1 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA);
  buffer2 = (uint8_t *)heap_caps_malloc(BUFF_SIZE, MALLOC_CAP_DMA);
  if (buffer1) memset(buffer1, 0, BUFF_SIZE);
  if (buffer2) memset(buffer2, 0, BUFF_SIZE);
  Serial.printf("[BSP] buffer1=%p buffer2=%p (size=%d each)\n", (void*)buffer1, (void*)buffer2, BUFF_SIZE);
  assert(buffer1);
  assert(buffer2);
  lv_display_set_buffers(disp, buffer1, buffer2, BUFF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lvgl_buf[0] = buffer1;  // v0.26.0: shared with the custom renderer (see bsp_set_frame_hook)
  lvgl_buf[1] = buffer2;
  for (int k = 0; k < 2; k++) strip_free[k] = xSemaphoreCreateBinary(), xSemaphoreGive(strip_free[k]);
  lv_display_set_user_data(disp, panel_handle);
  lvgl_flush_sem = xSemaphoreCreateBinary();
  lv_display_set_flush_wait_cb(disp, lvglFlushWaitCb);
  lv_display_add_event_cb(disp, lvglPerfEventCb, LV_EVENT_REFR_START, NULL);
  lv_display_add_event_cb(disp, lvglPerfEventCb, LV_EVENT_REFR_READY, NULL);
  // v0.24.2: LVGL hears "flush done" when the display transfer has actually
  // finished (the standard Espressif pattern), not the moment it was queued.
  esp_lcd_panel_io_callbacks_t io_cbs = {};
  io_cbs.on_color_trans_done = notifyFlushReady;
  esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, disp);
  Serial.printf("[BSP] display created, user_data(panel_handle)=%p\n", (void*)panel_handle);

  bsp_touch_init(i2c_bus->Get_I2cBusHandle());
  lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, my_lvgl_indev_cb);
  // Poll touch every 15ms instead of whatever lv_conf.h's default happens to
  // be (often 30ms+). This directly reduces perceived tap latency.
  lv_timer_t *indev_timer = lv_indev_get_read_timer(indev);
  if (indev_timer) {
    lv_timer_set_period(indev_timer, 15);
  }

  result.disp = disp;
  result.indev = indev;

  bsp_display_brightness_init();

  esp_timer_create_args_t lvgl_tick_timer_args = {};
  lvgl_tick_timer_args.callback = &increase_lvgl_tick;
  lvgl_tick_timer_args.name = "lvgl_tick";
  esp_timer_handle_t lvgl_tick_timer = NULL;
  esp_err_t tret = esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer);
  Serial.printf("[BSP] esp_timer_create (tick): %s\n", esp_err_to_name(tret));
  ESP_ERROR_CHECK(tret);
  tret = esp_timer_start_periodic(lvgl_tick_timer, LVGL_TICK_PERIOD_MS * 1000);
  Serial.printf("[BSP] esp_timer_start_periodic (tick): %s\n", esp_err_to_name(tret));
  ESP_ERROR_CHECK(tret);

#if SF_BOARD == 143
  // 3.1-dev.63 (user: the phones "felt so sluggish"; measured in a 4-bot game: /state 319 ms median, up to 1.3 s, vs
  // 60 ms in the Lobby): the table's screen task (priority 2, ~77% busy drawing the glow) floated onto core 1 and
  // starved loopTask (priority 1), which runs the web server -- every request waited for the gaps between frames.
  // 3.1-dev.68: dev.63 pinned it to core 0 -- the table then FROZE twice in a 4-bot game (task watchdog, IDLE0 starved: the
  // glow + Wi-Fi left core 0 no idle time). Now it stays on core 1 at loopTask's own priority (1): the two share core 1
  // round-robin, so a request waits at most a tick, not a whole frame; core 0 keeps Wi-Fi and its idle task.
  BaseType_t task_ok = xTaskCreatePinnedToCore(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, 1, NULL, 1);
#else
  BaseType_t task_ok = xTaskCreate(lvgl_port_task, "LVGL", LVGL_TASK_STACK_SIZE, NULL, LVGL_TASK_PRIORITY, NULL);
#endif
  Serial.printf("[BSP] xTaskCreate(LVGL): %s\n", (task_ok == pdPASS) ? "pdPASS" : "FAILED");
  return result;
}

bsp_lvgl_t bsp_get_broolesia_display(void) {
  if (result.disp == NULL || result.indev == NULL) {
    ESP_LOGE(TAG, "Display not initialized yet");
    return (bsp_lvgl_t){ 0 };
  }
  return result;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void) {
  if (i2c_bus == nullptr) {
    ESP_LOGE(TAG, "I2C bus not initialized yet");
    return nullptr;
  }
  return i2c_bus->Get_I2cBusHandle();
}

esp_err_t bsp_spiffs_mount(void) {
  esp_vfs_spiffs_conf_t conf = {
    .base_path = "/spiffs",
    .partition_label = "storage",
    .max_files = 2,
    .format_if_mount_failed = true,
  };
  esp_err_t ret_val = esp_vfs_spiffs_register(&conf);
  ESP_ERROR_CHECK(ret_val);
  size_t total = 0, used = 0;
  ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
  if (ret_val != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
  } else {
    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
  }
  return ret_val;
}
