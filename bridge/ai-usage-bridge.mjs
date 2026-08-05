#!/usr/bin/env node
// AI Usage Bridge — reads your local Claude Code login, asks Anthropic for your
// own usage, and serves it as plain JSON on your LAN so an ESP32 (or anything)
// can display it. Zero dependencies — Node's stdlib only.
//
//   node ai-usage-bridge.mjs           # serve on 0.0.0.0:8787
//   PORT=9000 node ai-usage-bridge.mjs # custom port
//
// Privacy: your token is read locally and used ONLY to call
// https://api.anthropic.com/api/oauth/usage (the same endpoint Claude Code's
// /status uses). It is never written to disk or sent anywhere else. The JSON we
// expose contains percentages + reset times + model name — never the token.

import http from "node:http";
import https from "node:https";
import { execFile, execFileSync } from "node:child_process";
import { readFile, readdir, stat, open, mkdtemp, writeFile } from "node:fs/promises";
import { existsSync, openSync, writeSync, closeSync, readdirSync, readFileSync, writeFileSync, mkdirSync, createReadStream } from "node:fs";
import os from "node:os";
import path from "node:path";

import { readSystem } from "./lib/system.mjs";
import { loadOrCreateToken, tokensMatch } from "./lib/pairing.mjs";
import { loadActionsConfig, validateAction, parseUsbAction } from "./lib/actions.mjs";
import { handleVoice, NoSpeech } from "./lib/voice.mjs";
import { askLLM, providerList, setActiveProvider } from "./lib/voice-providers.mjs";
import { advertise } from "./lib/mdns.mjs";
import { USAGE_POLL_MS, USAGE_FRESH_MS, nextBackoffMs, readPeerUsage } from "./lib/usage.mjs";
import { singleFlightCache } from "./lib/single-flight.mjs";
// note: execFile is already imported from "node:child_process" above.

const PORT = Number(process.env.PORT || 8787);
const HOST = process.env.HOST || "0.0.0.0";
const HOME = os.homedir();
const UA = "AIUsageBridge/1.0 (self-hosted)";

const REMOTE_ENABLED = process.env.REMOTE !== "0";           // remote on unless REMOTE=0
const PAIR_PATH = path.join(HOME, ".config", "ai-usage-bridge", "pairing.json");
const ACTIONS_PATH = process.env.ACTIONS || path.join(HOME, ".config", "ai-usage-bridge", "actions.json");
const PAIR_TOKEN = REMOTE_ENABLED ? loadOrCreateToken(PAIR_PATH) : null;
let prevNet = null;

/* ------------------------------------------------------------------ *
 * 1. Read Claude Code's OAuth credentials (no Keychain prompt on mac) *
 * ------------------------------------------------------------------ */
function execFileP(cmd, args) {
  return new Promise((resolve, reject) => {
    execFile(cmd, args, { timeout: 5000 }, (err, stdout) =>
      err ? reject(err) : resolve(stdout));
  });
}

async function readClaudeToken() {
  // a) macOS login Keychain — read via Apple's own `security` tool so the
  //    Apple-signed binary reads the item without a GUI prompt.
  if (process.platform === "darwin") {
    try {
      const out = await execFileP("/usr/bin/security",
        ["find-generic-password", "-w", "-s", "Claude Code-credentials"]);
      const tok = pickToken(JSON.parse(out));
      if (tok) return tok;
    } catch { /* fall through to file */ }
  }
  // b) Cross-platform fallback — the credentials file Claude Code writes.
  for (const rel of [".claude/.credentials.json", ".config/claude/.credentials.json"]) {
    const fp = path.join(HOME, rel);
    if (existsSync(fp)) {
      try {
        const tok = pickToken(JSON.parse(await readFile(fp, "utf8")));
        if (tok) return tok;
      } catch { /* try next */ }
    }
  }
  return null;
}

// The credential blob is `{ claudeAiOauth: { accessToken, ... } }`.
function pickToken(obj) {
  const o = obj?.claudeAiOauth ?? obj;
  return o?.accessToken || o?.access_token || null;
}

/* ------------------------------------------------------------------ *
 * 2. Ask Anthropic for this account's usage                          *
 * ------------------------------------------------------------------ */
function fetchUsage(token) {
  return new Promise((resolve, reject) => {
    const req = https.request("https://api.anthropic.com/api/oauth/usage", {
      method: "GET",
      headers: {
        "Authorization": `Bearer ${token}`,
        "anthropic-beta": "oauth-2025-04-20",
        "anthropic-version": "2023-06-01",
        "User-Agent": UA,
      },
    }, (res) => {
      let body = "";
      res.on("data", (d) => (body += d));
      res.on("end", () => {
        if (res.statusCode === 401 || res.statusCode === 403)
          return reject(new Error("unauthorized"));
        if (res.statusCode !== 200) {
          const err = new Error(`http ${res.statusCode}`);
          // Retry-After is advice, not a promise — nextBackoffMs decides what to do
          // with it (Anthropic sends `retry-after: 0` on a usage 429).
          const after = Number(res.headers["retry-after"]);
          if (Number.isFinite(after) && after > 0) err.retryAfterMs = after * 1000;
          return reject(err);
        }
        try { resolve(JSON.parse(body)); }
        catch { reject(new Error("bad json")); }
      });
    });
    req.on("error", reject);
    req.setTimeout(8000, () => req.destroy(new Error("timeout")));
    req.end();
  });
}

/* ------------------------------------------------------------------ *
 * 3. Which model + effort you're actually on (local, read-only)      *
 * ------------------------------------------------------------------ */
async function currentModelId() {
  const root = path.join(HOME, ".claude", "projects");
  if (!existsSync(root)) return null;
  const files = await newestTranscripts(root, 6);
  let bestTs = "", bestModel = null;
  for (const f of files) {
    const hit = await latestModelIn(f);
    if (hit && hit.ts > bestTs) { bestTs = hit.ts; bestModel = hit.model; }
  }
  return bestModel;
}

async function newestTranscripts(root, limit) {
  const found = [];
  async function walk(dir) {
    let entries;
    try { entries = await readdir(dir, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      const fp = path.join(dir, e.name);
      if (e.isDirectory()) await walk(fp);
      else if (e.name.endsWith(".jsonl")) {
        try { found.push({ fp, mtime: (await stat(fp)).mtimeMs }); } catch { /* skip */ }
      }
    }
  }
  await walk(root);
  return found.sort((a, b) => b.mtime - a.mtime).slice(0, limit).map((x) => x.fp);
}

// Scan the tail of a transcript for the most recent real assistant model.
async function latestModelIn(fp) {
  let fh;
  try { fh = await open(fp, "r"); } catch { return null; }
  try {
    const { size } = await fh.stat();
    const window = 262_144;
    const start = size > window ? size - window : 0;
    const buf = Buffer.alloc(size - start);
    await fh.read(buf, 0, buf.length, start);
    const lines = buf.toString("utf8").split("\n");
    for (let i = lines.length - 1; i >= 0; i--) {
      const line = lines[i].trim();
      if (!line) continue;
      let obj; try { obj = JSON.parse(line); } catch { continue; }
      if (obj.isSidechain === true) continue;            // subagent message
      const model = obj?.message?.model;
      if (!model || model.startsWith("<")) continue;      // skip <synthetic>
      return { ts: obj.timestamp || "", model };
    }
  } finally { await fh.close(); }
  return null;
}

async function currentEffort() {
  for (const name of [".claude/settings.local.json", ".claude/settings.json"]) {
    const fp = path.join(HOME, name);
    if (!existsSync(fp)) continue;
    try {
      const o = JSON.parse(await readFile(fp, "utf8"));
      if (o.effortLevel) return prettyEffort(o.effortLevel);
    } catch { /* next */ }
  }
  return null;
}

/* "claude-opus-4-8"        -> "Opus 4.8"
   "claude-haiku-4-5-...."  -> "Haiku 4.5"
   "claude-opus-4-8[1m]"    -> "Opus 4.8 · 1M"                          */
function prettyModel(id) {
  if (!id) return null;
  let base = id, suffix = "";
  const open = base.indexOf("["), close = base.indexOf("]");
  if (open >= 0 && close > open) { suffix = base.slice(open + 1, close); base = base.slice(0, open); }
  if (base.startsWith("claude-")) base = base.slice(7);
  const parts = base.split("-");
  const family = parts.shift() || base;
  const familyName = family.charAt(0).toUpperCase() + family.slice(1);
  const version = [];
  for (const p of parts) { if (/^\d{1,2}$/.test(p)) version.push(p); else break; }
  let name = version.length ? `${familyName} ${version.join(".")}` : familyName;
  if (suffix) name += ` · ${suffix.toUpperCase()}`;
  return name;
}

function prettyEffort(raw) {
  const m = { low: "Low", medium: "Medium", high: "High", xhigh: "xHigh", max: "Max" };
  return m[String(raw).toLowerCase()] || (raw.charAt(0).toUpperCase() + raw.slice(1));
}

/* ------------------------------------------------------------------ *
 * 4. Detect other CLIs (scaffold — Claude is the live feed today)    *
 * ------------------------------------------------------------------ */
function detect(rel) { return rel.some((r) => existsSync(path.join(HOME, r))); }
const providerLinks = {
  codex: () => detect([".codex/auth.json"]),
  gemini: () => detect([".gemini/oauth_creds.json"]),
};

/* ------------------------------------------------------------------ *
 * 5. Assemble the payload the device polls                           *
 * ------------------------------------------------------------------ */
function num(x) { return typeof x === "number" ? Math.round(x) : null; }

// Seconds until an ISO reset time — so the device can count down locally
// without NTP or ISO parsing.
function secsUntil(iso) {
  if (!iso) return null;
  const t = Date.parse(iso);
  return Number.isNaN(t) ? null : Math.max(0, Math.round((t - Date.now()) / 1000));
}
function win(w) {
  return { util: num(w.utilization), resets_at: w.resets_at ?? null, reset_in: secsUntil(w.resets_at) };
}

// Ask Claude a one-shot question for the voice assistant (Pixie). Reuses the same
// OAuth token as the dashboard, so it shares the account rate limit (429 -> brief
// retry, then surface). Model overridable via PIXIE_MODEL.
// Rate limits are per-model, so a busy model (429) doesn't mean the account is out.
// Try Pixie's models in order and fall back on 429 \u2014 Haiku is fast + light, ideal
// for short voice replies, so it's the natural fallback when Sonnet is throttled.
const PIXIE_MODELS = process.env.PIXIE_MODEL
  ? [process.env.PIXIE_MODEL]
  : ["claude-sonnet-5", "claude-haiku-4-5-20251001"];

// Give Pixie live web access (prices, weather, news, today's date) via Anthropic's
// server-side web_search tool — the model runs the search itself. Set
// PIXIE_WEB_SEARCH=0 to disable. Use the BASIC variant (web_search_20250305): it
// works on every Pixie model incl. Haiku, whereas web_search_20260209 (dynamic
// filtering) is Sonnet-5/Opus-only and would break the Haiku fallback. Verified the
// Claude Code OAuth token accepts this tool (Haiku 200 with a real, cited answer).
const PIXIE_WEB_SEARCH = process.env.PIXIE_WEB_SEARCH !== "0";

async function askClaude(prompt) {
  const token = await readClaudeToken();
  if (!token) throw new Error("not linked");
  const mkBody = (model) => JSON.stringify({
    model,
    // web search interleaves tool-use blocks with the answer, so it needs more room
    // than a plain reply; keep it tight (300) when search is off.
    max_tokens: PIXIE_WEB_SEARCH ? 1024 : 300,
    system: "You are Pixie, a concise voice assistant living on a tiny desk display. " +
      "Answer in the user's language, in 1-3 short spoken sentences. Plain text only \u2014 no markdown, lists, or code. " +
      "You can search the web \u2014 use it for real-time facts (prices, weather, news, today's date) and reply with the number or fact itself, not the source or a URL.",
    messages: [{ role: "user", content: String(prompt).slice(0, 2000) }],
    ...(PIXIE_WEB_SEARCH ? { tools: [{ type: "web_search_20250305", name: "web_search", max_uses: 3 }] } : {}),
  });
  let lastStatus = 0;
  for (const model of PIXIE_MODELS) {
    for (let attempt = 0; attempt < 2; attempt++) {
      const res = await fetch("https://api.anthropic.com/v1/messages", {
        method: "POST",
        headers: {
          "Authorization": `Bearer ${token}`,
          "anthropic-beta": "oauth-2025-04-20",
          "anthropic-version": "2023-06-01",
          "content-type": "application/json",
          "User-Agent": UA,
        },
        body: mkBody(model),
      });
      if (res.ok) {
        const j = await res.json();
        // With web search the reply interleaves server_tool_use / web_search_tool_result
        // blocks with text — join every text block, not just content[0].
        return (j?.content || []).filter((b) => b?.type === "text").map((b) => b.text).join(" ").trim();
      }
      lastStatus = res.status;
      if (res.status === 429) {
        if (attempt === 0) { await new Promise((r) => setTimeout(r, 1200)); continue; }  // brief retry
        break;   // still throttled on this model -> try the next model
      }
      throw new Error(`claude ${res.status}`);   // non-429 -> real error
    }
  }
  throw new Error(`claude ${lastStatus || "unavailable"}`);
}

// Last-known-good Claude fields. The Anthropic usage endpoint is rate-limited
// (429) — and it's shared with the ai-usage-bar menu-bar app on the same token —
// so a poll can transiently fail. Rather than emit nulls (which blanks the
// device to "no live data"), we reuse the last real values until a fresh poll
// succeeds. `lastGoodAt` lets callers reason about staleness if needed.
let lastGoodClaude = { model: null, effort: null, five_hour: null, seven_day: null };
let lastGoodAt = 0;

// Usage-endpoint backoff. It's rate-limited (shared with the menu-bar app), so on a
// 429 we stop polling it for a growing window to let the limit recover instead of
// hammering it every cycle. Reset on the next success. The window's shape — and the
// cap that keeps it inside USAGE_FRESH_MS — lives in lib/usage.mjs.
let usageBackoffUntil = 0;
let usageBackoffMs = 0;
// Why a poll last failed. `error` used to be set only on the cycle that actually
// polled, so during a backoff window /usage reported `stale: true` with no reason at
// all — a rate-limited bridge looked identical to a frozen one, which is exactly how a
// 20-minute 429 streak went undiagnosed. Remember it until a poll succeeds.
let lastUsageError = null;

// Neighbours that already pay for this endpoint and cache the answer on disk. The
// oh-my-claudecode HUD statusline polls it every 90 s — faster than we do — so when we
// lose the race its file usually holds a reading newer than ours, free to read.
// Absent on a machine without the HUD, which is fine: readPeerUsage just returns null.
const CLAUDE_CFG = process.env.CLAUDE_CONFIG_DIR?.trim() || path.join(HOME, ".claude");
const PEER_USAGE_PATHS = [
  path.join(CLAUDE_CFG, "plugins", "oh-my-claudecode", ".usage-cache-anthropic.json"),
  path.join(CLAUDE_CFG, "plugins", "oh-my-claudecode", ".usage-cache.json"),
];

// Persist last-known-good to disk so a bridge *restart* (or a cold start during a
// 429) still shows the last real reading instead of blanking to "no live data".
const LAST_GOOD_PATH = path.join(HOME, ".config", "ai-usage-bridge", "last-good.json");
(function loadLastGood() {
  try {
    const o = JSON.parse(readFileSync(LAST_GOOD_PATH, "utf8"));
    if (o && o.claude) { lastGoodClaude = { ...lastGoodClaude, ...o.claude }; lastGoodAt = o.at || 0; }
  } catch { /* no file yet — fine */ }
})();
function saveLastGood() {
  try {
    mkdirSync(path.dirname(LAST_GOOD_PATH), { recursive: true });
    writeFileSync(LAST_GOOD_PATH, JSON.stringify({ claude: lastGoodClaude, at: lastGoodAt }));
  } catch { /* best-effort */ }
}

async function buildPayload() {
  const providers = {
    claude: { name: "Claude", linked: false, model: null, effort: null, five_hour: null, seven_day: null },
    codex: { name: "Codex", linked: providerLinks.codex(), model: null, effort: null, five_hour: null, seven_day: null },
    gemini: { name: "Gemini", linked: providerLinks.gemini(), model: null, effort: null, five_hour: null, seven_day: null },
  };

  const token = await readClaudeToken();
  if (token) {
    const c = providers.claude;
    c.linked = true;

    // Model + effort come from local files (free) — always refresh, independent
    // of the rate-limited usage endpoint (so a usage 429 doesn't wipe the model).
    try { c.model = prettyModel(await currentModelId()); } catch { /* keep null */ }
    try { c.effort = await currentEffort(); } catch { /* keep null */ }

    // Usage: poll on a gentle cadence. The `usageBackoffUntil` gate now serves double
    // duty — after a SUCCESS we push it out by USAGE_POLL_MS (steady, low-pressure
    // cadence); after a FAILURE (e.g. 429) we grow a backoff window instead. Either
    // way we don't re-hit the shared, rate-limited endpoint until the gate opens.
    // `usageAt` is when the reading we ended up with was actually taken — now for our
    // own poll, the neighbour's fetch time for a borrowed one. It drives lastGoodAt, so
    // borrowing can never claim data is fresher than it is.
    let usageAt = 0;
    if (Date.now() >= usageBackoffUntil) {
      try {
        const usage = await fetchUsage(token);
        c.five_hour = usage.five_hour ? win(usage.five_hour) : null;
        c.seven_day = usage.seven_day ? win(usage.seven_day) : null;
        usageAt = Date.now();
        lastUsageError = null;
        usageBackoffMs = 0; usageBackoffUntil = Date.now() + USAGE_POLL_MS;   // steady cadence
      } catch (e) {
        usageBackoffMs = nextBackoffMs(usageBackoffMs, { retryAfterMs: e.retryAfterMs });
        usageBackoffUntil = Date.now() + usageBackoffMs;
        lastUsageError = e.message;                   // e.g. "http 429" / "unauthorized"
      }
    }
    // Didn't get one ourselves? Another tool on this machine may already have a newer
    // reading cached — take it rather than queue up behind the same rate limit. Costs
    // no API call, so it also relieves the pressure that caused the miss.
    if (!usageAt) {
      const peer = readPeerUsage(PEER_USAGE_PATHS, lastGoodAt);
      if (peer) {
        c.five_hour = win(peer.five_hour);
        c.seven_day = peer.seven_day ? win(peer.seven_day) : null;
        usageAt = peer.at;
      }
    }
    if (lastUsageError) c.error = lastUsageError;     // visible during a backoff window too

    // Sticky last-known-good: keep showing the last real reading instead of blanking.
    for (const k of ["model", "effort", "five_hour", "seven_day"]) {
      if (c[k] == null) c[k] = lastGoodClaude[k];
      else lastGoodClaude[k] = c[k];
    }
    if (usageAt > lastGoodAt && c.five_hour) { lastGoodAt = usageAt; saveLastGood(); }
    // CACHED only when the reading is genuinely OLD (we've been failing a while), not
    // merely "didn't poll this cycle" — otherwise the gentle cadence + any transient
    // 429 would flag CACHED constantly. A reading within USAGE_FRESH_MS stays LIVE.
    // A terminal auth failure (401/403 → "unauthorized") never self-recovers the way a
    // 429 does, so surface it immediately instead of hiding it behind the fresh window:
    // mark stale + keep the error so a revoked/expired token stays diagnosable via /usage.
    const usageAgeMs = lastGoodAt ? Date.now() - lastGoodAt : Infinity;
    const authFailed = c.error === "unauthorized";
    c.stale = !!(c.five_hour || c.seven_day) && (usageAgeMs > USAGE_FRESH_MS || authFailed);
    if (c.stale) c.usage_age_s = Number.isFinite(usageAgeMs) ? Math.round(usageAgeMs / 1000) : null;
    else if (!authFailed) delete c.error;   // hide a transient 429/timeout on a fresh reading; keep auth errors
  }

  let system = null;
  try { const s = await readSystem(prevNet); system = s.system; prevNet = s.net; } catch { /* keep null */ }
  return { ok: true, updated: new Date().toISOString(), providers, system };
}

/* ------------------------------------------------------------------ *
 * 6. HTTP server                                                     *
 * ------------------------------------------------------------------ */
const TTL = 60_000;   // don't hammer Anthropic (shared token → 429); device may poll faster
// Every consumer — HTTP handlers and the USB writer alike — shares this one builder, so
// overlapping callers wait on a single build instead of each starting their own and
// racing to the same rate-limited endpoint. See lib/single-flight.mjs.
const currentPayload = singleFlightCache(buildPayload, { ttlMs: TTL });

const server = http.createServer(async (req, res) => {
  const url = (req.url || "/").split("?")[0];
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Cache-Control", "no-store");

  if (url === "/action") {
    if (!REMOTE_ENABLED) return send(res, 403, { ok: false, error: "remote disabled" });
    if (req.method !== "POST") return send(res, 405, { ok: false, error: "POST only" });
    let raw = "", done = false;
    const reply = (code, obj) => { if (!done) { done = true; send(res, code, obj); } };
    req.on("error", () => reply(400, { ok: false, error: "bad request" }));
    req.on("data", (d) => { raw += d; if (raw.length > 8192) { reply(413, { ok: false, error: "too large" }); req.destroy(); } });
    req.on("end", () => {
      if (done) return;
      let body; try { body = JSON.parse(raw || "{}"); } catch { return reply(400, { ok: false, error: "bad json" }); }
      if (!tokensMatch(body.token || "", PAIR_TOKEN)) return reply(401, { ok: false, error: "unauthorized" });
      const v = validateAction(body, loadActionsConfig(ACTIONS_PATH));
      if (!v.ok) return reply(400, v);
      execFile(v.cmd.file, v.cmd.args, { timeout: 5000 }, (e) =>
        e ? reply(500, { ok: false, error: "action failed" }) : reply(200, { ok: true }));
    });
    return;
  }

  if (url === "/voice") {
    if (req.method !== "POST") return send(res, 405, { ok: false, error: "POST only" });
    if (!tokensMatch(req.headers["x-pixie-token"] || "", PAIR_TOKEN))
      return send(res, 401, { ok: false, error: "unauthorized" });
    const chunks = []; let size = 0; let tooBig = false;
    req.on("error", () => {});
    req.on("data", (d) => { size += d.length; if (size > 4_000_000) { tooBig = true; req.destroy(); } else chunks.push(d); });
    req.on("end", async () => {
      if (tooBig) return send(res, 413, { ok: false, error: "too large" });
      try {
        const dir = await mkdtemp(path.join(os.tmpdir(), "pixie-"));
        const inWav = path.join(dir, "in.wav");
        const outWav = path.join(dir, "out.wav");
        await writeFile(inWav, Buffer.concat(chunks));
        const { transcript, reply } = await handleVoice(inWav, (p) => askLLM(p, askClaude), outWav);
        const audio = await readFile(outWav);
        res.writeHead(200, {
          "Content-Type": "audio/wav",
          "Content-Length": audio.length,
          "X-Transcript": encodeURIComponent(transcript),
          "X-Reply": encodeURIComponent(reply),
          "Cache-Control": "no-store",
        });
        res.end(audio);
      } catch (e) {
        if (e instanceof NoSpeech) return send(res, 422, { ok: false, error: "no speech" });
        send(res, 500, { ok: false, error: e.message });
      }
    });
    return;
  }

  // Voice providers: list (open) + switch the active one (token-gated).
  if (url === "/voice/providers") {
    return send(res, 200, { ok: true, ...providerList() });
  }
  if (url === "/voice/provider") {
    if (req.method !== "POST") return send(res, 405, { ok: false, error: "POST only" });
    if (!tokensMatch(req.headers["x-pixie-token"] || "", PAIR_TOKEN))
      return send(res, 401, { ok: false, error: "unauthorized" });
    let raw = "";
    req.on("data", (d) => { raw += d; if (raw.length > 1024) req.destroy(); });
    req.on("end", () => {
      let id; try { id = JSON.parse(raw || "{}").id; } catch { return send(res, 400, { ok: false, error: "bad json" }); }
      const now = setActiveProvider(String(id || ""));
      if (!now) return send(res, 400, { ok: false, error: "unknown provider" });
      send(res, 200, { ok: true, active: now });
    });
    return;
  }

  if (url === "/" || url === "/health") {
    return send(res, 200, { ok: true, service: "ai-usage-bridge", endpoint: "/usage" });
  }
  if (url !== "/usage") return send(res, 404, { ok: false, error: "not found" });

  try {
    send(res, 200, await currentPayload());
  } catch (e) {
    send(res, 500, { ok: false, error: e.message });
  }
});

/* ------------------------------------------------------------------ *
 * 7. USB-serial transport (optional): push /usage frames to the device *
 *    over the USB cable it's already powered by — works with no Wi-Fi   *
 *    (e.g. carried to the office). Auto-on when one usbmodem port is     *
 *    present; set USB=0 to disable, or USB_PORT=/dev/cu.x to pin it.     *
 * ------------------------------------------------------------------ */
// Pick the board's serial port. USB_PORT is a *preference*, not a permanent lock:
// the S3's native USB re-enumerates under a different /dev/cu.usbmodemNNNNN every
// time it is replugged (or the Mac reboots), so a pinned path that has since gone
// away must fall back to auto-detect. Was: `return process.env.USB_PORT` outright,
// which pinned the transport to a dead node forever — the bridge then retried
// `stty` on a nonexistent device every 12 s and the device never got a frame again,
// which is fatal on a LAN with client isolation where USB is the only path left.
function detectUsbPort() {
  if (process.env.USB === "0") return null;
  const pinned = process.env.USB_PORT;
  if (pinned && existsSync(pinned)) return pinned;
  try {
    const ports = readdirSync("/dev").filter((f) => f.startsWith("cu.usbmodem")).map((f) => "/dev/" + f);
    return ports.length === 1 ? ports[0] : null;   // auto only when unambiguous
  } catch { return null; }
}

// Bidirectional USB: push /usage frames to the device AND read Remote actions back
// from it (`@ACT` lines), so the Remote works over the cable with no Wi-Fi.
function startUsbWriter() {
  if (process.env.USB === "0") return;                 // USB explicitly disabled
  let fd = null, rs = null, buf = "", port = null;
  const close = () => {
    try { if (rs) rs.destroy(); } catch { /* ignore */ }
    try { if (fd !== null) closeSync(fd); } catch { /* ignore */ }
    fd = null; rs = null; buf = "";
  };
  const onLine = (line) => {
    const p = parseUsbAction(line.trim());
    if (!p) return;
    if (!REMOTE_ENABLED || !tokensMatch(p.token, PAIR_TOKEN)) return;
    const v = validateAction(p.body, loadActionsConfig(ACTIONS_PATH));
    if (!v.ok) return;
    execFile(v.cmd.file, v.cmd.args, { timeout: 5000 }, () => {});   // fire-and-forget
  };
  // Re-detects the port every call, so a board plugged in AFTER the bridge started
  // (or one that re-enumerated) is picked up on the next tick — no restart needed.
  // Was: detect once at startup and give up forever if the board wasn't present yet.
  // Log only on state change: this runs every 12 s forever, so an unconditional
  // message (or stty's own stderr) buries every other line in the log — the very
  // thing that hid a wedged transport for a week.
  let lastNote = "";
  const note = (msg) => { if (msg !== lastNote) { lastNote = msg; console.log(msg); } };
  const openPort = () => {
    port = detectUsbPort();
    if (!port) {                                       // no board yet — retry next tick
      note("  usb: no board port found — waiting for /dev/cu.usbmodem* (replug, or set USB_PORT)");
      return;
    }
    try {
      // stdio ignored: a vanished port makes stty write to stderr on every tick.
      execFileSync("stty", ["-f", port, "115200", "raw", "-echo"], { stdio: "ignore" });   // raw; no shell (injection-safe)
      fd = openSync(port, "r+");                        // read + write
      rs = createReadStream(null, { fd, autoClose: false });
      rs.on("data", (chunk) => {
        buf += chunk.toString("utf8");
        let i;
        while ((i = buf.indexOf("\n")) >= 0) { onLine(buf.slice(0, i)); buf = buf.slice(i + 1); }
        if (buf.length > 8192) buf = "";                 // runaway guard
      });
      rs.on("error", close);
      note(`  usb: bridge on ${port} — frames out + Remote actions in (set USB=0 to disable)`);
    } catch {
      close();
      note(`  usb: ${port} present but not openable — retrying every 12 s`);
    }
  };
  openPort();
  const tick = async () => {
    try {
      if (fd === null) { openPort(); if (fd === null) return; }
      writeSync(fd, JSON.stringify(await currentPayload()) + "\n");
    } catch { close(); }   // device may have re-enumerated; reopen next tick
  };
  tick();
  setInterval(tick, 12_000);
}

function send(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { "Content-Type": "application/json; charset=utf-8" });
  res.end(body);
}

server.listen(PORT, HOST, () => {
  const ips = Object.values(os.networkInterfaces()).flat()
    .filter((i) => i && i.family === "IPv4" && !i.internal).map((i) => i.address);
  console.log(`AI Usage Bridge → http://${ips[0] || "localhost"}:${PORT}/usage`);
  console.log(`  point the ESP32 at this address (LAN only).`);
  if (!ips.length) console.log("  (no LAN IPv4 found — check your network)");
    if (REMOTE_ENABLED) {
      advertise(PORT);
      console.log(`  remote: enabled — pairing token ${PAIR_TOKEN} (enter this on the device)`);
      console.log(`  shortcuts config: ${ACTIONS_PATH} (copy actions.example.json to start)`);
    } else {
      console.log("  remote: disabled (REMOTE=0) — dashboard only");
    }
    startUsbWriter();
});
