import { test } from "node:test";
import assert from "node:assert/strict";
import { singleFlightCache } from "../lib/single-flight.mjs";

// A controllable clock, so TTL expiry is exact instead of slept for.
function clock(start = 1_000_000) {
  let t = start;
  return { now: () => t, advance: (ms) => { t += ms; } };
}
// Drain pending microtasks — singleFlightCache starts its build one tick late so that a
// synchronous throw inside it becomes a rejection rather than blowing up its first caller.
const tick = () => new Promise((r) => setImmediate(r));
// A build we can resolve by hand, to hold callers inside the same in-flight window.
function deferred() {
  let resolve, reject;
  const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
  return { promise, resolve, reject };
}

// The bug: the old guard advanced its timestamp only after the build RESOLVED, so
// every caller arriving during a build (up to ~10 s of it) started another one — and
// each extra build hit the rate-limited usage endpoint, manufacturing 429s.
test("concurrent callers share one build", async () => {
  const d = deferred();
  let builds = 0;
  const get = singleFlightCache(() => { builds++; return d.promise; }, { ttlMs: 60_000, now: clock().now });

  const all = [get(), get(), get()];
  await tick();   // build is deferred a microtask, so let it actually start
  assert.equal(builds, 1, "a second caller started its own build");
  d.resolve("payload");
  assert.deepEqual(await Promise.all(all), ["payload", "payload", "payload"]);
  assert.equal(builds, 1);
});

test("a hit inside the TTL does not rebuild", async () => {
  const c = clock();
  let builds = 0;
  const get = singleFlightCache(async () => `build-${++builds}`, { ttlMs: 60_000, now: c.now });

  assert.equal(await get(), "build-1");
  c.advance(59_999);
  assert.equal(await get(), "build-1");
  assert.equal(builds, 1);
});

test("the TTL is measured from when the build finished", async () => {
  const c = clock();
  let builds = 0;
  const get = singleFlightCache(async () => `build-${++builds}`, { ttlMs: 60_000, now: c.now });

  assert.equal(await get(), "build-1");
  c.advance(60_001);
  assert.equal(await get(), "build-2");
  assert.equal(builds, 2);
});

test("a failed build rejects every joiner and does not poison the next attempt", async () => {
  const d = deferred();
  let builds = 0;
  const get = singleFlightCache(() => { builds++; return builds === 1 ? d.promise : Promise.resolve("ok"); },
    { ttlMs: 60_000, now: clock().now });

  const a = get(), b = get();
  d.reject(new Error("http 429"));
  await assert.rejects(a, /http 429/);
  await assert.rejects(b, /http 429/);
  assert.equal(builds, 1);

  assert.equal(await get(), "ok");   // the in-flight slot was released, not left stuck
  assert.equal(builds, 2);
});

test("a build that throws synchronously becomes a rejection, not a crash", async () => {
  const get = singleFlightCache(() => { throw new Error("boom"); }, { ttlMs: 60_000, now: clock().now });
  await assert.rejects(get(), /boom/);
  await assert.rejects(get(), /boom/);   // still callable afterwards
});

test("callers arriving after a build get the cached value without rebuilding", async () => {
  const c = clock();
  let builds = 0;
  const get = singleFlightCache(async () => `build-${++builds}`, { ttlMs: 60_000, now: c.now });

  await get();
  const later = await Promise.all([get(), get(), get()]);
  assert.deepEqual(later, ["build-1", "build-1", "build-1"]);
  assert.equal(builds, 1);
});
