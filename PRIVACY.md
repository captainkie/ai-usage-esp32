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

This mirrors the privacy stance of the original
[AI Usage Bar](https://github.com/captainkie/ai-usage-bar) menu-bar app.
