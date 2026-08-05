// One build at a time, shared by everyone waiting for it.
//
// The naive guard — `if (now - cache.at > TTL) cache = { at: now, body: await build() }`
// — only advances its timestamp once the build RESOLVES. A payload build here can take
// most of ten seconds (an 8 s usage-endpoint timeout plus the local scans), and both
// the 12 s USB tick and every device poll go through it, so each caller arriving inside
// that window started a build of its own. Every one of those then hit the same
// rate-limited usage endpoint — the duplicates manufactured the 429s that the backoff
// went on to punish us for.

/**
 * Wrap an async builder in a TTL cache that collapses concurrent callers.
 *
 * A rejected build is not cached: the in-flight slot is released so the next caller
 * retries. Callers past the TTL wait for the fresh value rather than being handed a
 * stale one — freshness of this payload is the whole point of it.
 *
 * @param build  () => Promise<value>
 * @param ttlMs  how long a finished build stays servable
 * @param now    injectable clock for tests
 */
export function singleFlightCache(build, { ttlMs, now = Date.now }) {
  let at = 0, body = null, inFlight = null;
  return function get() {
    if (body !== null && now() - at <= ttlMs) return Promise.resolve(body);
    if (!inFlight) {
      // Promise.resolve().then(build) so a synchronous throw inside build surfaces as a
      // rejection to the joiners instead of blowing up whichever caller got there first.
      inFlight = Promise.resolve().then(build)
        .then((value) => { at = now(); body = value; return value; })
        .finally(() => { inFlight = null; });
    }
    return inFlight;
  };
}
