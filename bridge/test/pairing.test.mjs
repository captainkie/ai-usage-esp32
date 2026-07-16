import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, readFileSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { loadOrCreateToken, tokensMatch } from "../lib/pairing.mjs";

test("loadOrCreateToken creates a 0600 file then reuses it", () => {
  const fp = join(mkdtempSync(join(tmpdir(), "pair-")), "pairing.json");
  const a = loadOrCreateToken(fp);
  assert.match(a, /^[0-9a-f]{32}$/);
  assert.equal(statSync(fp).mode & 0o777, 0o600);
  assert.equal(JSON.parse(readFileSync(fp, "utf8")).token, a);
  assert.equal(loadOrCreateToken(fp), a);   // idempotent
});

test("tokensMatch is exact and length-safe", () => {
  assert.equal(tokensMatch("abc", "abc"), true);
  assert.equal(tokensMatch("abc", "abd"), false);
  assert.equal(tokensMatch("abc", "abcd"), false);
  assert.equal(tokensMatch("", "abc"), false);
});
