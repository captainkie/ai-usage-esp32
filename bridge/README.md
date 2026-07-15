# AI Usage Bridge

A tiny, **zero-dependency** Node service that runs on your Mac, reads your
**Claude Code** login, asks Anthropic for **your own** usage, and serves it as
plain JSON on your LAN — so the ESP32 display (or anything else) can show it.

> **Why a bridge and not talk to Anthropic from the ESP32 directly?**
> Your OAuth token is short-lived and refreshed by Claude Code in your Mac's
> Keychain. The bridge reads the *current* token each poll, so it never goes
> stale — and your token **never leaves your Mac**. The device only ever sees
> percentages, reset times, and a model name.

## Run it

```sh
node ai-usage-bridge.mjs          # serves on 0.0.0.0:8787
PORT=9000 node ai-usage-bridge.mjs
```

Auto-start at login (macOS):

```sh
./install-macos.sh 8787
```

Then open `http://<your-mac-ip>:8787/usage` — you'll point the ESP32 at that
address during Wi-Fi setup.

## What it exposes — `GET /usage`

```json
{
  "ok": true,
  "updated": "2026-07-15T15:04:59.549Z",
  "providers": {
    "claude": {
      "name": "Claude", "linked": true,
      "model": "Opus 4.8 · 1M", "effort": "xHigh",
      "five_hour": { "util": 18, "resets_at": "2026-07-15T17:50:00Z" },
      "seven_day": { "util": 57, "resets_at": "2026-07-16T17:00:00Z" }
    },
    "codex":  { "name": "Codex",  "linked": true,  "five_hour": null, "seven_day": null },
    "gemini": { "name": "Gemini", "linked": true,  "five_hour": null, "seven_day": null }
  }
}
```

- **Claude** is the live feed (the same `GET /api/oauth/usage` Claude Code's
  `/status` uses). `error: "unauthorized"` means the token expired — open Claude
  Code once to refresh.
- **Codex / Gemini** are *detected* (so the device can list them) but have no
  live usage wired yet — `five_hour`/`seven_day` stay `null`. The firmware shows
  them as selectable with a "no live data" state.

## How it reads your login

- **macOS:** `security find-generic-password -w -s "Claude Code-credentials"` —
  Apple's own tool reads the Keychain item **without a GUI prompt**.
- **Fallback:** `~/.claude/.credentials.json`.

The token is used only for the one HTTPS call to `api.anthropic.com`. No disk
writes, no telemetry, no third parties.

## Requirements

- Node.js ≥ 18 (you already have it — Claude Code runs on Node).
- A signed-in Claude Code on the same Mac.
