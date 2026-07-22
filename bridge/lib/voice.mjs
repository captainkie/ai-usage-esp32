// Pixie voice pipeline: a spoken question (WAV) -> a spoken answer (WAV).
//
// Free + local on the Mac: whisper.cpp for STT and macOS `say` for TTS. The LLM
// ("askClaude") is injected by the caller so this module stays testable and the
// token handling lives in one place (the bridge).
//
// One-time setup (see bridge/README.md):
//   brew install whisper-cpp
//   curl -L -o ~/.config/ai-usage-bridge/models/ggml-base.bin \
//     https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin

import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { readFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

const execFileP = promisify(execFile);

const MODEL = process.env.WHISPER_MODEL ||
  path.join(os.homedir(), ".config", "ai-usage-bridge", "models", "ggml-base.bin");
const WHISPER = process.env.WHISPER_BIN || "whisper-cli";

/** Thrown when whisper finds no words (silence / mis-trigger) — caller maps to 422. */
export class NoSpeech extends Error {}

/**
 * STT — transcribe a 16 kHz mono WAV with whisper.cpp (multilingual, local).
 * Returns { text, lang } where lang is whisper's 2-letter auto-detected language.
 */
export async function transcribe(wavPath) {
  const outBase = wavPath.replace(/\.wav$/i, "") + ".stt";
  const { stderr } = await execFileP(
    WHISPER,
    ["-m", MODEL, "-f", wavPath, "-l", "auto", "-otxt", "-of", outBase],
    { maxBuffer: 16 << 20 },
  );
  let text = "";
  try { text = (await readFile(outBase + ".txt", "utf8")).trim(); } catch { /* empty */ }
  const m = /auto-detected language:\s*([a-z]{2})/i.exec(stderr || "");
  return { text, lang: m ? m[1].toLowerCase() : "en" };
}

/** A female macOS voice per language: Kanya (Thai), Samantha (default). */
export function pickVoice(lang) {
  return lang === "th" ? "Kanya" : "Samantha";
}

/** TTS — write `text` to a 16 kHz mono WAV via macOS `say` (no ffmpeg). */
export async function synthesize(text, lang, outWav) {
  await execFileP("say", ["-v", pickVoice(lang), "-o", outWav, "--data-format=LEI16@16000", text]);
  return outWav;
}

/**
 * Full pipeline: WAV question -> { transcript, reply, lang, wavPath } with the
 * reply spoken into `outWav`. `askClaude(text) => Promise<string>` is injected.
 */
export async function handleVoice(wavPath, askClaude, outWav) {
  const { text, lang } = await transcribe(wavPath);
  if (!text) throw new NoSpeech("no speech detected");
  const reply = await askClaude(text);
  await synthesize(reply, lang, outWav);
  return { transcript: text, reply, lang, wavPath: outWav };
}
