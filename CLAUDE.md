# CLAUDE.md

This file orients agents working in this repository.

Authoritative spec: `TECHNICAL.md` (v3.0). Display: `RGB-MATRIX.md`. Personality for the assistant: `jarvis/CLAUDE.md`.

## Project Overview

Jarvis is a **text-only** personal assistant on a 16 GB MacBook. Typed input → command router → Hermes-style brain (ChatGPT by default) → `speak` as text. No STT, TTS, or wake word in this build.

```
CLI / dashboard → router → routines or brain → speak → dashboard
                                      └─► state_change → WiFi ESP32 matrix
```

## Brain

- Default provider: **ChatGPT** via `OPENAI_API_KEY` in `.env`
- Explicit switch only: `JARVIS_BRAIN_PROVIDER=local` (Qwen3.8-27B max quant) or `anthropic`
- **No fallbacks.** A missing key or API error fails the turn.
- `speak` is the only user-visible channel (dashboard/CLI). Internal reasoning stays silent.

## Layout

```
jarvis/
├── CLAUDE.md
├── config.yaml
├── hermes/config.yaml          # optional Hermes CLI MCP wiring
├── routines/default.yaml
└── services/
    ├── jarvis_brain.py
    ├── jarvis_router.py
    ├── jarvis_mcp_server.py
    ├── jarvis_dashboard.py
    ├── jarvis_timers.py
    ├── jarvis_display.py
    ├── jarvis_tools.py
    ├── safety_gate.py
    └── firmware/jarvis_matrix/
```

## Environment

Python 3.12 via [uv](https://docs.astral.sh/uv/) (`uv sync`, `.venv`). Redis for the event bus. `python-dotenv` loads `.env`.

```
OPENAI_API_KEY
ANTHROPIC_API_KEY          # optional, explicit switch
HA_URL / HA_TOKEN
JARVIS_BRAIN_PROVIDER      # openai | local | anthropic
```

## Later (not this build)

Wake word, STT, TTS. Same `speak` / `assistant_reply` channel.
