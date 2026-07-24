// Pixie voice pipeline: a spoken question (WAV) -> a spoken answer (WAV).
//
// Free + local on the Mac: whisper.cpp for STT and macOS `say` for TTS. The LLM
// ("askClaude") is injected by the caller so this module stays testable and the
// token handling lives in one place (the bridge).
//
// One-time setup (see bridge/README.md):
//   brew install whisper-cpp
//   curl -L -o ~/.config/ai-usage-bridge/models/ggml-medium.bin \
//     https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-medium.bin
// `medium` (not `base`) — base transcribes Thai badly ("ภาษาไทย" -> "ภาษาท่าย");
// medium is accurate at ~2s/clip on Apple silicon. Override with WHISPER_MODEL.

import { execFile } from "node:child_process";
import { promisify } from "node:util";
import { readFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

const execFileP = promisify(execFile);

const MODEL = process.env.WHISPER_MODEL ||
  path.join(os.homedir(), ".config", "ai-usage-bridge", "models", "ggml-medium.bin");
const WHISPER = process.env.WHISPER_BIN || "whisper-cli";
// STT language. Default "th" — whisper's `-l auto` confuses Thai with other tonal
// languages (e.g. Vietnamese) on short clips, so we force Thai. Set PIXIE_STT_LANG
// to "auto" for auto-detect, or "en"/etc. for a non-Thai device.
const STT_LANG = process.env.PIXIE_STT_LANG || "th";

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
    ["-m", MODEL, "-f", wavPath, "-l", STT_LANG, "-otxt", "-of", outBase],
    { maxBuffer: 16 << 20 },
  );
  let text = "";
  try { text = (await readFile(outBase + ".txt", "utf8")).trim(); } catch { /* empty */ }
  // When forced to a language, report it (so `say` picks the matching voice); only
  // parse whisper's guess when we actually asked it to auto-detect.
  let lang = STT_LANG;
  if (STT_LANG === "auto") {
    const m = /auto-detected language:\s*([a-z]{2})/i.exec(stderr || "");
    lang = m ? m[1].toLowerCase() : "en";
  }
  return { text, lang };
}

/** A female macOS voice per language: Kanya (Thai), Samantha (default). */
export function pickVoice(lang) {
  return lang === "th" ? "Kanya" : "Samantha";
}

/** TTS — write `text` to a 16 kHz mono WAV via macOS `say` (no ffmpeg). */
export async function synthesize(text, lang, outWav) {
  // `--` terminates option parsing, so a reply that begins with "-" is spoken as
  // text and can't smuggle flags into `say` (argv flag injection).
  await execFileP("say", ["-v", pickVoice(lang), "-o", outWav, "--data-format=LEI16@16000", "--", String(text)]);
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
