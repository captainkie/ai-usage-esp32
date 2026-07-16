import { test } from "node:test";
import assert from "node:assert/strict";
import { dnsSdArgs } from "../lib/mdns.mjs";

test("dnsSdArgs registers the _aiusage._tcp service on the given port", () => {
  assert.deepEqual(dnsSdArgs(8787, "AI Usage Bridge"),
    ["-R", "AI Usage Bridge", "_aiusage._tcp", "local", "8787"]);
});
