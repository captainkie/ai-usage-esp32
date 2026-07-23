# Pixie — Voice AI Assistant (Phase 1) — Design

_Date: 2026-07-23 · Status: approved (design), pending on-device audio verification_

## Context

**Pixie** (the project formerly known as "AI Usage Bar for ESP32" — that stays the
headline feature + search keyword) is a tiny Waveshare **ESP32-S3-Touch-LCD-3.49**
desk companion. It already shows Claude Code usage (screen ①), a Mac system
monitor (②), and a Mac remote (③), fed by a zero-dependency Node **bridge** on the
Mac over Wi-Fi or USB, with a pixel companion that reacts to your usage.

This spec covers **Phase 1 of the voice AI assistant**: say **"Jarvis"** (or tap),
ask a question, and Pixie answers out loud — with all speech processing **free and
local on your Mac**.

## Goal (Phase 1)

A single-turn, wake-word voice Q&A:

> **"Jarvis" (or tap) → speak a question → Pixie speaks Claude's answer.**

The real objective is to stand up the **audio pipeline + bridge voice endpoint** that
every later phase reuses. Phase 1 uses the **stock, offline "Jarvis" WakeNet word**
(with push-to-talk as the guaranteed fallback); the brand-matching **"Pixie" custom
wake word** is a roadmap follow-on (see §2).

## Non-goals (later phases — do not build now)

- Multi-provider (GPT / Gemini / Kimi) + the AI-settings **screen ④** → Phase 2
- Voice **control** of the device/Mac ("lock my Mac", "switch to Codex") → Phase 3
- **Proactive** spoken announcements (usage/reset alerts) → Phase 4
- The **"Pixie" custom wake word**, real-time streaming, multi-turn memory, and a
  user-configurable wake word → roadmap / later

## Architecture — bridge = brain, ESP = ears + mouth

```
say "Jarvis" (or tap) → speak                      Mac bridge  (POST /voice, token-gated)
   │ ESP-SR wakes → capture mic (16kHz mono, ES7210)   │  1. whisper.cpp   → transcript (local, free)
   ▼ ───────────── WAV over Wi-Fi ────────────────────▶│  2. Claude        → reply text (existing path)
Pixie speaks the answer (ES8311 speaker) ◀── WAV ───────│  3. macOS `say`   → speech, female (local, free)
   + screen shows transcript + reply, mascot animates   └──────────────────────────────────────────
```

The ESP stays "dumb" (mic in / speaker out / screen); the Mac bridge does all the
heavy lifting (STT + LLM + TTS). This keeps the token on the Mac and makes later
multi-provider trivial (just more bridge endpoints).

## Components

### 1. ESP audio I/O
- Bring up **ES7210** (mic ADC) + **ES8311** (speaker DAC) via the BSP's
  `codec_board` + `esp_codec_dev` over I²S, with the **TCA9554** IO expander.
- Capture and playback at **16 kHz, 16-bit, mono**.
- Reference: the BSP's proven `08_Audio_Test` example.

### 2. Trigger — "Jarvis" wake word + push-to-talk fallback
- **Primary: "Jarvis" wake word** — ESP-SR **WakeNet** ships a stock, offline
  "Hi Jarvis" model, so hands-free activation needs **no custom training**. Requires
  the **`esp_sr_16`** partition scheme (a dedicated ~3.9 MB model partition, a change
  from the current `app3M_fat9M_16MB`).
- **Fallback (guaranteed to ship): push-to-talk** — tap the screen (or the BOOT
  button) to start capture. If ESP-SR-in-Arduino proves too costly (memory pressure
  alongside LVGL + Wi-Fi + the codec), Phase 1 ships on push-to-talk and "Jarvis"
  lands as a fast-follow.
- **Roadmap: "Pixie" wake word** — the brand-matching word is **not** a stock WakeNet
  model, so it needs a **custom model** (microWakeWord/openWakeWord, or Espressif's
  custom WakeNet service). Scoped as a roadmap follow-on.
- Capture ends on **silence** (VAD via the ESP-SR audio front-end) or a **max
  duration** (~8 s).

### 3. ESP voice client
- After trigger + capture: assemble a WAV and `POST http://<bridge>/voice` with the
  pairing token, over Wi-Fi.
- Response = TTS audio (WAV) + transcript/reply text. Play the audio; show the text.

### 4. Bridge `POST /voice`
- **Auth:** pairing token (same mechanism as `/action`).
- **Pipeline:** save incoming WAV → **whisper.cpp** (multilingual model → transcript)
  → **Claude** (reuse the existing Anthropic call path with the fresh Keychain token)
  → reply text → **`say`** (female voice) → WAV → return audio + `{transcript, reply}`.
- Everything is free + local except the Claude call, which already happens for the
  dashboard (same trust boundary).

### 5. On-screen Voice UI
- A voice overlay / placeholder **screen ④** with states: 🎤 **Listening** → 💭
  **Thinking** → 🔊 **Speaking** → idle.
- Shows the **transcript** (what you said) and **Pixie's reply**.
- The **pixel mascot** reacts (listen / think / speak poses) — reuse the mascot engine.

## Data flow

1. "Jarvis" (or tap) → ESP captures the utterance (VAD / max ~8 s).
2. ESP `POST`s the WAV → bridge `/voice` (with token).
3. Bridge: whisper.cpp → transcript; Claude → reply; `say` → WAV.
4. Bridge returns the WAV + `{transcript, reply}`.
5. ESP plays the WAV and shows the text; the mascot animates.

## Interface — `POST /voice`

- **Request:** `Content-Type: audio/wav`, `X-Pixie-Token: <token>`; body = WAV
  (16 kHz, mono).
- **Response:** `200`, `Content-Type: audio/wav`; headers `X-Transcript` and
  `X-Reply` (URL-encoded UTF-8); body = TTS WAV.
- **Errors:** `401` (bad token), `422` (no speech detected — skip Claude), `500`
  (pipeline error).
- (A multipart or JSON+base64 shape is an alternative; the header approach keeps ESP
  parsing simplest. Final choice belongs in the implementation plan.)

## Voice + multilingual (Thai + English, female)

- whisper.cpp **multilingual** model (e.g. `ggml-base` or `ggml-small`) → handles
  Thai and English. whisper reports the detected language.
- `say` uses a **female** voice, picked by detected language — **`Kanya`** (Thai,
  female) for Thai replies, **`Samantha`** (US English, female) otherwise. Claude
  replies in the user's language automatically.

## Error handling

- No/empty transcript → `422` → ESP shows "ไม่ได้ยิน" + a short beep; **no Claude call**.
- Claude / network error → `500` → ESP shows an error, optionally speaks "ขอโทษ ลองใหม่".
- Wi-Fi down → ESP can't reach `/voice` → "ต้องต่อ Wi-Fi สำหรับใช้เสียง".

## Privacy / security

- `/voice` is gated by the **pairing token** (same as `/action`).
- **Audio and transcript never leave the Mac** — whisper.cpp and `say` are local.
  Only the transcript **text** goes to Claude (identical to the dashboard's trust
  boundary). No cloud STT/TTS.

## Constraints

- **Wi-Fi required** for voice (audio streams over Wi-Fi). The dashboard still works
  over USB; USB-audio is a later option.
- **Latency ~10–15 s** per query (record → whisper → Claude → say) — a
  request/response MVP, not streaming.
- **Single-turn** (no conversation memory) in Phase 1.

## Risks + mitigations

- **ESP-SR "Jarvis" wake word in Arduino + the `esp_sr_16` partition change is the
  highest risk** — memory pressure alongside LVGL + Wi-Fi + the codec, and Espressif's
  examples are ESP-IDF-centric. → The **push-to-talk fallback guarantees Phase 1
  ships**; "Jarvis" is layered on once stable. The brand **"Pixie"** wake word (custom
  model) is a separate roadmap task.
- **Audio codec bring-up** (ES7210/ES8311 via `codec_board`) → lean on the proven
  `08_Audio_Test` reference.
- **Memory** — full framebuffer + mascot + Wi-Fi + audio buffers (+ the SR model) is
  a lot of PSRAM; measure headroom during bring-up.

## Testing

- **Bridge `/voice` — fully testable without the board:** feed a sample WAV (record
  one, or generate one with `say`) → assert the transcript, the reply, and that a
  playable WAV comes back. Scriptable + repeatable.
- **ESP audio — needs on-device human verification:** say "Jarvis" (or tap), speak
  into the mic, hear the speaker. Build + compile now; **flash + verify with the
  user**. Do **not** flash over the working dashboard until it's ready.

## Verification split (this is a hardware feature)

- ✅ **Autonomous now:** spec, implementation plan, bridge `/voice`
  (whisper.cpp + Claude + `say`), the mockup Voice screen + screenshots, README, and
  a compiling firmware scaffold.
- 🙌 **Needs the user (hands-on):** flash the firmware; speak to verify mic capture,
  the "Jarvis" wake word (+ push-to-talk), speaker playback, latency, and tuning.
