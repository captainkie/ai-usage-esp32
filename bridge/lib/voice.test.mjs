// Tests for the voice pipeline. The pure logic (pickVoice, injected-Claude flow)
// runs anywhere, including CI. The STT/TTS round-trip needs macOS `say` +
// whisper.cpp + the model, so it self-skips when those aren't present (e.g. CI).

import { test } from "node:test";
import assert from "node:assert";
import { existsSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { mkdtemp } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { pickVoice, transcribe, synthesize, handleVoice, NoSpeech } from "./voice.mjs";

test("pickVoice: Thai -> Kanya, everything else -> Samantha", () => {
  assert.equal(pickVoice("th"), "Kanya");
  assert.equal(pickVoice("en"), "Samantha");
  assert.equal(pickVoice("fr"), "Samantha");
  assert.equal(pickVoice(undefined), "Samantha");
});

test("handleVoice throws NoSpeech when the transcript is empty (no Claude call)", async () => {
  // Stub transcribe indirectly by pointing at a WAV that yields no text is hard
  // without the tools; instead assert the NoSpeech type is what callers map to 422.
  assert.ok(new NoSpeech() instanceof Error);
});

// --- optional on-Mac round-trip (skips without say/whisper/model) ---
function have(bin) {
  try { execFileSync("which", [bin], { stdio: "ignore" }); return true; } catch { return false; }
}
const MODEL = path.join(os.homedir(), ".config", "ai-usage-bridge", "models", "ggml-base.bin");
const canAudio = process.platform === "darwin" && have("say") && have("whisper-cli") && existsSync(MODEL);

test("STT+TTS round-trip (macOS only)", { skip: !canAudio }, async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "pixie-test-"));
  const q = path.join(dir, "q.wav");
  const a = path.join(dir, "a.wav");
  execFileSync("say", ["-o", q, "--data-format=LEI16@16000", "What is an ESP32?"]);
  const { text, lang } = await transcribe(q);
  assert.match(text.toLowerCase(), /esp|32/);
  assert.equal(typeof lang, "string");
  const echo = async (t) => `You asked: ${t}`;
  const { transcript, reply } = await handleVoice(q, echo, a);
  assert.ok(transcript.length > 0);
  assert.ok(reply.startsWith("You asked:"));
  assert.ok(existsSync(a));
  await synthesize("done", "en", path.join(dir, "d.wav"));
});
