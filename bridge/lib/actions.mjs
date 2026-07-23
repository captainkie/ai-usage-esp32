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

// Parse an inbound USB line from the device. The Remote sends actions as
// `@ACT <token> <json>` so it can drive the Mac over the USB cable with no Wi-Fi.
// Returns { token, body } or null. Pure — the caller checks the token + validates.
export function parseUsbAction(line) {
  if (typeof line !== "string" || !line.startsWith("@ACT ")) return null;
  const rest = line.slice(5).trimStart();
  const sp = rest.indexOf(" ");
  if (sp < 0) return null;
  const token = rest.slice(0, sp);
  if (!token) return null;
  let body;
  try { body = JSON.parse(rest.slice(sp + 1)); } catch { return null; }
  return { token, body };
}

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
