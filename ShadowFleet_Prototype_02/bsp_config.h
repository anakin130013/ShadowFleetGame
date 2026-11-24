#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#include "board_select.h"

#define BSP_SPI_HOST        SPI2_HOST
#define BSP_I2C_HOST        I2C_NUM_0

#define LCD_WIDTH           466
#define LCD_HEIGHT          466

#if SF_BOARD == SF_BOARD_AMOLED_175
// ---- ESP32-S3-Touch-AMOLED-1.75(-B) ----
// Source: Waveshare repo HARDWARE_REFERENCE.md + Arduino pin_config.h
// (github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75), checked 2026-09-28.
// One shared I2C bus carries touch (CST9217 @0x5A) plus the AXP2101 PMIC,
// TCA9554 expander, RTC, IMU and audio codecs -- none of which the game
// needs yet. No panel power-enable pin: the panel is powered by the board.
#define ESP32_I2C_SDA       GPIO_NUM_15
#define ESP32_I2C_SCL       GPIO_NUM_14

#define LCD_D0_PIN          GPIO_NUM_4    // QSPI SDIO0
#define LCD_D1_PIN          GPIO_NUM_5    // QSPI SDIO1
#define LCD_D2_PIN          GPIO_NUM_6    // QSPI SDIO2
#define LCD_D3_PIN          GPIO_NUM_7    // QSPI SDIO3
#define LCD_CS_PIN          GPIO_NUM_12
#define LCD_SCK_PIN         GPIO_NUM_38
#define LCD_RST_PIN         GPIO_NUM_39   // CO5300 reset (separate from touch reset)

#define TP_RST_PIN          GPIO_NUM_40   // CST9217 reset
#define TP_INT_PIN          GPIO_NUM_11
#define TOUCH_I2C_ADDR      0x5A

// Audio (v0.25.3): ES8311 codec on the shared I2C bus + its speaker amp.
// Source: Waveshare's LVGL-9 BSP header (BSP_I2S_*) and their Arduino example
// 08_ES8311, whose setPins(BCLK, WS, DOUT, DIN, MCLK) = (9, 45, 8, 10, 42) --
// pin_config.h's names disagree with each other, these two sources agree.
#define AUDIO_I2S_BCLK_PIN  9
#define AUDIO_I2S_WS_PIN    45
#define AUDIO_I2S_DOUT_PIN  8     // ESP32 -> codec DAC -> speaker
#define AUDIO_I2S_DIN_PIN   10    // codec ADC (mics) -> ESP32, unused
#define AUDIO_I2S_MCLK_PIN  42
#define AUDIO_PA_PIN        46    // speaker amplifier enable (HIGH = on)
#define AUDIO_CODEC_ADDR    0x18  // ES8311

#else
// ---- ESP32-S3-Touch-AMOLED-1.43 (NON-C variant) ----
// Source: official Waveshare wiki pinout table for this exact board.
// The previous values in this file were for the 1.43C variant (different
// chip package / different wiring) and do not match this hardware.
#define ESP32_I2C_SDA       GPIO_NUM_47
#define ESP32_I2C_SCL       GPIO_NUM_48

#define LCD_D0_PIN          GPIO_NUM_11   // QSPI_D0
#define LCD_D1_PIN          GPIO_NUM_12   // QSPI_D1
#define LCD_D2_PIN          GPIO_NUM_13   // QSPI_D2
#define LCD_D3_PIN          GPIO_NUM_14   // QSPI_D3
#define LCD_CS_PIN          GPIO_NUM_9    // QSPI_CS
#define LCD_SCK_PIN         GPIO_NUM_10   // QSPI_CLK
#define LCD_RST_PIN         GPIO_NUM_21   // AMOLED_RST
#define LCD_EN_PIN          GPIO_NUM_42   // AMOLED_EN -- power enable, must be driven HIGH
#define LCD_TE_PIN          GPIO_NUM_8    // Not used by this driver; GPIO8 is actually IMU_INT on this board

#define TP_RST_PIN          GPIO_NUM_16
#define TP_INT_PIN          GPIO_NUM_17
#define TOUCH_I2C_ADDR      0x38

/*
电路Codec引脚说明
*/
#define CODEC_I2S_NUM               I2S_NUM_0
#define CODEC_I2S_MCLK_PIN          GPIO_NUM_38
#define CODEC_I2S_SCLK_PIN          GPIO_NUM_39
#define CODEC_I2S_LRCK_PIN          GPIO_NUM_40
#define CODEC_I2S_DSDIN_PIN         GPIO_NUM_41
#define CODEC_I2S_DSOUT_PIN         GPIO_NUM_42
#define CODEC_NS415_CTRL_PIN        GPIO_NUM_46
#define CODEC_LDO_EN_PIN            GPIO_NUM_18

/*
BAT ADC
*/
#define BAT_ADC_GPIO_PIN            GPIO_NUM_4
#define BAT_LED_GPIO_PIN            GPIO_NUM_5
#define BAT_ETA6098_STAT_PIN        GPIO_NUM_7
#endif

#define ESP32_I2C_SDA_PIN   ESP32_I2C_SDA
#define ESP32_I2C_SCL_PIN   ESP32_I2C_SCL

/*
BUTTON
*/
#define BUTTON_0_GPIO_PIN           GPIO_NUM_0

/*lvgl*/
#define LVGL_TICK_PERIOD_MS    2
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 5
// Bumped from 8KB: GameState grew to ~3.3KB per copy once reputation and
// obligation tracking were added, and several call paths stack-allocate a
// full GameState copy (GameState gs = gameStateRead();). A specific case
// (showHome() -> updateHomeUI()) was found stacking TWO such copies
// simultaneously and very likely overflowing an 8KB stack, crashing the
// board on navigation into Home. That specific double-copy was eliminated,
// but this larger stack stays as a safety margin against similar risk
// elsewhere / in the future as more state gets added.
#if SF_ROLE == SF_ROLE_TABLE
#define LVGL_TASK_STACK_SIZE   (16 * 1024)  // the table: ~8.5 KB spare
#else
// 2.0-dev.6: the 1.75" had only ~3.8 KB spare on Home (screens built from
// starTick sit on top of its GameState copies, ~4.7 KB each); ~47 KB free.
#define LVGL_TASK_STACK_SIZE   (24 * 1024)
#endif
#define LVGL_TASK_PRIORITY     2

#endif
