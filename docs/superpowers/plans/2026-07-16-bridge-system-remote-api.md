# Bridge: System Monitor + Remote API — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the Mac bridge so it (a) reports live system stats in `GET /usage` and (b) accepts allowlisted, token-authenticated remote commands via `POST /action` — the Mac side of the Monitor and Remote screens.

**Architecture:** Keep the bridge zero-dependency (Node stdlib only). Extract testable, pure parser/validator functions into `bridge/lib/*.mjs`; the parsers turn shell-command output (fixture strings in tests) into plain objects, so the whole thing is TDD-able on the Mac with `node --test` — no ESP32 board required. `ai-usage-bridge.mjs` stays the thin HTTP + orchestration layer.

**Tech Stack:** Node ≥18 ESM, `node:test` + `node:assert/strict`, `node:child_process` (`execFile`), `node:crypto`, `node:fs`. macOS CLIs: `top`, `vm_stat`, `sysctl`, `df`, `netstat`, `pmset`, `ps`, `osascript`, `open`, `dns-sd`.

---

## Scope

This plan is **Phase 1 (bridge only)** from the design spec
(`docs/superpowers/specs/2026-07-16-multi-screen-mac-remote-design.md`). It produces working,
independently testable software (the extended bridge). Firmware, USB/BLE transports, and the
commercial onboarding are **separate plans** (firmware plan is best executed once the board arrives).

## File Structure

- Create `bridge/lib/system.mjs` — pure parsers (`parseCpu`, `parseMem`, `parseDf`, `parseNetstat`, `netRate`, `parseBattery`, `parseTop`) + `readSystem(prevNet)` orchestrator.
- Create `bridge/lib/pairing.mjs` — `loadOrCreateToken(path)`, `tokensMatch(a, b)`.
- Create `bridge/lib/actions.mjs` — `loadActionsConfig(path)`, `validateAction(body, cfg)`.
- Create `bridge/lib/mdns.mjs` — `dnsSdArgs(port, name)` (pure) + `advertise(port, name)`.
- Create `bridge/actions.example.json` — sample user shortcut config.
- Create `bridge/test/system.test.mjs`, `bridge/test/pairing.test.mjs`, `bridge/test/actions.test.mjs`, `bridge/test/mdns.test.mjs`.
- Modify `bridge/package.json` — add `"test": "node --test"`.
- Modify `bridge/ai-usage-bridge.mjs` — add `system` to the payload; add `POST /action`; advertise mDNS on listen.
- Modify `bridge/README.md`, `SECURITY.md`, `PRIVACY.md` — document the new surface.

Design rule: every function that parses text is **pure** (string in → object out) and lives in `lib/`. Anything that shells out or touches the network is a thin wrapper that calls those pure functions.

---

### Task 1: Test harness + lib directory

**Files:**
- Modify: `bridge/package.json:7-9`

- [ ] **Step 1: Add the test script**

Change the `scripts` block in `bridge/package.json` to:

```json
  "scripts": {
    "start": "node ai-usage-bridge.mjs",
    "test": "node --test"
  },
```

- [ ] **Step 2: Create the directories**

Run: `mkdir -p bridge/lib bridge/test`
Expected: no output, directories exist.

- [ ] **Step 3: Verify the runner works (empty pass)**

Create `bridge/test/smoke.test.mjs`:

```js
import { test } from "node:test";
import assert from "node:assert/strict";
test("runner works", () => { assert.equal(1 + 1, 2); });
```

Run: `cd bridge && node --test`
Expected: `# pass 1`.

- [ ] **Step 4: Commit**

```bash
git add bridge/package.json bridge/test/smoke.test.mjs
git commit -m "test: add node --test harness for the bridge"
```

---

### Task 2: System stat parsers (`bridge/lib/system.mjs`)

**Files:**
- Create: `bridge/lib/system.mjs`
- Test: `bridge/test/system.test.mjs`

- [ ] **Step 1: Write failing tests for every parser**

Create `bridge/test/system.test.mjs`:

```js
import { test } from "node:test";
import assert from "node:assert/strict";
import { parseCpu, parseMem, parseDf, parseNetstat, netRate, parseBattery, parseTop } from "../lib/system.mjs";

test("parseCpu takes the last sample and sums user+sys", () => {
  const s = `Processes: 1\nCPU usage: 5.0% user, 5.0% sys, 90.0% idle\nCPU usage: 12.5% user, 7.5% sys, 80.0% idle\n`;
  assert.equal(parseCpu(s), 20);           // last sample, 12.5+7.5
});

test("parseMem computes used% and GB from vm_stat + memsize", () => {
  const vm = [
    "Mach Virtual Memory Statistics: (page size of 16384 bytes)",
    "Pages free:                          100000.",
    "Pages active:                        300000.",
    "Pages wired down:                    150000.",
    "Pages occupied by compressor:         50000.",
  ].join("\n");
  const memBytes = 16 * 1024 ** 3;         // 16 GiB
  const m = parseMem(vm, memBytes);
  // used pages = 300000+150000+50000 = 500000 * 16384 = 8192000000 bytes = 7.63 GiB
  assert.equal(m.total_gb, 16);
  assert.equal(m.used_gb, 7.6);
  assert.equal(m.util, 48);                // round(7.63/16*100)
});

test("parseDf reads the / data line", () => {
  const df = [
    "Filesystem 1024-blocks      Used Available Capacity iused ifree %iused Mounted on",
    "/dev/disk3s5 971350180 380000000 400000000    74%  100 200   1%   /",
  ].join("\n");
  const d = parseDf(df);
  assert.equal(d.util, 74);
  assert.equal(d.mount, "/");
  assert.equal(d.total_gb, 926.3);         // 971350180 KiB / 1024^2
  assert.equal(d.used_gb, 362.4);          // 380000000 KiB / 1024^2
});

test("parseNetstat sums the first en0 row bytes", () => {
  const ns = [
    "Name  Mtu   Network       Address            Ipkts Ierrs     Ibytes    Opkts Oerrs     Obytes  Coll",
    "en0   1500  <Link#4>      a1:b2:c3:d4:e5:f6   1000     0    2000000     900     0     500000     0",
    "en0   1500  192.168.1     192.168.1.20        1000     0    2000000     900     0     500000     0",
  ].join("\n");
  assert.deepEqual(parseNetstat(ns, "en0"), { rx: 2000000, tx: 500000 });
});

test("netRate turns byte deltas into KB/s and clamps counter resets", () => {
  const r = netRate({ rx: 1000000, tx: 200000 }, { rx: 3048576, tx: 404800 }, 2000);
  assert.equal(r.down_kbps, 1000);         // (2048576 B / 2 s) / 1024
  assert.equal(r.up_kbps, 100);            // (204800 B / 2 s) / 1024
  assert.deepEqual(netRate({ rx: 5, tx: 5 }, { rx: 1, tx: 1 }, 1000), { down_kbps: 0, up_kbps: 0 });
});

test("parseBattery reads percent + charging, null when no battery", () => {
  const batt = "Now drawing from 'Battery Power'\n -InternalBattery-0 (id=123)\t82%; discharging; 3:12 remaining present: true";
  assert.deepEqual(parseBattery(batt), { percent: 82, charging: false });
  assert.equal(parseBattery("Now drawing from 'AC Power'"), null);
});

test("parseTop returns the top N by cpu, basename only", () => {
  const ps = " %CPU COMM\n 42.0 /Applications/Google Chrome.app/Contents/MacOS/Google Chrome\n 21.0 node\n 14.0 /usr/bin/Xcode\n 2.0 loginwindow";
  assert.deepEqual(parseTop(ps, 3), [
    { name: "Google Chrome", cpu: 42 },
    { name: "node", cpu: 21 },
    { name: "Xcode", cpu: 14 },
  ]);
});
```

- [ ] **Step 2: Run to verify failure**

Run: `cd bridge && node --test test/system.test.mjs`
Expected: FAIL — `Cannot find module '../lib/system.mjs'`.

- [ ] **Step 3: Implement the parsers**

Create `bridge/lib/system.mjs`:

```js
// Pure parsers for macOS system stats. Each takes command output (a string)
// and returns plain data — so they are unit-testable with fixtures.
import { execFile } from "node:child_process";

const GiB = 1024 ** 3;
const r1 = (x) => Math.round(x * 10) / 10;

export function parseCpu(topOut) {
  const m = [...topOut.matchAll(/CPU usage:\s*([\d.]+)%\s*user,\s*([\d.]+)%\s*sys/g)];
  if (!m.length) return null;
  const last = m[m.length - 1];
  return Math.round(parseFloat(last[1]) + parseFloat(last[2]));
}

export function parseMem(vmStat, memBytes) {
  const page = Number((vmStat.match(/page size of (\d+) bytes/) || [])[1]) || 4096;
  const pages = (label) => Number((vmStat.match(new RegExp(label + ":\\s+(\\d+)\\.")) || [])[1]) || 0;
  const used = (pages("Pages active") + pages("Pages wired down") + pages("Pages occupied by compressor")) * page;
  return { util: Math.round((used / memBytes) * 100), used_gb: r1(used / GiB), total_gb: r1(memBytes / GiB) };
}

export function parseDf(dfOut) {
  const line = dfOut.trim().split("\n").filter(Boolean).at(-1).trim().split(/\s+/);
  const total = Number(line[1]) * 1024, used = Number(line[2]) * 1024;   // KiB -> bytes
  return { util: parseInt(line[4], 10), used_gb: r1(used / GiB), total_gb: r1(total / GiB), mount: line.at(-1) };
}

export function parseNetstat(nsOut, iface = "en0") {
  const row = nsOut.split("\n").find((l) => l.startsWith(iface + " ") || l.startsWith(iface + "\t"));
  if (!row) return { rx: 0, tx: 0 };
  const c = row.trim().split(/\s+/);
  return { rx: Number(c[6]) || 0, tx: Number(c[9]) || 0 };
}

export function netRate(prev, cur, dtMs) {
  const dt = dtMs / 1000;
  const rate = (a, b) => (b >= a && dt > 0 ? Math.round((b - a) / dt / 1024) : 0);
  return { down_kbps: rate(prev.rx, cur.rx), up_kbps: rate(prev.tx, cur.tx) };
}

export function parseBattery(pmset) {
  const pct = pmset.match(/(\d+)%/);
  if (!pct) return null;
  return { percent: Number(pct[1]), charging: /charging|charged|AC Power/i.test(pmset) && !/discharging/i.test(pmset) };
}

export function parseTop(psOut, n = 3) {
  return psOut.split("\n").map((l) => l.trim()).filter(Boolean)
    .map((l) => l.match(/^([\d.]+)\s+(.+)$/)).filter(Boolean)
    .filter((m) => !Number.isNaN(parseFloat(m[1])) && /^[\d.]/.test(m[1]))
    .map((m) => ({ cpu: Math.round(parseFloat(m[1])), name: m[2].split("/").at(-1).replace(/\.app.*$/, "").trim() }))
    .slice(0, n);
}

function run(file, args) {
  return new Promise((res) => execFile(file, args, { timeout: 4000 }, (e, out) => res(e ? "" : out)));
}

// Orchestrator: shells out, returns { system, net } where net is the raw
// counter sample to pass back next time for rate calculation.
export async function readSystem(prevNet = null, dtMs = 15000) {
  const [top, vm, mem, df, ns, batt, ps] = await Promise.all([
    run("top", ["-l", "2", "-n", "0"]), run("vm_stat", []),
    run("sysctl", ["-n", "hw.memsize"]), run("df", ["-k", "/"]),
    run("netstat", ["-ibn"]), run("pmset", ["-g", "batt"]),
    run("ps", ["-Aceo", "pcpu,comm", "-r"]),
  ]);
  const curNet = parseNetstat(ns);
  const memBytes = Number(mem.trim()) || 0;
  return {
    system: {
      cpu: parseCpu(top) != null ? { util: parseCpu(top) } : null,
      mem: memBytes ? parseMem(vm, memBytes) : null,
      disk: parseDf(df),
      net: prevNet ? netRate(prevNet, curNet, dtMs) : { down_kbps: 0, up_kbps: 0 },
      battery: parseBattery(batt),
      top: parseTop(ps, 3),
    },
    net: curNet,
  };
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cd bridge && node --test test/system.test.mjs`
Expected: PASS (all 7 tests).

- [ ] **Step 5: Sanity-check against the real Mac (not a test, just eyeball)**

Run: `cd bridge && node -e "import('./lib/system.mjs').then(m=>m.readSystem()).then(r=>console.log(JSON.stringify(r.system,null,2)))"`
Expected: real cpu/mem/disk/battery/top values for this Mac.

- [ ] **Step 6: Commit**

```bash
git add bridge/lib/system.mjs bridge/test/system.test.mjs
git commit -m "feat(bridge): system stat parsers (cpu/mem/disk/net/battery/top)"
```

---

### Task 3: Pairing token (`bridge/lib/pairing.mjs`)

**Files:**
- Create: `bridge/lib/pairing.mjs`
- Test: `bridge/test/pairing.test.mjs`

- [ ] **Step 1: Write failing tests**

Create `bridge/test/pairing.test.mjs`:

```js
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
```

- [ ] **Step 2: Run to verify failure**

Run: `cd bridge && node --test test/pairing.test.mjs`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement**

Create `bridge/lib/pairing.mjs`:

```js
import { randomBytes, timingSafeEqual } from "node:crypto";
import { existsSync, readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname } from "node:path";

export function loadOrCreateToken(filePath) {
  if (existsSync(filePath)) {
    try { const t = JSON.parse(readFileSync(filePath, "utf8")).token; if (t) return t; } catch { /* regenerate */ }
  }
  const token = randomBytes(16).toString("hex");
  mkdirSync(dirname(filePath), { recursive: true });
  writeFileSync(filePath, JSON.stringify({ token }), { mode: 0o600 });
  return token;
}

export function tokensMatch(a, b) {
  if (typeof a !== "string" || typeof b !== "string") return false;
  const ba = Buffer.from(a), bb = Buffer.from(b);
  if (ba.length !== bb.length) return false;
  return timingSafeEqual(ba, bb);
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cd bridge && node --test test/pairing.test.mjs`
Expected: PASS (2 tests).

- [ ] **Step 5: Commit**

```bash
git add bridge/lib/pairing.mjs bridge/test/pairing.test.mjs
git commit -m "feat(bridge): pairing token load/create + constant-time compare"
```

---

### Task 4: Action allowlist (`bridge/lib/actions.mjs`)

**Files:**
- Create: `bridge/lib/actions.mjs`
- Create: `bridge/actions.example.json`
- Test: `bridge/test/actions.test.mjs`

- [ ] **Step 1: Create the sample config**

Create `bridge/actions.example.json`:

```json
{
  "urls":      { "YouTube": "https://youtube.com" },
  "apps":      { "Music": "Music", "Safari": "Safari" },
  "shortcuts": { "Focus": "Focus" }
}
```

- [ ] **Step 2: Write failing tests**

Create `bridge/test/actions.test.mjs`:

```js
import { test } from "node:test";
import assert from "node:assert/strict";
import { validateAction } from "../lib/actions.mjs";

const cfg = { urls: { YouTube: "https://youtube.com" }, apps: { Music: "Music" }, shortcuts: { Focus: "Focus" } };

test("open_url only allows https and returns an execFile spec", () => {
  assert.deepEqual(validateAction({ action: "open_url", url: "https://youtube.com" }, cfg),
    { ok: true, cmd: { file: "open", args: ["-a", "Safari", "https://youtube.com"] } });
  assert.equal(validateAction({ action: "open_url", url: "http://x.com" }, cfg).ok, false);
  assert.equal(validateAction({ action: "open_url", url: "file:///etc/passwd" }, cfg).ok, false);
  assert.equal(validateAction({ action: "open_url", url: "not a url" }, cfg).ok, false);
});

test("open_app / shortcut must be in the config", () => {
  assert.deepEqual(validateAction({ action: "open_app", name: "Music" }, cfg).cmd,
    { file: "open", args: ["-a", "Music"] });
  assert.equal(validateAction({ action: "open_app", name: "Terminal" }, cfg).ok, false);
  assert.deepEqual(validateAction({ action: "shortcut", name: "Focus" }, cfg).cmd,
    { file: "shortcuts", args: ["run", "Focus"] });
});

test("media / volume are enumerated", () => {
  assert.equal(validateAction({ action: "media", key: "playpause" }, cfg).ok, true);
  assert.equal(validateAction({ action: "media", key: "eject" }, cfg).ok, false);
  assert.equal(validateAction({ action: "volume", dir: "up" }, cfg).ok, true);
  assert.equal(validateAction({ action: "volume", dir: "sideways" }, cfg).ok, false);
});

test("lock / display_sleep need no params; unknown action rejected", () => {
  assert.equal(validateAction({ action: "lock" }, cfg).cmd.file, "osascript");
  assert.deepEqual(validateAction({ action: "display_sleep" }, cfg).cmd, { file: "pmset", args: ["displaysleepnow"] });
  assert.equal(validateAction({ action: "nuke" }, cfg).ok, false);
});
```

- [ ] **Step 3: Run to verify failure**

Run: `cd bridge && node --test test/actions.test.mjs`
Expected: FAIL — module not found.

- [ ] **Step 4: Implement**

Create `bridge/lib/actions.mjs`:

```js
import { existsSync, readFileSync } from "node:fs";

export function loadActionsConfig(path) {
  const empty = { urls: {}, apps: {}, shortcuts: {} };
  if (!existsSync(path)) return empty;
  try { return { ...empty, ...JSON.parse(readFileSync(path, "utf8")) }; } catch { return empty; }
}

const osa = (script) => ({ file: "osascript", args: ["-e", script] });
const VOL = {
  up:   osa("set volume output volume (output volume of (get volume settings) + 10)"),
  down: osa("set volume output volume (output volume of (get volume settings) - 10)"),
  mute: osa("set volume output muted (not (output muted of (get volume settings)))"),
};
const MEDIA = {
  playpause: osa('tell application "Music" to playpause'),
  next:      osa('tell application "Music" to next track'),
  prev:      osa('tell application "Music" to previous track'),
};

// Returns { ok:true, cmd:{file,args} } or { ok:false, error }.
export function validateAction(body, cfg) {
  const a = body && body.action;
  switch (a) {
    case "open_url": {
      let u; try { u = new URL(String(body.url)); } catch { return { ok: false, error: "bad url" }; }
      if (u.protocol !== "https:") return { ok: false, error: "https only" };
      return { ok: true, cmd: { file: "open", args: ["-a", "Safari", u.href] } };
    }
    case "open_app":
      if (!cfg.apps[body.name]) return { ok: false, error: "app not allowlisted" };
      return { ok: true, cmd: { file: "open", args: ["-a", cfg.apps[body.name]] } };
    case "shortcut":
      if (!cfg.shortcuts[body.name]) return { ok: false, error: "shortcut not allowlisted" };
      return { ok: true, cmd: { file: "shortcuts", args: ["run", cfg.shortcuts[body.name]] } };
    case "media":
      return MEDIA[body.key] ? { ok: true, cmd: MEDIA[body.key] } : { ok: false, error: "bad media key" };
    case "volume":
      return VOL[body.dir] ? { ok: true, cmd: VOL[body.dir] } : { ok: false, error: "bad volume dir" };
    case "lock":
      return { ok: true, cmd: osa('tell application "System Events" to keystroke "q" using {control down, command down}') };
    case "display_sleep":
      return { ok: true, cmd: { file: "pmset", args: ["displaysleepnow"] } };
    default:
      return { ok: false, error: "unknown action" };
  }
}
```

- [ ] **Step 5: Run to verify pass**

Run: `cd bridge && node --test test/actions.test.mjs`
Expected: PASS (4 tests).

- [ ] **Step 6: Commit**

```bash
git add bridge/lib/actions.mjs bridge/actions.example.json bridge/test/actions.test.mjs
git commit -m "feat(bridge): allowlisted action validation + sample actions.json"
```

---

### Task 5: mDNS advertise (`bridge/lib/mdns.mjs`)

**Files:**
- Create: `bridge/lib/mdns.mjs`
- Test: `bridge/test/mdns.test.mjs`

- [ ] **Step 1: Write failing test (pure arg builder)**

Create `bridge/test/mdns.test.mjs`:

```js
import { test } from "node:test";
import assert from "node:assert/strict";
import { dnsSdArgs } from "../lib/mdns.mjs";

test("dnsSdArgs registers the _aiusage._tcp service on the given port", () => {
  assert.deepEqual(dnsSdArgs(8787, "AI Usage Bridge"),
    ["-R", "AI Usage Bridge", "_aiusage._tcp", "local", "8787"]);
});
```

- [ ] **Step 2: Run to verify failure**

Run: `cd bridge && node --test test/mdns.test.mjs`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement**

Create `bridge/lib/mdns.mjs`:

```js
import { spawn } from "node:child_process";

export function dnsSdArgs(port, name = "AI Usage Bridge") {
  return ["-R", name, "_aiusage._tcp", "local", String(port)];
}

// Best-effort Bonjour advertisement via Apple's dns-sd. Returns the child so
// the caller can keep it alive; failures are non-fatal (Wi-Fi still works with
// a typed IP). The process must stay running for the record to persist.
export function advertise(port, name = "AI Usage Bridge") {
  try {
    const child = spawn("dns-sd", dnsSdArgs(port, name), { stdio: "ignore" });
    child.on("error", () => {});
    return child;
  } catch { return null; }
}
```

- [ ] **Step 4: Run to verify pass**

Run: `cd bridge && node --test test/mdns.test.mjs`
Expected: PASS (1 test).

- [ ] **Step 5: Commit**

```bash
git add bridge/lib/mdns.mjs bridge/test/mdns.test.mjs
git commit -m "feat(bridge): mDNS/_aiusage._tcp advertisement helper"
```

---

### Task 6: Wire everything into the HTTP server

**Files:**
- Modify: `bridge/ai-usage-bridge.mjs`

- [ ] **Step 1: Add imports + config near the top**

After the existing `import path from "node:path";` line, add:

```js
import { readSystem } from "./lib/system.mjs";
import { loadOrCreateToken, tokensMatch } from "./lib/pairing.mjs";
import { loadActionsConfig, validateAction } from "./lib/actions.mjs";
import { advertise } from "./lib/mdns.mjs";
import { execFile } from "node:child_process";

const REMOTE_ENABLED = process.env.REMOTE !== "0";           // remote on unless REMOTE=0
const PAIR_PATH = path.join(HOME, ".config", "ai-usage-bridge", "pairing.json");
const ACTIONS_PATH = process.env.ACTIONS || path.join(HOME, ".config", "ai-usage-bridge", "actions.json");
const PAIR_TOKEN = REMOTE_ENABLED ? loadOrCreateToken(PAIR_PATH) : null;
let prevNet = null;
```

- [ ] **Step 2: Add `system` to the payload**

In `buildPayload()`, change the final `return` so it also reports system stats. Replace:

```js
  return { ok: true, updated: new Date().toISOString(), providers };
}
```

with:

```js
  let system = null;
  try { const s = await readSystem(prevNet); system = s.system; prevNet = s.net; } catch { /* keep null */ }
  return { ok: true, updated: new Date().toISOString(), providers, system };
}
```

- [ ] **Step 3: Add the `POST /action` route + JSON body helper**

Immediately before `if (url === "/" || url === "/health") {` inside the request handler, add:

```js
  if (url === "/action") {
    if (!REMOTE_ENABLED) return send(res, 403, { ok: false, error: "remote disabled" });
    if (req.method !== "POST") return send(res, 405, { ok: false, error: "POST only" });
    let raw = ""; req.on("data", (d) => (raw += d));
    req.on("end", () => {
      let body; try { body = JSON.parse(raw || "{}"); } catch { return send(res, 400, { ok: false, error: "bad json" }); }
      if (!tokensMatch(body.token || "", PAIR_TOKEN)) return send(res, 401, { ok: false, error: "unauthorized" });
      const v = validateAction(body, loadActionsConfig(ACTIONS_PATH));
      if (!v.ok) return send(res, 400, v);
      execFile(v.cmd.file, v.cmd.args, { timeout: 5000 }, (e) =>
        e ? send(res, 500, { ok: false, error: e.message }) : send(res, 200, { ok: true }));
    });
    return;
  }
```

- [ ] **Step 4: Advertise mDNS + print the token on listen**

In the `server.listen(PORT, HOST, () => { … })` callback, after the existing `console.log(...)` lines, add:

```js
    if (REMOTE_ENABLED) {
      advertise(PORT);
      console.log(`  remote: enabled — pairing token ${PAIR_TOKEN} (enter this on the device)`);
      console.log(`  shortcuts config: ${ACTIONS_PATH} (copy actions.example.json to start)`);
    } else {
      console.log("  remote: disabled (REMOTE=0) — dashboard only");
    }
```

- [ ] **Step 5: Manual end-to-end verification (real server)**

Run in one terminal: `cd bridge && node ai-usage-bridge.mjs`
Note the printed `pairing token`.

In another terminal:

```bash
# usage now carries a system block:
curl -s localhost:8787/usage | node -e "let s='';process.stdin.on('data',d=>s+=d).on('end',()=>console.log(JSON.parse(s).system))"
# unauthorized action is rejected:
curl -s -XPOST localhost:8787/action -d '{"action":"display_sleep"}' ; echo
# bad url rejected even with token:
curl -s -XPOST localhost:8787/action -d '{"token":"<TOKEN>","action":"open_url","url":"http://x"}' ; echo
# valid action (opens YouTube in Safari):
curl -s -XPOST localhost:8787/action -d '{"token":"<TOKEN>","action":"open_url","url":"https://youtube.com"}' ; echo
```

Expected: `system` object printed with real values; `{"ok":false,"error":"unauthorized"}`; `{"ok":false,"error":"https only"}`; `{"ok":true}` and Safari opens YouTube.

- [ ] **Step 6: Full test suite green**

Run: `cd bridge && node --test`
Expected: all tests PASS.

- [ ] **Step 7: Commit**

```bash
git add bridge/ai-usage-bridge.mjs
git commit -m "feat(bridge): serve system stats in /usage + POST /action remote (token + allowlist + mDNS)"
```

---

### Task 7: Documentation

**Files:**
- Modify: `bridge/README.md`
- Modify: `SECURITY.md`
- Modify: `PRIVACY.md`

- [ ] **Step 1: Document the new endpoints in `bridge/README.md`**

Add a section describing: the `system` block in `/usage`; `POST /action` with the token + allowlist table; `actions.json` (copy from `actions.example.json`); `REMOTE=0` to disable; the printed pairing token; mDNS `_aiusage._tcp`.

- [ ] **Step 2: Update `SECURITY.md`**

Add: the command surface is allowlisted and token-gated; `open_url` is https-only; app/shortcut names are config-restricted; remote is disabled with `REMOTE=0`; USB transport (future) removes LAN exposure; the login password is never handled.

- [ ] **Step 3: Update `PRIVACY.md`**

Add: system stats (cpu/mem/disk/net/battery/top process names) are read locally and served only on the LAN alongside usage; nothing new leaves the Mac; commands are executed locally, never forwarded.

- [ ] **Step 4: Commit**

```bash
git add bridge/README.md SECURITY.md PRIVACY.md
git commit -m "docs(bridge): document system stats + remote action API and its safeguards"
```

---

## Self-Review

**Spec coverage:** §7.1 system block → Task 2 + Task 6.2. §7.2 `/action` allowlist → Task 4 + Task 6.3. §7.3 pairing token → Task 3 + Task 6. §7.4 mDNS → Task 5 + Task 6.4. §9 security (https-only, allowlist, no password, disable flag) → Task 4 + Task 6 + Task 7. Firmware (§4–6, §11), transports (§8), commercial (§10) are explicitly **out of scope** for this plan (separate plans). Covered.

**Placeholder scan:** every code step contains complete, runnable code; Task 7 doc steps describe concrete content to add (the source of truth is the code + spec). No TBD/TODO.

**Type consistency:** `validateAction` returns `{ ok, cmd:{file,args} }` / `{ ok, error }` — consumed exactly that way in Task 6.3. `readSystem` returns `{ system, net }` — used as such in Task 6.2. `loadOrCreateToken`/`tokensMatch` signatures match Task 6 usage. `parseNetstat`/`netRate` shapes (`{rx,tx}`, `{down_kbps,up_kbps}`) are consistent across Task 2 and the orchestrator.

## Follow-on plans (not this plan)

1. **Firmware: multi-screen + monitor/remote** (needs the board) — tileview refactor, 3 screens, `net_action()`, pairing-token entry.
2. **USB transport** — serial framing on both sides (plug-and-play, strongest security).
3. **Commercial onboarding** — signed menu-bar app, QR, pre-flash.
4. **BLE transport** (optional, last).
