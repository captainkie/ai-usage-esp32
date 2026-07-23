// Pixie voice client: record a question from the mic, POST it to the Mac bridge's
// /voice endpoint (whisper.cpp -> Claude -> say), and play the spoken answer.
// Depends on audio.h (audio_record/audio_play) and net.h (g_host/g_port/g_token).
#pragma once
#include <HTTPClient.h>
#include "audio.h"

// Shared voice UI state (render task reads it; the net task writes it).
enum { VOICE_IDLE = 0, VOICE_LISTENING, VOICE_THINKING, VOICE_SPEAKING, VOICE_ERROR };
static volatile int g_voice_state = VOICE_IDLE;
static char g_voice_transcript[128] = "";
static char g_voice_reply[256] = "";

#define VOICE_SECONDS   4
#define VOICE_SAMPLES   (AUDIO_RATE * VOICE_SECONDS)     // 16000 * 4 = 64000
#define VOICE_PCM_BYTES (VOICE_SAMPLES * 2)              // 16-bit mono

// Little-endian helpers for the 44-byte WAV header.
static void wav_u32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void wav_u16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

// Fill a 44-byte canonical WAV header for 16 kHz mono 16-bit PCM of `dataBytes`.
static void voice_wav_header(uint8_t *h, uint32_t dataBytes) {
  memcpy(h, "RIFF", 4);            wav_u32(h+4, 36 + dataBytes);
  memcpy(h+8, "WAVE", 4);
  memcpy(h+12, "fmt ", 4);         wav_u32(h+16, 16);
  wav_u16(h+20, 1);                wav_u16(h+22, AUDIO_CHANNELS);
  wav_u32(h+24, AUDIO_RATE);       wav_u32(h+28, AUDIO_RATE * AUDIO_CHANNELS * 2);
  wav_u16(h+32, AUDIO_CHANNELS*2); wav_u16(h+34, AUDIO_BITS);
  memcpy(h+36, "data", 4);         wav_u32(h+40, dataBytes);
}

// URL-decode in place (for the X-Transcript / X-Reply headers).
static void voice_url_decode(char *s) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '%' && p[1] && p[2]) {
      auto hex = [](char c){ return c<='9'?c-'0':(c|0x20)-'a'+10; };
      *o++ = (char)((hex(p[1])<<4) | hex(p[2])); p += 2;
    } else if (*p == '+') *o++ = ' ';
    else *o++ = *p;
  }
  *o = 0;
}

// One full voice turn: record -> POST /voice -> play the reply. Blocking; call
// from the net task (loop()), never inside an LVGL callback.
static bool voice_ask() {
  if (g_token.length() == 0 || g_host.length() == 0) { g_voice_state = VOICE_ERROR; return false; }
  if (WiFi.status() != WL_CONNECTED) { g_voice_state = VOICE_ERROR; return false; }

  // 1) record — amp OFF so its hum doesn't bleed into the mic
  g_voice_state = VOICE_LISTENING;
  uint8_t *wav = (uint8_t *)heap_caps_malloc(44 + VOICE_PCM_BYTES, MALLOC_CAP_SPIRAM);
  if (!wav) { g_voice_state = VOICE_ERROR; return false; }
  voice_wav_header(wav, VOICE_PCM_BYTES);
  audio_amp(false);
  size_t got = audio_record((int16_t *)(wav + 44), VOICE_SAMPLES);
  audio_amp(true);
  if (got == 0) { free(wav); g_voice_state = VOICE_ERROR; return false; }

  // 2) POST /voice
  g_voice_state = VOICE_THINKING;
  HTTPClient http;
  String url = "http://" + g_host + ":" + g_port + "/voice";
  http.begin(url);
  http.addHeader("Content-Type", "audio/wav");
  http.addHeader("X-Pixie-Token", g_token);
  const char *collect[] = { "X-Transcript", "X-Reply" };
  http.collectHeaders(collect, 2);
  http.setTimeout(30000);
  int code = http.POST(wav, 44 + VOICE_PCM_BYTES);
  free(wav);
  if (code != 200) {
    Serial.printf("[voice] POST -> HTTP %d\n", code);
    http.end(); g_voice_state = VOICE_ERROR; return false;
  }

  // transcript + reply (for the screen)
  strlcpy(g_voice_transcript, http.header("X-Transcript").c_str(), sizeof(g_voice_transcript));
  strlcpy(g_voice_reply,      http.header("X-Reply").c_str(),      sizeof(g_voice_reply));
  voice_url_decode(g_voice_transcript);
  voice_url_decode(g_voice_reply);
  Serial.printf("[voice] you: %s\n[voice] pixie: %s\n", g_voice_transcript, g_voice_reply);

  // 3) read the reply WAV, skip its 44-byte header, play the PCM.
  // Bound the buffer: a reply longer than ~30s of 16k mono audio (or an unknown
  // Content-Length) is capped so a malformed/oversized response can't overflow.
  const size_t VOICE_REPLY_CAP = 16000 * 2 * 30;   // ~30s @ 16kHz mono 16-bit
  int len = http.getSize();
  size_t cap = (len > 0 && (size_t)len < VOICE_REPLY_CAP) ? (size_t)len : VOICE_REPLY_CAP;
  WiFiClient *stream = http.getStreamPtr();
  uint8_t *reply = (uint8_t *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
  if (!reply) { http.end(); g_voice_state = VOICE_ERROR; return false; }
  size_t rn = 0;
  unsigned long t0 = millis();
  while (http.connected() && rn < cap && (len < 0 || rn < (size_t)len) && millis() - t0 < 20000) {
    int avail = stream->available();
    if (avail > 0) {
      size_t room = cap - rn;
      if ((size_t)avail > room) avail = room;      // never write past the buffer
      rn += stream->readBytes(reply + rn, avail);
      t0 = millis();
      if (rn >= cap) break;
    } else delay(5);
  }
  http.end();

  g_voice_state = VOICE_SPEAKING;
  if (rn > 44) audio_play((int16_t *)(reply + 44), (rn - 44) / 2);
  free(reply);
  g_voice_state = VOICE_IDLE;
  return true;
}
