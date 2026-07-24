# Pixie Assistant Platform — Roadmap & Architecture

**Date:** 2026-07-24
**Status:** Design approved (brainstorm). Foundation + Phase 1 to be spec'd into an implementation plan when built.
**Branch context:** builds on `feat/voice-firmware` (voice pipeline shipped: on-device `/voice`, whisper **medium** STT, `say` TTS, web search).

---

## Vision

Turn **Pixie** from a voice Q&A toy into a **real AI assistant that does things** — open apps, check email, know your meetings, live in a team chat — while staying true to the giveaway spirit (free, local-first, token-gated, private).

The whole feature list the user wants — "open YouTube on the Mac", "check my mail", "join my meeting", "be in our Discord" — is **one idea**: give Pixie's brain a set of **tools it can call**. Web search (already shipped) is the proof: the model decides to use a tool, the bridge runs it, Pixie speaks the result.

---

## Architecture — one tool-calling core, many surfaces

Evolve the bridge's `askClaude` from a **one-shot** call into an **agentic tool loop**:

```
  speak / type
       │
       ▼
  ┌─────────────────────────── Pixie brain (bridge) ───────────────────────────┐
  │  askAgent(prompt, tools)                                                     │
  │   → Claude picks a tool  → bridge executes it → feeds result back → repeat   │
  │                          → Claude writes the spoken/typed answer            │
  └─────────────────────────────────────────────────────────────────────────────┘
       ▲                                   │
       │ tool call                         │ answer
  ┌────┴─────┐  ┌──────────┐  ┌──────────┐ │
  │ Mac      │  │ email    │  │ calendar │ …  ← tool registry (add capabilities one at a time)
  │ actions  │  │ (mail/   │  │ (mac/    │
  │ (exists) │  │  gmail)  │  │  gcal)   │
  └──────────┘  └──────────┘  └──────────┘

  Surfaces that share this one brain:
    • /voice   (ESP32 screen ④ — tap-to-talk, later wake word)
    • Discord  (bot in a channel — text)
```

**Three load-bearing ideas:**

1. **Capability = tool.** Each capability is a tool (JSON schema) + an executor. Adding a feature = adding a tool, not rewriting the loop.
   - **Server-side tools** run on Anthropic's side (web search — done).
   - **Client-side tools** the bridge executes. **Mac control reuses what already exists:** `bridge/lib/actions.mjs` `validateAction` + the `open`/`osascript`/`shortcuts` executor, and the `actions.json` allowlist (`urls` / `apps` / `shortcuts`). Screen ③ already drives these over `/action`; voice just gives Claude the same actions as tools.

2. **One brain, many surfaces.** The same `askAgent` loop serves `/voice` (ESP32) **and** a Discord bot. A new surface is an input/output adapter, not new logic.

3. **Consent layer.** Tools are tagged by risk:
   - **read-only / low-risk** (check email, next meeting, open an allowlisted app) → run immediately, Pixie narrates.
   - **state-changing / outward-facing** (send an email, join a meeting, run a shortcut) → Pixie **confirms first** ("จะส่งเมลหา… นะ ยืนยันไหม") before executing.
   - Config-driven per tool so the giveaway default is safe.

---

## Roadmap (build order)

Ordered as the user set it. The tool-calling **assistant platform is the final, most ambitious block** — it lands *after* the wake word and the provider switcher.

| # | Item | Status |
|---|------|--------|
| 0a | **Thai STT accuracy** — whisper `base` → **`medium`** | ✅ shipped 2026-07-24 |
| 0b | **Web search** — server-side `web_search` tool in `askClaude` (live prices/weather/news) | ✅ shipped 2026-07-24 |
| 0c | **Mascot redesign** — pixel runner → cute glasses "secretary" Pixie (see below) | 🎨 design approved, sprite TODO |
| 1 | **Wake word "Pixie"** — `wake.h` microWakeWord/TFLite-Micro; keep tap-to-talk as the reliable default | ⏳ next firmware task |
| 2 | **Provider / model switch** — pick the brain per query (Claude / Codex / Gemini / …), AI-settings on screen ④ (`voice-providers.mjs` scaffolding exists) | 📋 planned |
| 3 | **AI CONTROL — the tool-calling assistant platform** *(the last, biggest block)* | 📋 designed, this doc |

### Phase 3 breaks down into capability tools

Built in this order once the platform core lands:

- **3.0 · Agent core** — `askAgent` tool loop + tool registry + consent tagging. Prerequisite for everything below. Reuses `lib/actions.mjs` for the first tools.
- **3.1 · Mac control by voice** — tools `open_app` / `open_url` / `media` / `volume` / `lock` / `shortcut` (all already have executors). *"เปิด YouTube"* → Claude calls `open_url(youtube.com)` → Safari opens → Pixie confirms. Low-risk ones immediate; `shortcut` behind consent.
- **3.2 · Awareness (read-only)** — `check_email` (summarise unread), `next_meeting` / `today_agenda`. **Two backends, user picks:** macOS **Mail/Calendar via AppleScript** (local, no cloud auth, giveaway-friendly) **and** **Google Gmail + Calendar** (richer; OAuth/MCP). Same tool schema, swappable executor.
- **3.3 · Action (needs consent)** — `reply_email` / `send_email`, `join_meeting` (open the Zoom/Meet link, optionally at the right time), `set_reminder`. All gated by the confirm step.
- **3.4 · Discord surface** — a bot (discord.js, hosted by the bridge/Mac) in a team channel → same brain + tools → replies in-channel. **Guardrail: Discord gets an info/chat tool subset ONLY — it must NOT be able to control the Mac from a group chat.** Decide respond-on-mention vs respond-to-all; keep the work channel's privacy in mind.

---

## Email / Calendar — support both backends

The user chose **both**. Design so the tool schema is backend-agnostic and the executor is selectable (config: `PIXIE_MAIL_BACKEND = mac | google`):

| Backend | How | Pros | Cons |
|---|---|---|---|
| **macOS Mail / Calendar** | AppleScript via `osascript` (same machinery as screen ③) | local, no cloud auth, private, giveaway-friendly | tied to the accounts set up on that Mac |
| **Google (Gmail + Calendar)** | Gmail + Google Calendar (OAuth, or the MCP servers already connected in-session) | works with the user's real work accounts; richer | needs per-user auth |

Default to **mac** for a giveaway unit; let a power user switch to **google**.

---

## Mascot redesign (Phase 0c) — cute glasses assistant

Replace the pixel "runner" (Arnold/T-800, the current `mascot.c` default) on **screen ④** with a character that fits the name **Pixie**: a cute glasses-wearing girl assistant.

- **Direction chosen: "A" (locked)** — long dark centre-part hair, **round thin gold-frame glasses**, a **blue hair clip**, camel cardigan over a white blouse, sweet smile. Inspired by a user reference (K-pop-with-glasses vibe); **Pixie stays an original cartoon — capture the vibe, not a real person's likeness.**
- **Four faces** wired to the voice states: `idle` (soft smile) · `listening` (wide attentive eyes) · `thinking` (eyes up + "…") · `speaking` (happy eyes + open mouth).
- **On-device target:** the mascot canvas is **120×120 RGB565** at pos (512,28), drawn by `mascot_render(buf, 120, 120, kind, fur, mood, t, bg565)` in `mascot.c`, and **recolours per provider** via `fur`. Implementation = a pixel sprite (or primitive redraw) matching the approved concept, with `mood` → the four faces.
- Concept mockup: published Artifact "Pixie mascot concepts" (3 directions + the 4 states + an on-device panel). Source: `design/` (mockup HTML in scratchpad during design).

---

## Non-goals / YAGNI (for now)

- No cloud STT/TTS — stays local (`whisper.cpp` + macOS `say`).
- No Discord→Mac control (security).
- No auto-typing the macOS login password (rejected earlier; unsafe on a giveaway).
- Don't pre-write full implementation plans for every phase — each phase gets its own spec → plan → build cycle when it starts. This doc is the roadmap + the Phase-3 architecture.

---

## Decisions locked (this brainstorm)

- ✅ Architecture = tool-calling agent core, capability = tool, one brain across voice + Discord.
- ✅ Consent layer: read-only immediate, state-changing/outward = confirm.
- ✅ Email/Calendar = support **both** macOS (AppleScript) and Google.
- ✅ Discord = info/chat only, never Mac control.
- ✅ Roadmap order: … → wake "Pixie" → provider/model switch → **AI-control assistant platform (last)**.
- ✅ Mascot = direction A (glasses secretary), original character.

## Next step

When Phase 3.0 starts: run `writing-plans` on the **agent core + Phase 3.1 (Mac control by voice)** as the first buildable slice, then build → verify on-device with the user.
