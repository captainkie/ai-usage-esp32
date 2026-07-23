// Pixie audio I/O — ES7210 mic (record) + ES8311 speaker (playback) via the
// Waveshare codec_board / esp_codec_dev stack. 16 kHz mono 16-bit (whisper's rate).
//
// Requires i2c_master_Init() + tca9554_init() to have run first (setup()).
// Adapted from the BSP Examples/Arduino/08_Audio_Test.
#pragma once
#include <stdint.h>
#include "i2c_bsp.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/esp_codec_dev/include/esp_codec_dev.h"

#define AUDIO_RATE     16000
#define AUDIO_BITS     16
#define AUDIO_CHANNELS 1

static esp_codec_dev_handle_t g_playback = NULL;
static esp_codec_dev_handle_t g_record   = NULL;
static esp_io_expander_handle_t g_io_expander = NULL;

// Set up the speaker-amp enable line (TCA9554 pin 7). Without the amp on the
// ES8311 plays but nothing reaches the speaker.
static bool audio_amp_enable() {
  i2c_master_bus_handle_t bus = NULL;
  if (i2c_master_get_bus_handle(0, &bus) != ESP_OK) return false;
  if (esp_io_expander_new_i2c_tca9554(bus, ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000, &g_io_expander) != ESP_OK) return false;
  esp_io_expander_set_dir(g_io_expander, IO_EXPANDER_PIN_NUM_7, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(g_io_expander, IO_EXPANDER_PIN_NUM_7, 1);
  return true;
}

// Toggle the speaker amp. Turn it OFF while recording — an always-on amp hums a
// constant tone into the ES7210 mic (whisper hears "[music]", not speech) — and
// back ON just before playback.
static void audio_amp(bool on) {
  if (g_io_expander) esp_io_expander_set_level(g_io_expander, IO_EXPANDER_PIN_NUM_7, on ? 1 : 0);
}

// Bring up the codec once. Safe to call after the display/i2c are up.
static bool audio_init() {
  audio_amp_enable();
  set_codec_board_type("S3_LCD_3_49");
  codec_init_cfg_t cfg = {
    .in_mode   = CODEC_I2S_MODE_TDM,
    .out_mode  = CODEC_I2S_MODE_TDM,
    .in_use_tdm = false,
    .reuse_dev = false,
  };
  if (init_codec(&cfg) != ESP_OK) return false;
  g_playback = get_playback_handle();
  g_record   = get_record_handle();
  if (!g_playback || !g_record) return false;

  esp_codec_dev_set_out_vol(g_playback, 95.0);   // speaker volume 0..100
  esp_codec_dev_set_in_gain(g_record, 35.0);     // mic gain — 35 captured cleanly; 40 amplified noise
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate     = AUDIO_RATE;
  fs.channel         = AUDIO_CHANNELS;
  fs.bits_per_sample = AUDIO_BITS;
  esp_codec_dev_open(g_playback, &fs);
  esp_codec_dev_open(g_record, &fs);
  return true;
}

static void audio_set_volume(int vol) {          // 0..100
  if (g_playback) esp_codec_dev_set_out_vol(g_playback, (float)vol);
}

// Record up to `maxSamples` int16 samples from the mic. Blocks. Returns the number
// of samples captured (== maxSamples here; a later step adds silence detection).
static size_t audio_record(int16_t *buf, size_t maxSamples) {
  if (!g_record) return 0;
  int err = esp_codec_dev_read(g_record, buf, maxSamples * sizeof(int16_t));
  return err == ESP_OK ? maxSamples : 0;
}

// Play `n` int16 samples out the speaker. Blocks until done.
static void audio_play(const int16_t *buf, size_t n) {
  if (!g_playback || n == 0) return;
  esp_codec_dev_write(g_playback, (void *)buf, n * sizeof(int16_t));
}
