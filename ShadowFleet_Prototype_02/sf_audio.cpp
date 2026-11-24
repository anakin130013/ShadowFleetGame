// Sound effects (v0.25.3) -- see sf_audio.h. The codec setup follows
// Waveshare's 08_ES8311 example (16 kHz, 16-bit, MCLK = 256 x fs); the
// synthesis is our own: each effect is a short list of tone steps (start and
// end frequency, length, waveform, loudness) rendered on an audio task.
#include <Arduino.h>
#include "sf_audio.h"
#include "board_select.h"
#include "bsp_config.h"

#if SF_BOARD == SF_BOARD_AMOLED_175
#include <ESP_I2S.h>
#include <driver/i2c_master.h>
#include "board_bsp.h"
#include "sf_es8311.h"

static const uint32_t SFX_RATE = 16000;
static const int SFX_VOLUME = 70;         // codec volume 0-100 (retune after a listen)
static const float SFX_LEVEL = 0.28f;     // peak sample level before the codec

static i2c_master_dev_handle_t codec_dev = nullptr;
static I2SClass sfx_i2s;
static QueueHandle_t sfx_queue = nullptr;
static char sfx_status[96] = "not started";

// The codec driver's register access (sf_es8311.c), over the shared bus.
extern "C" esp_err_t sf_audio_i2c_write(uint8_t addr, const uint8_t *data, size_t len) {
  (void)addr;
  return codec_dev ? i2c_master_transmit(codec_dev, data, len, 100) : ESP_ERR_INVALID_STATE;
}
extern "C" esp_err_t sf_audio_i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *value) {
  (void)addr;
  return codec_dev ? i2c_master_transmit_receive(codec_dev, &reg, 1, value, 1, 100) : ESP_ERR_INVALID_STATE;
}

enum Wave : uint8_t { W_SQUARE, W_TRIANGLE, W_NOISE, W_SINE };
struct Tone { uint16_t f0, f1, ms; uint8_t wave, vol; };  // vol 0-100; f0 = 0 -> a rest
#define TONE_END { 0, 0, 0, 0, 0 }

static const Tone SFX_CHIME_T[] = { { 1319, 1319, 110, W_TRIANGLE, 70 }, { 1976, 1976, 240, W_TRIANGLE, 60 }, TONE_END };
static const Tone SFX_HORN_T[] = { { 165, 110, 520, W_SQUARE, 55 }, { 110, 98, 260, W_SQUARE, 40 }, TONE_END };
static const Tone SFX_FANFARE_T[] = { { 523, 523, 110, W_SQUARE, 55 }, { 659, 659, 110, W_SQUARE, 55 },
                                      { 784, 784, 110, W_SQUARE, 55 }, { 1047, 1047, 420, W_SQUARE, 60 }, TONE_END };
static const Tone SFX_ALARM_T[] = { { 880, 880, 150, W_TRIANGLE, 75 }, { 660, 660, 150, W_TRIANGLE, 75 },
                                    { 880, 880, 150, W_TRIANGLE, 75 }, { 660, 660, 260, W_TRIANGLE, 70 }, TONE_END };
static const Tone SFX_CHIRP_T[] = { { 1, 1, 130, W_NOISE, 35 }, { 1200, 2400, 90, W_SINE, 60 }, TONE_END };
static const Tone *const SFX_TABLE[SFX_COUNT] = { SFX_CHIME_T, SFX_HORN_T, SFX_FANFARE_T, SFX_ALARM_T, SFX_CHIRP_T };

static void renderSfx(const Tone *t) {
  int16_t buf[256 * 2];  // stereo frames
  uint32_t noise = 0x1234567u;
  float phase = 0;
  for (; t->ms; t++) {
    const int n = SFX_RATE * t->ms / 1000;
    const float amp = SFX_LEVEL * t->vol / 100.0f;
    for (int i = 0; i < n; i += 256) {
      const int m = min(256, n - i);
      for (int k = 0; k < m; k++) {
        const int j = i + k;
        const float f = t->f0 + (float)(t->f1 - t->f0) * j / n;
        phase += f / SFX_RATE;
        phase -= (int)phase;
        float s;
        switch (t->wave) {
          case W_SQUARE: s = phase < 0.5f ? 1.0f : -1.0f; break;
          case W_TRIANGLE: s = 4.0f * fabsf(phase - 0.5f) - 1.0f; break;
          case W_NOISE: noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5; s = (int32_t)noise / 2147483648.0f; break;
          default: s = sinf(phase * 6.2831853f); break;
        }
        // 4 ms attack, then a gentle fall to 30% across the step: no clicks.
        const float env = min(1.0f, j / (SFX_RATE * 0.004f)) * (1.0f - 0.7f * j / n);
        const int16_t v = (int16_t)(s * amp * env * 32767);
        buf[2 * k] = buf[2 * k + 1] = v;
      }
      sfx_i2s.write((const uint8_t *)buf, m * 4);
    }
  }
  memset(buf, 0, sizeof(buf));  // a little silence so the codec doesn't cut the tail
  for (int r = 0; r < 4; r++) sfx_i2s.write((const uint8_t *)buf, sizeof(buf));
}

static void sfxTask(void *) {
  for (;;) {
    uint8_t id;
    if (xQueueReceive(sfx_queue, &id, portMAX_DELAY) != pdTRUE || id >= SFX_COUNT) continue;
    digitalWrite(AUDIO_PA_PIN, HIGH);  // amp on only while playing: no idle hiss
    renderSfx(SFX_TABLE[id]);
    if (uxQueueMessagesWaiting(sfx_queue) == 0) digitalWrite(AUDIO_PA_PIN, LOW);
  }
}

void sfAudioBegin() {
  pinMode(AUDIO_PA_PIN, OUTPUT);
  digitalWrite(AUDIO_PA_PIN, LOW);
  i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
  if (!bus) { snprintf(sfx_status, sizeof(sfx_status), "OFF: no I2C bus"); Serial.printf("[AUDIO] %s\n", sfx_status); return; }
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = AUDIO_CODEC_ADDR;
  cfg.scl_speed_hz = 100000;
  esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &codec_dev);
  if (err != ESP_OK) { snprintf(sfx_status, sizeof(sfx_status), "OFF: add codec device: %s", esp_err_to_name(err)); Serial.printf("[AUDIO] %s\n", sfx_status); return; }
  // Chip ID (ES8311 registers 0xFD/0xFE = 0x83/0x11): proves the codec answers on I2C.
  uint8_t id1 = 0, id2 = 0;
  const esp_err_t e1 = sf_audio_i2c_read_reg(AUDIO_CODEC_ADDR, 0xFD, &id1), e2 = sf_audio_i2c_read_reg(AUDIO_CODEC_ADDR, 0xFE, &id2);
  if (e1 != ESP_OK || e2 != ESP_OK) { snprintf(sfx_status, sizeof(sfx_status), "OFF: codec not answering at 0x%02X (%s)", AUDIO_CODEC_ADDR, esp_err_to_name(e1 != ESP_OK ? e1 : e2)); Serial.printf("[AUDIO] %s\n", sfx_status); return; }

  sfx_i2s.setPins(AUDIO_I2S_BCLK_PIN, AUDIO_I2S_WS_PIN, AUDIO_I2S_DOUT_PIN, AUDIO_I2S_DIN_PIN, AUDIO_I2S_MCLK_PIN);
  if (!sfx_i2s.begin(I2S_MODE_STD, SFX_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    snprintf(sfx_status, sizeof(sfx_status), "OFF: I2S start failed (chip id %02X %02X)", id1, id2);
    Serial.printf("[AUDIO] %s\n", sfx_status);
    return;
  }
  es8311_handle_t es = es8311_create(0, AUDIO_CODEC_ADDR);
  const es8311_clock_config_t clk = { false, false, true, (int)(SFX_RATE * 256), (int)SFX_RATE };
  if (!es || es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK ||
      es8311_sample_frequency_config(es, clk.mclk_frequency, clk.sample_frequency) != ESP_OK) {
    snprintf(sfx_status, sizeof(sfx_status), "OFF: ES8311 init failed (chip id %02X %02X)", id1, id2);
    Serial.printf("[AUDIO] %s\n", sfx_status);
    return;
  }
  es8311_microphone_config(es, false);
  es8311_voice_volume_set(es, SFX_VOLUME, nullptr);
  sfx_queue = xQueueCreate(6, 1);
  xTaskCreate(sfxTask, "sfx", 4096, nullptr, 1, nullptr);
  snprintf(sfx_status, sizeof(sfx_status), "ready (ES8311 chip id %02X %02X, volume %d)", id1, id2, SFX_VOLUME);
  Serial.printf("[AUDIO] %s\n", sfx_status);
  sfxPlay(SFX_CHIME);  // v0.25.6: a speaker test at boot
}

const char *sfAudioStatus() { return sfx_status; }

void sfxPlay(SfxId id) {
  if (!sfx_queue) return;
  const uint8_t v = id;
  xQueueSend(sfx_queue, &v, 0);  // never blocks the UI; drops if 6 are already waiting
}

#else  // the 1.43" has no audio hardware (Waveshare wiki): sound is a no-op
extern "C" esp_err_t sf_audio_i2c_write(uint8_t, const uint8_t *, size_t) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t sf_audio_i2c_read_reg(uint8_t, uint8_t, uint8_t *) { return ESP_ERR_NOT_SUPPORTED; }
void sfAudioBegin() {}
void sfxPlay(SfxId) {}
const char *sfAudioStatus() { return "none (no audio hardware)"; }
#endif
