# Claude Code Project Guide for AetherSDR (N8EM Ten-Tec fork)

This is N8EM's fork of AetherSDR. It adds a backend for a Ten-Tec Orion
(or Omni VII) on serial CAT, with an SDRplay RSP2 as the panadapter
receiver. The fork's own rules come first; the upstream guide follows.

## Fork rules (always apply)

1. **Never edit `~/orion3-sdr-console`.** Read it for reference only; no
   commits or builds there. It must keep starting exactly as it is.
2. **Bring over no appearance from the Orion console**: no layouts,
   colours, meter faces, panadapter drawing, dialogs or window styles.
   AetherSDR's UI is the UI. Port drivers and protocol logic only, and
   re-express them in AetherSDR's own patterns.
3. **Never key the transmitter or send CW in unattended tests.**
   Receive-only use of the real SDR is fine. Test CAT against a fake
   radio on a pty (the Orion console's `tests/fake_radio.py` shows how).
4. **The repo is public.** A certain commercial Ten-Tec control-software
   author's callsign must never appear in it (code, comments, docs or
   commits). Before every push, run all three checks; each must print
   nothing:
   `grep -ri "n4""py" . --exclude-dir=build`,
   `git log --all -i -S"n4""py" --oneline`,
   `git log --all -i --grep="n4""py" --oneline`.
   The pattern is split so this file itself stays clean.
5. **Releases must not bundle the proprietary SDRplay library.**
6. **Pushes happen only after the operator's on-air test**, and only when
   he asks: build, test, commit, then wait.
7. The two programs share the hardware but **never run at the same
   time**. Serial ports, the SDRplay service and the network ports are
   single-owner. Each keeps its own settings; neither writes the other's.
8. Keep the fork mergeable with upstream. Put the work in one folder
   (`src/core/backends/orion/`) plus the fewest possible hook-up points,
   and note each one in `FORK-NOTES.md`.

## Fork pointers

- Remotes: `origin` = github.com/n8mus/AetherSDR (the fork),
  `upstream` = github.com/ten9876/AetherSDR.
- Reference program: `~/orion3-sdr-console` (read-only). Its `CLAUDE.md`
  "Station facts" holds the station's devices and quirks.
- Design: `docs/architecture/orion-backend-design.md`.
- Hook-up points into upstream code: `FORK-NOTES.md`.
- Use the udev device names (`/dev/orion`, `/dev/omni7`, `/dev/lpmeter`,
  `/dev/portS4`, WinKeyer FTDI by-id `A904QF5Z`), never raw `ttyS*`.
- Reach the SignaLink ("USB AUDIO CODEC") through PipeWire, never `plughw`.

---

## Upstream guide

> **Claude Code: read [`AGENTS.md`](AGENTS.md) for the full project guide.**
> This file exists only because Claude Code auto-loads `CLAUDE.md`.
> Everything project-wide — architecture, conventions, the constitution,
> commit signing, branch protection rules, the must-knows — lives in
> `AGENTS.md` so every AI tool reads the same source of truth.

## Why this is a thin file

AetherSDR's contribution surface is multi-agent in practice (six+
distinct AI tools touch the codebase). Each tool has its own
well-known file at a different path:

| Tool | File |
|---|---|
| Claude Code | `CLAUDE.md` (you are here) |
| OpenAI Codex CLI / spec-kit / Foundry | `AGENTS.md` (the canonical) |
| GitHub Copilot | `.github/copilot-instructions.md` |
| Gemini Code Assist | `GEMINI.md` |

All of those are pointers to `AGENTS.md`. The duplication-by-pointer
pattern keeps each tool reading from its native discovery path without
forcing a single canonical file to be copied N times.

Read `AGENTS.md` for everything else.
