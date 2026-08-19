# Jarvis LLM Layer — Technical Specification

**Version:** 3.0
**Date:** August 2026
**Platform:** Apple Silicon MacBook (16 GB unified memory)
**Status:** Text-only
**Brain:** Hermes-style tool loop. Default provider is ChatGPT (`OPENAI_API_KEY`). Local Qwen3.8-27B (max quant) and Anthropic are explicit switches only. **No fallbacks.**

---

## 1. Goal

Jarvis is a personal assistant that takes typed text, reasons, runs tools (Home Assistant, timers, weather, memory), and replies in text via the `speak` tool. Audio (wake word, STT, TTS) is a later phase.

**Success criteria:**
- Typed prompt → `speak` reply on the dashboard/CLI
- Correct tool execution for common smart-home commands
- Replies 1–3 sentences
- Persistent memory in `CLAUDE.md`
- Selected provider errors fail closed — never silently switch models

---

## 2. Architecture

```
CLI / dashboard  →  user_text  →  router (YAML routines)
                                      │ match        │ miss
                                      ▼              ▼
                                 routine engine    llm_request
                                      │              │
                                      └──────┬───────┘
                                             ▼
                                    speak → assistant_reply
                                             │
                                             ├─► dashboard / CLI
                                             └─► state_change → jarvis_display.py
                                                               → WiFi WebSocket
                                                               → ESP32-S3 → HUB75
```

Default brain path: OpenAI Chat Completions (`gpt-4.1`) with Jarvis tools. Optional `JARVIS_BRAIN_HARNESS=hermes` runs the Nous Hermes CLI instead.

### No fallbacks

- Missing `OPENAI_API_KEY` while provider is `openai` → refuse to start
- API 4xx/5xx → the turn fails; do not try Anthropic or local
- No Haiku→Sonnet escalation
- No USB serial if the matrix WebSocket is down

Switch providers only with `JARVIS_BRAIN_PROVIDER=openai|local|anthropic`.

### Local Qwen3.8-27B (explicit)

Only when `JARVIS_BRAIN_PROVIDER=local`. Serve a **3-bit MLX** (or IQ2 GGUF) build at `http://127.0.0.1:1234/v1`. No vision projector. Context 4K. `sudo sysctl iogpu.wired_limit_mb=14336`. If it does not load, local mode errors.

---

## 3. Components

| Service | Role |
|---|---|
| `jarvis_brain.py` | Tool loop / Hermes CLI. Subscribes to `llm_request`. |
| `jarvis_router.py` | Fuzzy YAML routines. Subscribes to `user_text`. |
| `jarvis_mcp_server.py` | MCP stdio tools for Hermes. |
| `jarvis_mcp_clients.py` | Optional Obsidian / Linear MCP for the default brain. |
| `jarvis_dashboard.py` | FastAPI chat UI. |
| `jarvis_timers.py` | Fires `speak` on timer expiry. |
| `jarvis_display.py` | Redis `state_change` → WiFi WebSocket. |
| `safety_gate.py` | Locks/alarms/garage need a typed **yes**. |

Redis channels: `user_text`, `llm_request`, `assistant_reply`, `state_change`, `timer_set`, `brain_state`, `brain_done`.

State machine: `IDLE` ↔ `PROCESSING`.

---

## 4. Environment

Python 3.12 and deps: `uv sync` (see `pyproject.toml`). Keys live in repo-root `.env` (gitignored). `.env-example` is placeholders only.

```
OPENAI_API_KEY           # required for default provider
ANTHROPIC_API_KEY        # only if JARVIS_BRAIN_PROVIDER=anthropic
HA_URL / HA_TOKEN        # Home Assistant
REDIS_URL                # default redis://127.0.0.1:6379/0
JARVIS_BRAIN_PROVIDER    # openai | local | anthropic
JARVIS_BRAIN_MODEL       # optional override
JARVIS_BRAIN_HARNESS     # openai (default) | hermes
OBSIDIAN_API_KEY         # optional; Obsidian Local REST API
LINEAR_API_KEY           # optional; Linear personal API key
```

---

## 5. Display

Waveshare P3 64×64 driven by ESP32-S3 over Wi-Fi. JSON frames: `{"mode":"think"}`, `{"mode":"clock","t":"14:32"}`. No USB path. Firmware: `jarvis/services/firmware/jarvis_matrix/`.
