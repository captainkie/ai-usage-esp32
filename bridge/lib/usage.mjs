// Keeping the usage reading fresh under a rate limit we do not own.
//
// https://api.anthropic.com/api/oauth/usage is throttled per account, and the bridge
// is rarely the only thing on this machine asking. Anything else signed in with the
// same Claude Code OAuth token — the oh-my-claudecode HUD statusline polls it every
// 90 s, the menu-bar app, a `/status` in the CLI — draws from the same budget, so
// whoever asks second gets a 429 with `retry-after: 0`. Two fixes live here:
//
//   1. a backoff that stays inside the freshness window, so losing the race slows
//      the bridge down instead of parking it,
//   2. a way to read a neighbour's fresh answer off disk instead of asking again.

import { existsSync, readFileSync } from "node:fs";

export const USAGE_POLL_MS  = 120_000;   // min gap between successful usage polls
export const USAGE_FRESH_MS = 300_000;   // reading <5 min old = LIVE; older = CACHED

export const BACKOFF_BASE_MS = 15_000;
// The cap MUST stay below USAGE_FRESH_MS. It was 600 s against a 300 s freshness
// window, which meant that once the backoff maxed out the next attempt could not
// physically arrive before the reading went CACHED — so after a handful of 429s the
// panel showed CACHED permanently, with no way back short of a restart. A cap inside
// the window makes the worst case "a slow refresh", not "a stuck one".
export const BACKOFF_MAX_MS = 120_000;

/**
 * How long to wait before the next usage poll after a failure.
 *
 * @param prevMs        the previous backoff (0 / falsy on the first failure)
 * @param retryAfterMs  server's Retry-After in ms, if it sent a usable one
 * @param rand          injectable for tests; must return [0, 1)
 */
export function nextBackoffMs(prevMs, { retryAfterMs = 0, rand = Math.random } = {}) {
  const prev = Number(prevMs) > 0 ? Number(prevMs) : BACKOFF_BASE_MS;
  const grown = Math.min(prev * 2, BACKOFF_MAX_MS);
  // Anthropic answers a usage 429 with `retry-after: 0`; obeying that literally would
  // hammer the very endpoint throttling us, so only a positive hint is worth honouring.
  const base = retryAfterMs > 0
    ? Math.min(Math.max(retryAfterMs, BACKOFF_BASE_MS), BACKOFF_MAX_MS)
    : grown;
  // Jitter: two pollers that back off by identical fixed amounts re-collide on every
  // retry and one of them can starve indefinitely. +/-25% breaks the lockstep.
  return Math.round(base * (0.75 + rand() * 0.5));
}

/** Seconds until an ISO reset time, so the device counts down locally without NTP. */
export function secsUntil(iso, now = Date.now()) {
  if (!iso) return null;
  const t = Date.parse(iso);
  return Number.isNaN(t) ? null : Math.max(0, Math.round((t - now) / 1000));
}

/**
 * Stamp a live `reset_in` onto a usage window.
 *
 * `reset_in` is DERIVED, never stored. It used to be computed once inside win() and
 * then carried along with the reading — into last-known-good and out to
 * last-good.json — so a cached window counted down from a baseline as old as the
 * reading itself, and a restart served whatever the file was written with. `resets_at`
 * is the durable fact; the countdown is recomputed from it on every payload.
 *
 * Returns a fresh object: these windows are shared with the last-known-good copy, and
 * mutating one in place would write a stale countdown straight back into the store.
 */
export function withCountdown(w, now = Date.now()) {
  if (!w) return null;
  return { ...w, reset_in: secsUntil(w.resets_at, now) };
}

function pct(v) {
  const n = Number(v);
  return Number.isFinite(n) && n >= 0 && n <= 100 ? n : null;
}

function iso(v) {
  if (v instanceof Date) return Number.isNaN(v.getTime()) ? null : v.toISOString();
  return typeof v === "string" && v ? v : null;
}

/**
 * Borrow a fresher reading another local tool already paid for.
 *
 * Reads oh-my-claudecode's HUD cache (`.usage-cache-anthropic.json`) and returns it in
 * the same shape `fetchUsage` resolves, so callers can run it through the same `win()`.
 * Purely opportunistic: every failure mode — no file, junk JSON, another provider's
 * numbers, a reading no newer than ours — returns null and the caller carries on with
 * what it had. A machine without the HUD installed simply never gets a hit.
 *
 * @param paths        candidate cache files, most specific first
 * @param newerThanMs  our own last-good timestamp; a reading must beat it to be useful
 */
export function readPeerUsage(paths, newerThanMs = 0, now = Date.now()) {
  for (const fp of paths) {
    try {
      if (!existsSync(fp)) continue;
      const c = JSON.parse(readFileSync(fp, "utf8"));
      // The HUD writes one file per provider; a Codex or Gemini reading is not ours.
      if (c?.source && c.source !== "anthropic") continue;
      // `timestamp` is when the cache was last written (a 429 rewrites it too);
      // `lastSuccessAt` is when the numbers in it were actually fetched. Prefer the latter.
      const at = Number(c?.lastSuccessAt ?? c?.timestamp);
      if (!Number.isFinite(at) || at <= newerThanMs || at > now) continue;
      const five = pct(c?.data?.fiveHourPercent);
      if (five === null) continue;
      const seven = pct(c?.data?.weeklyPercent);
      return {
        at,
        five_hour: { utilization: five, resets_at: iso(c.data.fiveHourResetsAt) },
        seven_day: seven === null
          ? null
          : { utilization: seven, resets_at: iso(c.data.weeklyResetsAt) },
      };
    } catch { /* unreadable or not JSON — try the next candidate */ }
  }
  return null;
}
