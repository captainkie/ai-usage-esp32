# Privacy

Short version: **your token never leaves your Mac**, and the only outbound
network call is your Mac asking Anthropic about your own usage.

## What is read (locally, on your Mac)

- Your **Claude Code OAuth token** — from the macOS Keychain
  (`Claude Code-credentials`) or `~/.claude/.credentials.json`.
- The **current model + effort** — from your local Claude Code files
  (`~/.claude/projects/**/*.jsonl`, `~/.claude/settings*.json`), read-only.
- Whether **Codex / Gemini** CLIs are signed in — by checking for their
  credential files (existence only).
- **Live system stats** — cpu, memory, disk, network rate, battery, and the
  names of your top CPU processes — read locally from standard macOS CLIs
  (`top`, `vm_stat`, `df`, `netstat`, `pmset`, `ps`). These are served on the LAN
  alongside your usage so the device's Monitor screen can show them. **Nothing
  new leaves your Mac** — same LAN, same trust boundary as `/usage`.

## What is sent, and where

| From → To | Over | Contains your token? |
|---|---|---|
| Mac bridge → `api.anthropic.com/api/oauth/usage` | HTTPS | **Yes** (your own token, your own usage) |
| ESP32 → Mac bridge (`/usage`) | HTTP (LAN) | **No** — only %, reset times, model name |

## What is **not** done

- No analytics, no accounts, no telemetry.
- The token is never written to disk by the bridge.
- The token is never sent to the ESP32 or to any third party.
- Nothing is uploaded anywhere except your own usage request to Anthropic.
- Remote commands (`POST /action`) are **executed locally on your Mac** and are
  never forwarded anywhere. The only thing that crosses the LAN is the device's
  request asking your Mac to run one of its own allowlisted actions.

This mirrors the privacy stance of the original
[AI Usage Bar](https://github.com/captainkie/ai-usage-bar) menu-bar app.
