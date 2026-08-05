import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  USAGE_FRESH_MS, BACKOFF_BASE_MS, BACKOFF_MAX_MS,
  nextBackoffMs, readPeerUsage,
} from "../lib/usage.mjs";

const noJitter = { rand: () => 0.5 };   // 0.5 -> factor 1.0, so the maths is exact

/* ---------------------------------------------------------------- *
 * backoff                                                          *
 * ---------------------------------------------------------------- */

// The bug this whole module exists for: the old cap was 600 s while a reading goes
// CACHED at 300 s, so a maxed-out backoff could never land a poll inside the freshness
// window — the panel was structurally stuck on CACHED once it got there.
test("max backoff stays inside the freshness window", () => {
  assert.ok(BACKOFF_MAX_MS < USAGE_FRESH_MS,
    `backoff cap ${BACKOFF_MAX_MS} must be < freshness window ${USAGE_FRESH_MS}`);
});

test("backoff starts at one doubling of the base and grows", () => {
  const first = nextBackoffMs(0, noJitter);
  assert.equal(first, BACKOFF_BASE_MS * 2);
  assert.equal(nextBackoffMs(first, noJitter), BACKOFF_BASE_MS * 4);
});

test("backoff never exceeds the cap, however long it has been failing", () => {
  let ms = 0;
  for (let i = 0; i < 50; i++) {
    ms = nextBackoffMs(ms, noJitter);
    assert.ok(ms <= BACKOFF_MAX_MS, `grew past the cap on attempt ${i}: ${ms}`);
  }
  assert.equal(ms, BACKOFF_MAX_MS);
});

// Anthropic answers a usage 429 with `retry-after: 0`. Taken literally that means
// "retry now", which would hammer the endpoint we are being throttled by.
test("a non-positive retry-after is ignored in favour of the exponential value", () => {
  assert.equal(nextBackoffMs(0, { ...noJitter, retryAfterMs: 0 }), BACKOFF_BASE_MS * 2);
  assert.equal(nextBackoffMs(0, { ...noJitter, retryAfterMs: -5000 }), BACKOFF_BASE_MS * 2);
});

test("a sane retry-after wins but is clamped to the backoff range", () => {
  assert.equal(nextBackoffMs(0, { ...noJitter, retryAfterMs: 45_000 }), 45_000);
  assert.equal(nextBackoffMs(0, { ...noJitter, retryAfterMs: 1_000 }), BACKOFF_BASE_MS);
  assert.equal(nextBackoffMs(0, { ...noJitter, retryAfterMs: 3_600_000 }), BACKOFF_MAX_MS);
});

// Two pollers on one token that both back off by the same fixed amount re-collide on
// every retry; jitter breaks the lockstep.
test("jitter spreads retries by +/-25% and never returns a non-positive delay", () => {
  const base = BACKOFF_BASE_MS * 2;
  assert.equal(nextBackoffMs(0, { rand: () => 0 }), base * 0.75);
  assert.equal(nextBackoffMs(0, { rand: () => 0.999999 }), Math.round(base * 1.2499995));
  for (let i = 0; i < 200; i++) {
    const ms = nextBackoffMs(0);
    assert.ok(ms >= base * 0.75 && ms <= base * 1.25, `jitter out of range: ${ms}`);
  }
});

/* ---------------------------------------------------------------- *
 * peer cache                                                       *
 * ---------------------------------------------------------------- */

const NOW = 1_800_000_000_000;
const dir = () => mkdtempSync(join(tmpdir(), "peer-"));
const writeCache = (d, name, obj) => {
  const fp = join(d, name);
  writeFileSync(fp, JSON.stringify(obj));
  return fp;
};
// The shape oh-my-claudecode's HUD writes to .usage-cache-anthropic.json.
const omcCache = (over = {}) => ({
  timestamp: NOW - 1000,
  data: {
    fiveHourPercent: 22,
    fiveHourResetsAt: "2026-08-05T07:20:00.996Z",
    weeklyPercent: 35,
    weeklyResetsAt: "2026-08-06T16:59:59.996Z",
  },
  error: false,
  source: "anthropic",
  rateLimited: true,
  lastSuccessAt: NOW - 60_000,
  ...over,
});

test("readPeerUsage returns null when no neighbour file exists", () => {
  assert.equal(readPeerUsage([join(dir(), "nope.json")], 0, NOW), null);
});

test("readPeerUsage maps a HUD cache into the shape fetchUsage resolves", () => {
  const fp = writeCache(dir(), "c.json", omcCache());
  const got = readPeerUsage([fp], 0, NOW);
  assert.equal(got.at, NOW - 60_000);
  assert.deepEqual(got.five_hour, { utilization: 22, resets_at: "2026-08-05T07:20:00.996Z" });
  assert.deepEqual(got.seven_day, { utilization: 35, resets_at: "2026-08-06T16:59:59.996Z" });
});

// The whole point is to fill a gap. A neighbour reading no newer than our own last
// good one is not worth adopting — and adopting it would rewind lastGoodAt.
test("readPeerUsage skips a reading that is not newer than ours", () => {
  const fp = writeCache(dir(), "c.json", omcCache());
  assert.equal(readPeerUsage([fp], NOW - 60_000, NOW), null);
  assert.equal(readPeerUsage([fp], NOW - 30_000, NOW), null);
  assert.ok(readPeerUsage([fp], NOW - 90_000, NOW));
});

test("readPeerUsage refuses another provider's reading", () => {
  const fp = writeCache(dir(), "c.json", omcCache({ source: "codex" }));
  assert.equal(readPeerUsage([fp], 0, NOW), null);
});

test("readPeerUsage falls back to timestamp when lastSuccessAt is absent", () => {
  const c = omcCache();
  delete c.lastSuccessAt;
  const fp = writeCache(dir(), "c.json", c);
  assert.equal(readPeerUsage([fp], 0, NOW).at, NOW - 1000);
});

test("readPeerUsage rejects a future timestamp", () => {
  const fp = writeCache(dir(), "c.json", omcCache({ lastSuccessAt: NOW + 60_000 }));
  assert.equal(readPeerUsage([fp], 0, NOW), null);
});

test("readPeerUsage survives junk and missing fields", () => {
  const d = dir();
  const bad = join(d, "bad.json");
  writeFileSync(bad, "{not json");
  assert.equal(readPeerUsage([bad], 0, NOW), null);
  assert.equal(readPeerUsage([writeCache(d, "a.json", omcCache({ data: {} }))], 0, NOW), null);
  assert.equal(readPeerUsage([writeCache(d, "b.json", omcCache({ data: { fiveHourPercent: "x" } }))], 0, NOW), null);
  // a five-hour reading is enough; the weekly one is optional
  const partial = readPeerUsage([writeCache(d, "c.json", omcCache({
    data: { fiveHourPercent: 22, fiveHourResetsAt: "2026-08-05T07:20:00.996Z" },
  }))], 0, NOW);
  assert.equal(partial.five_hour.utilization, 22);
  assert.equal(partial.seven_day, null);
});

test("readPeerUsage tries each path and takes the first usable one", () => {
  const d = dir();
  const missing = join(d, "gone.json");
  const junk = join(d, "junk.json");
  writeFileSync(junk, "{not json");
  const good = writeCache(d, "good.json", omcCache());
  assert.equal(readPeerUsage([missing, junk, good], 0, NOW).five_hour.utilization, 22);
});
