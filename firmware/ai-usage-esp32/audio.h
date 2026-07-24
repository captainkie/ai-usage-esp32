// Pixie audio I/O — ES7210 mic (record) + ES8311 speaker (playback) via the
// Waveshare codec_board / esp_codec_dev stack. 16 kHz mono 16-bit (whisper's rate).
//
// Requires i2c_master_Init() + tca9554_init() to have run first (setup()).
// Adapted from the BSP Examples/Arduino/08_Audio_Test.
#pragma once
#include <stdint.h>
#include "i2c_bsp.h"
#include "src/tca9554/esp_io_expander_tca9554.h"
#include "power.h"                                 // shared TCA9554 handle (g_io_expander, board_expander)
#include "src/codec_board/codec_board.h"
#include "src/codec_board/codec_init.h"
#include "src/esp_codec_dev/include/esp_codec_dev.h"

#define AUDIO_RATE     16000
#define AUDIO_BITS     16
#define AUDIO_CHANNELS 1

static esp_codec_dev_handle_t g_playback = NULL;
static esp_codec_dev_handle_t g_record   = NULL;
// g_io_expander (the shared TCA9554 handle) lives in power.h — power_begin() already
// created it and latched SYS_EN (EXIO6) by the time audio_init() runs.
static int g_out_vol = 100;                       // speaker volume 0..100 (screen ④ +/-); amp is
                                                  // OFF while recording so max vol won't hum the mic

// Set up the speaker-amp enable line (TCA9554 pin 7). Without the amp on the
// ES8311 plays but nothing reaches the speaker. Reuses the shared expander so we
// don't add the same I2C device twice.
static bool audio_amp_enable() {
  esp_io_expander_handle_t io = board_expander();
  if (!io) return false;
  esp_io_expander_set_dir(io, PWR_AMP_EXIO, IO_EXPANDER_OUTPUT);
  esp_io_expander_set_level(io, PWR_AMP_EXIO, 1);
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

  esp_codec_dev_set_out_vol(g_playback, (float)g_out_vol);   // 80 captured cleanly; 95 hummed into the mic
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
  if (vol < 0) vol = 0; if (vol > 100) vol = 100;
  g_out_vol = vol;
  if (g_playback) esp_codec_dev_set_out_vol(g_playback, (float)vol);
}
static int audio_get_volume() { return g_out_vol; }

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
