import { test } from "node:test";
import assert from "node:assert/strict";
import { validateAction, parseUsbAction } from "../lib/actions.mjs";

const cfg = { urls: { YouTube: "https://youtube.com" }, apps: { Music: "Music" }, shortcuts: { Focus: "Focus" } };

test("open_url only allows https and returns an execFile spec", () => {
  assert.deepEqual(validateAction({ action: "open_url", url: "https://youtube.com" }, cfg),
    { ok: true, cmd: { file: "open", args: ["-a", "Safari", "https://youtube.com/"] } }); // URL normalizes root path with trailing slash
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
  assert.deepEqual(validateAction({ action: "lock" }, cfg).cmd, { file: "open", args: ["-a", "ScreenSaverEngine"] });
  assert.deepEqual(validateAction({ action: "display_sleep" }, cfg).cmd, { file: "pmset", args: ["displaysleepnow"] });
  assert.equal(validateAction({ action: "nuke" }, cfg).ok, false);
});

/* ---- parseUsbAction: Remote actions carried over the USB cable ---- */
test("parseUsbAction reads a valid @ACT line into {token, body}", () => {
  const p = parseUsbAction('@ACT abc123 {"action":"volume","dir":"up"}');
  assert.deepEqual(p, { token: "abc123", body: { action: "volume", dir: "up" } });
});
test("parseUsbAction rejects non-@ACT lines, missing token, and bad json", () => {
  assert.equal(parseUsbAction('{"action":"volume"}'), null);   // a plain usage/other line
  assert.equal(parseUsbAction("@ACT onlytoken"), null);         // no json part
  assert.equal(parseUsbAction("@ACT tok {not json}"), null);    // bad json
  assert.equal(parseUsbAction("@ACT  {\"action\":\"lock\"}"), null); // empty token
  assert.equal(parseUsbAction(42), null);                       // non-string
});
test("parseUsbAction output feeds validateAction (end-to-end shape)", () => {
  const p = parseUsbAction('@ACT t {"action":"open_url","url":"https://youtube.com"}');
  const v = validateAction(p.body, { urls: {}, apps: {}, shortcuts: {} });
  assert.equal(v.ok, true);
  assert.deepEqual(v.cmd, { file: "open", args: ["-a", "Safari", "https://youtube.com/"] });
});
