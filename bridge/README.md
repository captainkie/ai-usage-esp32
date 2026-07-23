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
./install-macos.sh 8787            # scaffolds actions.json + prints your pairing token
REMOTE=0 ./install-macos.sh 8787   # dashboard only (no remote command endpoint)
```

Then open `http://<your-mac-ip>:8787/usage` — you'll point the ESP32 at that
address during Wi-Fi setup. With the remote enabled (default), the installer
also creates `~/.config/ai-usage-bridge/actions.json` from the sample and prints
the **pairing token** to enter on the device.

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

## Live system stats — the `system` block

`GET /usage` also carries a `system` block, read locally from macOS CLIs
(`top`, `vm_stat`, `sysctl`, `df`, `netstat`, `pmset`, `ps`) — this powers the
**Monitor** screen on the device:

```json
"system": {
  "cpu":     { "util": 26 },
  "mem":     { "util": 48, "used_gb": 7.6, "total_gb": 16 },
  "disk":    { "util": 74, "used_gb": 362.4, "total_gb": 926.4, "mount": "/" },
  "net":     { "down_kbps": 1000, "up_kbps": 100 },
  "battery": { "percent": 82, "charging": false },
  "top":     [ { "name": "Google Chrome", "cpu": 42 }, { "name": "node", "cpu": 21 } ]
}
```

Everything here is read-only and stays on your LAN — nothing new leaves the Mac.

## Remote control — `POST /action`

The **Remote** screen sends allowlisted, token-authenticated commands the Mac
runs locally. Send JSON to `POST /usage`'s sibling, `POST /action`:

```sh
curl -XPOST http://<mac>:8787/action \
  -d '{"token":"<pairing-token>","action":"open_app","name":"Music"}'
```

- **Pairing token** — printed once on startup (also stored `0600` at
  `~/.config/ai-usage-bridge/pairing.json`). Enter it on the device to pair.
  Every `/action` request must carry the matching `token` or it is rejected
  `401 unauthorized` (constant-time compare).
- **Allowlist** — only these actions are accepted; anything else is `400`:

  | `action`        | params            | runs                                    |
  |-----------------|-------------------|-----------------------------------------|
  | `open_url`      | `url` (**https**) | opens the URL in Safari (http/file → `400 https only`) |
  | `open_app`      | `name`            | `open -a <name>` — **name must be in `actions.json` → `apps`** |
  | `shortcut`      | `name`            | `shortcuts run <name>` — **name must be in `actions.json` → `shortcuts`** |
  | `media`         | `key`             | `playpause` / `next` / `prev` (Music)   |
  | `volume`        | `dir`             | `up` / `down` / `mute`                  |
  | `lock`          | –                 | locks the screen                        |
  | `display_sleep` | –                 | sleeps the display                      |

- **`actions.json`** — your allowlist of apps / shortcut names / URL labels.
  `./install-macos.sh` creates it for you (from `actions.example.json`); to do it
  by hand: `cp actions.example.json ~/.config/ai-usage-bridge/actions.json`
  (override the path with `ACTIONS=/path/to/actions.json`). App and shortcut
  names not in this file are refused, so the remote can never launch arbitrary
  software.
- **Disable remote entirely** — start with `REMOTE=0`; `/action` then returns
  `403` and no pairing token is created. Dashboard/Monitor still work.
- **Discovery** — on startup the bridge advertises `_aiusage._tcp` over mDNS
  (Bonjour) so the device can find it without typing an IP. Best-effort; a typed
  IP always works too.

## How it reads your login

- **macOS:** `security find-generic-password -w -s "Claude Code-credentials"` —
  Apple's own tool reads the Keychain item **without a GUI prompt**.
- **Fallback:** `~/.claude/.credentials.json`.

The token is used only for the one HTTPS call to `api.anthropic.com`. No disk
writes, no telemetry, no third parties.

## Requirements

- Node.js ≥ 18 (you already have it — Claude Code runs on Node).
- A signed-in Claude Code on the same Mac.

## Voice (Pixie) — `POST /voice`

Pixie's voice assistant. The device records a spoken question and POSTs the WAV
here; the bridge transcribes it **locally**, asks Claude, speaks the answer, and
returns the audio. **All speech stays on your Mac** — free + local (whisper.cpp +
macOS `say`).

**One-time setup:**
```sh
brew install whisper-cpp
mkdir -p ~/.config/ai-usage-bridge/models
curl -L -o ~/.config/ai-usage-bridge/models/ggml-base.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin
```
`say` (TTS) is built into macOS; replies use a female voice — **Kanya** for Thai,
**Samantha** otherwise.

**Endpoint:** `POST /voice` — header `X-Pixie-Token: <pairing token>`, body = a
16 kHz mono WAV. On success returns `200 audio/wav` with URL-encoded `X-Transcript`
+ `X-Reply` headers; errors are `401` (bad token), `422` (no speech), `500` (pipeline).

> **Rate limits:** the voice brain calls Claude with the same OAuth token as the
> dashboard, so it shares your account's rate limit — a busy account can answer
> `claude 429`. Multi-provider fallback (GPT / Gemini / …) is on the roadmap to
> sidestep this. Override the model with `PIXIE_MODEL` (default `claude-sonnet-5`).
