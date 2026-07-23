// Pixie voice — multi-provider registry + dispatcher.
//
// Adding a new provider is a CONFIG change, not a code change: drop an entry into
// ~/.config/ai-usage-bridge/voice-providers.json. Any OpenAI-compatible chat API
// (GLM/z.ai, OpenAI, Kimi/Moonshot, Together, local Ollama, …) works by giving it
// a `url` + `key` + `models`. "claude" is the built-in default (uses the Claude
// Code OAuth token — no key needed).
//
// Example ~/.config/ai-usage-bridge/voice-providers.json:
// {
//   "active": "claude",
//   "providers": {
//     "claude": { "name": "Claude", "builtin": true },
//     "glm":    { "name": "GLM (z.ai)",
//                 "url": "https://api.z.ai/api/paas/v4/chat/completions",
//                 "key": "YOUR_ZAI_KEY", "models": ["glm-4.6", "glm-4.5-flash"] }
//   }
// }

import { existsSync, readFileSync, writeFileSync, mkdirSync } from "node:fs";
import os from "node:os";
import path from "node:path";

const CONFIG_PATH = process.env.VOICE_PROVIDERS ||
  path.join(os.homedir(), ".config", "ai-usage-bridge", "voice-providers.json");

const SYSTEM_PROMPT =
  "You are Pixie, a concise voice assistant living on a tiny desk display. Answer " +
  "in the user's language, in 1-3 short spoken sentences. Plain text only — no " +
  "markdown, lists, or code.";

// Claude is always available (built-in, OAuth). Others come from the config file.
const BUILTIN = { claude: { name: "Claude", builtin: true } };

export function loadProviders() {
  let cfg = { active: "claude", providers: {} };
  try {
    if (existsSync(CONFIG_PATH)) cfg = { ...cfg, ...JSON.parse(readFileSync(CONFIG_PATH, "utf8")) };
  } catch { /* malformed -> defaults */ }
  cfg.providers = { ...BUILTIN, ...(cfg.providers || {}) };
  if (!cfg.providers[cfg.active]) cfg.active = "claude";
  return cfg;
}

function saveProviders(cfg) {
  try {
    mkdirSync(path.dirname(CONFIG_PATH), { recursive: true });
    writeFileSync(CONFIG_PATH, JSON.stringify({ active: cfg.active, providers: cfg.providers }, null, 2));
  } catch { /* best-effort */ }
}

// The list the device shows (no secrets). [{id, name, active, models}]
export function providerList() {
  const cfg = loadProviders();
  return {
    active: cfg.active,
    providers: Object.entries(cfg.providers).map(([id, p]) => ({
      id, name: p.name || id, active: id === cfg.active, models: p.models || null,
    })),
  };
}

// Switch the active provider. Returns the new active id, or null if unknown.
export function setActiveProvider(id) {
  const cfg = loadProviders();
  if (!cfg.providers[id]) return null;
  cfg.active = id;
  saveProviders(cfg);
  return id;
}

// Call an OpenAI-compatible chat endpoint. Tries each model, 429 -> next model.
async function askOpenAICompatible(p, prompt) {
  const models = p.models && p.models.length ? p.models : ["default"];
  let last = 0;
  for (const model of models) {
    const res = await fetch(p.url, {
      method: "POST",
      headers: { "Authorization": `Bearer ${p.key}`, "Content-Type": "application/json" },
      body: JSON.stringify({
        model,
        max_tokens: 300,
        messages: [{ role: "system", content: SYSTEM_PROMPT },
                   { role: "user", content: String(prompt).slice(0, 2000) }],
      }),
    });
    if (res.ok) { const j = await res.json(); return (j?.choices?.[0]?.message?.content || "").trim(); }
    last = res.status;
    if (res.status !== 429) throw new Error(`${p.name || "provider"} ${res.status}`);
  }
  throw new Error(`${p.name || "provider"} ${last || "unavailable"}`);
}

/**
 * Ask the ACTIVE provider. `askClaude` (the built-in Anthropic path, injected by the
 * bridge so token handling stays in one place) is used when active === "claude".
 */
export async function askLLM(prompt, askClaude) {
  const cfg = loadProviders();
  const p = cfg.providers[cfg.active];
  if (!p || p.builtin || cfg.active === "claude") return askClaude(prompt);
  if (!p.url || !p.key) throw new Error(`${p.name || cfg.active}: missing url/key`);
  return askOpenAICompatible(p, prompt);
}
