# Jarvis TODO (v3)

Authoritative spec: `TECHNICAL.md` (v3.0). Display: `RGB-MATRIX.md`.

This build is **text-only**, ChatGPT-first, **no fallbacks**.

## Done

- [x] Remove unused audio/Jetson paths (CosyVoice, claw-code, `jarvis_voice.py`, `jarvis_tts.py`)
- [x] uv project (`pyproject.toml` + `uv.lock`); `.env` / `.env-example` for `OPENAI_API_KEY`
- [x] Brain: OpenAI tool loop; explicit `local` / `anthropic`; optional Hermes CLI harness
- [x] Router: `user_text` → routines or `llm_request`; `speak` → `assistant_reply`
- [x] MCP tools: `speak`, `ha_control`, `ha_query`, `set_timer`, `get_weather`, `memory_write`
- [x] Safety gate: locks/alarms/garage require typed **yes**
- [x] Timers, dashboard, WiFi display bridge + ESP32 firmware sketch

## Run

```bash
uv sync
redis-server &
uv run python jarvis/services/jarvis_router.py
uv run python jarvis/services/jarvis_brain.py --daemon
uv run python jarvis/services/jarvis_dashboard.py
uv run python jarvis/services/jarvis_timers.py
uv run python jarvis/services/jarvis_display.py   # optional
```

One-shot:

```bash
uv run python jarvis/services/jarvis_brain.py "hello, introduce yourself briefly"
```

## Later (not this build)

- [ ] Wake word (stock openWakeWord, then voice verifier)
- [ ] STT (mlx-whisper / whisper.cpp)
- [ ] TTS (Kokoro) consuming the same `assistant_reply` channel
