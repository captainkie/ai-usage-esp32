# Pixie Voice Assistant — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tap the screen (or say "Jarvis") → speak → Pixie answers out loud, with STT + TTS free and local on the Mac and Claude as the brain.

**Architecture:** The ESP32 captures mic audio and POSTs a WAV to the Mac bridge's new `/voice` endpoint; the bridge runs whisper.cpp (STT) → Claude (existing path) → macOS `say` (TTS, female) and returns a WAV + transcript/reply; the ESP plays the WAV and shows the text. The ESP stays a dumb audio+screen terminal.

**Tech Stack:** Node (zero-dep bridge) · whisper.cpp + `say` (Mac CLIs) · Arduino-ESP32 / LVGL · ES7210+ES8311 via the BSP `codec_board`/`esp_codec_dev` · ESP-SR WakeNet ("Jarvis") for the wake word.

**Verification reality:** Groups A + B are fully verifiable on the Mac tonight. Group C (firmware audio) compiles tonight but its mic/speaker/wake-word behaviour needs a human to speak + listen — flash + verify **with the user**; do NOT flash over the working dashboard until the user is present.

---

## File structure

**Bridge (Mac, autonomous):**
- Create `bridge/lib/voice.mjs` — the `/voice` pipeline: WAV → whisper.cpp → Claude → `say` → WAV + `{transcript, reply}`. One responsibility: turn an audio question into an audio answer.
- Create `bridge/lib/voice.test.mjs` — fixture-based tests (WAV in → transcript/reply/WAV out).
- Modify `bridge/ai-usage-bridge.mjs` — route `POST /voice` (token-gated) to `voice.mjs`; reuse the existing Claude call (`fetchUsage`'s sibling — extract an `askClaude(text)` helper the dashboard already has the token for).
- Modify `bridge/install-macos.sh` + `bridge/README.md` — document the whisper.cpp one-time setup (`brew install whisper-cpp` + model download).

**Mockup / docs (autonomous):**
- Modify `design/mockup.html` — add **screen ④ Pixie Voice** to the existing swipe/tileview sim (listening/thinking/speaking states, transcript + reply, mascot reaction), matching screens ①②③.
- Modify `README.md` — rebrand to **Pixie** (keep "AI Usage Bar" + "ESP32" keywords), add a Voice section + the demo, update Roadmap (Pixie custom wake word, multi-provider, voice control, proactive).
- Create `design/previews/screen-voice.png` — screenshot of screen ④ for the README.

**Firmware (ESP, build-only tonight):**
- Create `firmware/ai-usage-esp32/audio.h` — codec init + record + play (adapted from BSP `08_Audio_Test`).
- Create `firmware/ai-usage-esp32/voice.h` — record→POST `/voice`→play client + the on-screen voice state.
- Modify `firmware/ai-usage-esp32/ai-usage-esp32.ino` — add the Voice tile/overlay + a push-to-talk trigger; wire `voice.h` into the loop.
- Modify `firmware/README.md` — add the audio codec files, the `esp_sr_16` partition (for "Jarvis"), and the on-device voice test checklist.

---

## Group A — Bridge `/voice` (autonomous, TDD)

### Task A0: whisper.cpp + model available on the Mac

- [ ] **Step 1: Install whisper-cpp and a multilingual model**

Run:
```bash
brew install whisper-cpp
mkdir -p ~/.config/ai-usage-bridge/models
# base multilingual (~148MB) — good speed/quality for Thai+English:
curl -L -o ~/.config/ai-usage-bridge/models/ggml-base.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin
```
Expected: `whisper-cli --help` works; the model file exists (~148MB).

- [ ] **Step 2: Sanity-check STT with a generated sample**

Run:
```bash
say -v Samantha -o /tmp/hi.aiff "Hello Pixie, what model am I using?"
ffmpeg -y -i /tmp/hi.aiff -ar 16000 -ac 1 /tmp/hi.wav
whisper-cli -m ~/.config/ai-usage-bridge/models/ggml-base.bin -f /tmp/hi.wav -l auto -otxt -of /tmp/hi
cat /tmp/hi.txt
```
Expected: the transcript reads back roughly "Hello Pixie, what model am I using?".

- [ ] **Step 3: Commit the setup docs** (model path + commands) into `bridge/README.md`.

### Task A1: `askClaude(text)` helper (reuse the existing token path)

**Files:** Modify `bridge/ai-usage-bridge.mjs` (extract from the existing Anthropic call); Test `bridge/lib/voice.test.mjs`.

- [ ] **Step 1:** In `ai-usage-bridge.mjs`, add `export async function askClaude(prompt)` that reads the Claude token the same way `buildPayload` does (`readClaudeToken()`), POSTs to `https://api.anthropic.com/v1/messages` with a small system prompt ("You are Pixie, a concise voice assistant. Answer in the user's language, 1-3 sentences, no markdown."), model `claude-sonnet-5` (fast + cheap for voice), `max_tokens: 300`, and returns the text.
- [ ] **Step 2:** Manual check: `node -e "import('./bridge/ai-usage-bridge.mjs').then(m=>m.askClaude('Say hi in Thai in 3 words').then(console.log))"` → prints a short Thai greeting. (Requires a logged-in Claude Code token.)
- [ ] **Step 3:** Commit.

### Task A2: `voice.mjs` pipeline (WAV → transcript → reply → WAV)

**Files:** Create `bridge/lib/voice.mjs`, `bridge/lib/voice.test.mjs`.

- [ ] **Step 1 (failing test):** `voice.test.mjs` — `transcribe(wavPath)` returns non-empty text for `/tmp/hi.wav`; `synthesize(text, lang)` writes a playable WAV (>1KB) and picks `Kanya` for `lang="th"`, `Samantha` otherwise; `pickVoice(lang)` unit test (no audio needed).
- [ ] **Step 2:** Run `node --test bridge/lib/voice.test.mjs` → FAIL (module missing).
- [ ] **Step 3 (implement):** `voice.mjs`:
  - `transcribe(wavPath)` → `execFile("whisper-cli", ["-m", MODEL, "-f", wavPath, "-l","auto","-otxt","-of", tmp])`, read `tmp.txt`, trim; also parse the detected language from stderr (`auto-detected language: xx`).
  - `pickVoice(lang)` → `lang==="th" ? "Kanya" : "Samantha"`.
  - `synthesize(text, lang)` → `execFile("say", ["-v", pickVoice(lang), "-o", outAiff, text])` then `execFile("ffmpeg", ["-y","-i",outAiff,"-ar","16000","-ac","1", outWav])`; return `outWav`. (If avoiding ffmpeg: `say --data-format=LEI16@16000 -o out.wav` writes a WAV directly — prefer this, drop the ffmpeg dep.)
  - `handleVoice(wavPath)` → transcribe → (empty ⇒ throw `NoSpeech`) → `askClaude` → synthesize → return `{transcript, reply, lang, wavPath}`.
- [ ] **Step 4:** Run `node --test bridge/lib/voice.test.mjs` → PASS.
- [ ] **Step 5:** Commit.

### Task A3: route `POST /voice` (token-gated) in the bridge

**Files:** Modify `bridge/ai-usage-bridge.mjs`.

- [ ] **Step 1:** In the request handler, before `/usage`, add: `if (url === "/voice")` → require `POST`; read the raw body (cap ~2MB); check `X-Pixie-Token` via `tokensMatch(...)` (401 on mismatch); write the body to a temp WAV; `await handleVoice(tmp)`; on `NoSpeech` → 422; on other error → 500; on success → respond `200`, `Content-Type: audio/wav`, headers `X-Transcript` + `X-Reply` (URL-encoded), body = the TTS WAV bytes.
- [ ] **Step 2 (integration test):** script — `curl -s -X POST --data-binary @/tmp/hi.wav -H "X-Pixie-Token: $TOKEN" -H "Content-Type: audio/wav" http://127.0.0.1:8787/voice -D /tmp/h.txt -o /tmp/reply.wav; grep -i x-reply /tmp/h.txt; afplay /tmp/reply.wav`. Expected: `X-Transcript`/`X-Reply` headers present; `reply.wav` plays Pixie's spoken answer.
- [ ] **Step 3:** Confirm `node --test` (all bridge tests) still green.
- [ ] **Step 4:** Commit.

---

## Group B — Mockup screen ④ + Pixie rebrand + README (autonomous)

### Task B1: add screen ④ "Pixie Voice" to `design/mockup.html`

**Files:** Modify `design/mockup.html`.

- [ ] **Step 1:** Study how screens ①②③ are built (tileview/swipe, page dots, segmented nav). Add a 4th tile `Voice` following the exact same pattern + a 4th page dot + nav segment.
- [ ] **Step 2:** Screen ④ content (640×172 space): a mic glyph + a state label cycling 🎤 Listening → 💭 Thinking → 🔊 Speaking (sim buttons to step through), a transcript line ("what model am I on?"), Pixie's reply line ("You're on Opus 4.8…"), and the pixel mascot reacting. Match the existing palette/fonts.
- [ ] **Step 3:** Open the mockup locally (or via the agent-browser skill) and verify all 4 screens swipe + the Voice states cycle. Screenshot screen ④ → `design/previews/screen-voice.png`.
- [ ] **Step 4:** Commit (mockup + screenshot).

### Task B2: rebrand to **Pixie** + Voice section + Roadmap in `README.md`

**Files:** Modify `README.md` (+ `design/mockup.html` `<title>`).

- [ ] **Step 1:** Title → "**Pixie** · a tiny ESP32 desk companion" with a sub-line keeping "**AI Usage Bar** for Claude Code, on the **ESP32-S3**" (keywords + headline feature). Do NOT rename the GitHub repo slug.
- [ ] **Step 2:** Add a "Voice (Pixie)" section: say "Jarvis" (or tap) → ask → spoken answer; free+local (whisper.cpp + `say`), Claude brain, token-gated, Wi-Fi-only for now. Embed `design/previews/screen-voice.png`.
- [ ] **Step 3:** Update Roadmap: voice assistant Phase 1 shipped-in-progress; **"Pixie" custom wake word**, multi-provider + settings screen ④, voice control, proactive announcements.
- [ ] **Step 4:** Commit.

---

## Group C — ESP firmware (build-only tonight; on-device verify WITH the user)

> Reference: the BSP `Examples/Arduino/08_Audio_Test` (ES7210/ES8311 via `codec_board`/`esp_codec_dev` + TCA9554). Copy its `src/` codec files into the sketch. These tasks must **compile** tonight; mic/speaker/wake-word behaviour is verified on-device with the user.

### Task C1: audio codec bring-up (`audio.h`)

**Files:** Create `firmware/ai-usage-esp32/audio.h`; copy BSP codec `src/` into the sketch.

- [ ] **Step 1:** Port `08_Audio_Test`'s codec init into `audio_init()`; add `audio_record(int16_t*buf, size_t maxSamples, size_t*outN)` (16kHz mono, stop on silence via energy threshold or max ~8s) and `audio_play(const int16_t*buf, size_t n)`.
- [ ] **Step 2:** Compile the sketch with the codec files added (arduino-cli). Expected: builds clean. (No behaviour check without hardware.)
- [ ] **Step 3:** Commit.

### Task C2: voice client + on-screen state (`voice.h`)

**Files:** Create `firmware/ai-usage-esp32/voice.h`; Modify `ai-usage-esp32.ino`.

- [ ] **Step 1:** `voice_ask()` — `audio_record` → build a WAV in PSRAM → `HTTPClient POST http://<host>:<port>/voice` with `X-Pixie-Token` + the WAV → read `X-Transcript`/`X-Reply` + the response WAV → `audio_play`. Set a shared `g_voice` state (idle/listening/thinking/speaking/error + transcript + reply) under the mutex.
- [ ] **Step 2:** In `.ino`: add a **Voice tile** (screen ④) or full-screen overlay showing `g_voice` state + transcript + reply + the mascot; a **push-to-talk** trigger (long-press the mascot, or the BOOT button) that sets a flag; drain it in `loop()` (outside the LVGL lock) → `voice_ask()`.
- [ ] **Step 3:** Compile. Expected: builds clean.
- [ ] **Step 4:** Commit.

### Task C3: "Jarvis" WakeNet + `esp_sr_16` partition (attempt; fall back to push-to-talk)

**Files:** Modify `ai-usage-esp32.ino` + `firmware/README.md`.

- [ ] **Step 1:** Switch the build to `PartitionScheme=esp_sr_16`; add ESP-SR (WakeNet "Hi Jarvis") init + the AFE feeding mic frames; on wake → set the same trigger flag as push-to-talk.
- [ ] **Step 2:** Compile. If ESP-SR + LVGL + Wi-Fi won't fit / won't build in Arduino, **stop, keep push-to-talk**, and record the blocker in `firmware/README.md` (wake word → fast-follow).
- [ ] **Step 3:** Commit whichever state builds.

### Task C4: on-device verification checklist (WITH the user)

- [ ] Flash (only when the user is present). Verify: tap → records; speaker plays the reply; transcript/reply show on screen; latency acceptable; then "Jarvis" wakes it. Tune gain/VAD/voice. Record results in `firmware/README.md`.

---

## Self-review

- **Spec coverage:** audio I/O (C1), trigger Jarvis+push-to-talk (C3/C2), voice client (C2), `/voice` pipeline whisper→Claude→say (A2/A3), voice UI (C2 + mockup B1), female Thai/English voice (A2 `pickVoice`), token gating (A3), errors 401/422/500 (A3), privacy local STT/TTS (A0/A2), mockup+README (B1/B2). ✓ All spec sections map to a task.
- **Placeholders:** none — commands, file paths, and endpoint contract are concrete. Firmware code references the BSP `08_Audio_Test` as the source of the codec API rather than inventing it (honest: that API must be read at implementation time).
- **Type consistency:** `handleVoice`/`transcribe`/`synthesize`/`pickVoice`/`askClaude` names + the `/voice` header contract (`X-Pixie-Token`, `X-Transcript`, `X-Reply`) are used consistently across A1–A3 and C2.

## Execution notes (autonomous run)

- Tonight, autonomously: **Group A** (whisper.cpp + `/voice`, fully testable) and **Group B** (mockup + rebrand + README + screenshot), pushed to git as a draft PR.
- **Group C** compiles tonight but is **not flashed**; its checklist (C4) runs hands-on with the user. The working 3-screen/USB firmware on the device is left untouched.
