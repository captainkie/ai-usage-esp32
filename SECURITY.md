# Security

`ai-usage-esp32` is a self-hosted desk display. It has two parts with very
different trust profiles: the **bridge** (runs on your Mac, touches your token)
and the **firmware** (runs on the ESP32, only ever sees percentages).

## Threat model in one paragraph

The bridge reads your **Claude Code OAuth token** locally and uses it for
exactly one request — `GET https://api.anthropic.com/api/oauth/usage` over
TLS — the same call Claude Code's own `/status` makes. The token is **never
written to disk by the bridge, never sent to the ESP32, and never sent to any
third party.** The device receives only utilisation percentages, reset times,
and a model name, as JSON, over your LAN.

## What each part can access

**Bridge (`bridge/ai-usage-bridge.mjs`)**
- Reads the token: macOS Keychain via Apple's `security` tool
  (`Claude Code-credentials`), or `~/.claude/.credentials.json` as a fallback.
- Reads (read-only) `~/.claude/projects/**/*.jsonl` and `~/.claude/settings*.json`
  to show the current model + effort.
- Makes one outbound HTTPS call to `api.anthropic.com` with your token.
- Serves `GET /usage` (JSON) on your LAN. **This endpoint is unauthenticated**
  but contains **no token** — only percentages, reset times, a model name, and
  read-only system stats (cpu/mem/disk/net/battery + top process names).
- Serves `POST /action` (remote control) — see the command surface below.

## Remote command surface (`POST /action`)

The Remote screen can ask the Mac to run a **small, fixed set** of local
actions. It is locked down on every axis:

- **Token-gated.** Every request must carry the pairing token (a 128-bit random
  value, stored `0600` at `~/.config/ai-usage-bridge/pairing.json`, compared in
  constant time). No token → `401`, and the action is never evaluated or run.
- **Allowlisted, not arbitrary.** Only `open_url`, `open_app`, `shortcut`,
  `media`, `volume`, `lock`, `display_sleep` are accepted; anything else → `400`.
  There is **no** "run this shell command" action.
- **`open_url` is https-only.** `http://` and `file://` are rejected `400`.
- **App / shortcut names are config-restricted.** `open_app` and `shortcut` only
  run names present in *your* `actions.json`; unknown names → `400`. The remote
  cannot launch software you did not list.
- **Kill switch.** Start with `REMOTE=0` to disable `/action` entirely (`403`)
  and skip token creation — dashboard/Monitor still work.
- **No password, ever.** The bridge never handles your login/keychain password;
  `lock`/`display_sleep` use macOS' own mechanisms.
- **Future USB transport** (separate plan) removes LAN exposure altogether by
  carrying the same commands over a wired serial link instead of HTTP.

**Firmware (`firmware/`)**
- Connects to your Wi-Fi and polls `http://<mac>:<port>/usage`.
- Stores your Wi-Fi credentials + the bridge IP in the ESP32's NVS (as any
  Wi-Fi device does). No token is ever stored on the device.

## Guidance

- Run the bridge on a **trusted LAN**. It binds `0.0.0.0` so the device can
  reach it; anyone on the same network can read your usage percentages (not your
  token). If that matters to you, put it on a segment you control.
- The device talks to the bridge over plain **HTTP on the LAN** (no token in
  transit). The only TLS-protected, token-bearing call is Mac → Anthropic.
- Keep Claude Code signed in; the bridge relies on it refreshing the token.

## Reporting a vulnerability

Please **do not** open a public issue for security problems. Instead, open a
private **GitHub Security Advisory** on this repository
(`Security ▸ Report a vulnerability`). Include repro steps and impact; you'll get
a response as soon as possible.
