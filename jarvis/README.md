# jarvis

Text-only personal assistant. ChatGPT by default. No STT/TTS in this build.

```
user_text → router → routine or brain → speak → assistant_reply → dashboard
```

## Layout

```
jarvis/
├── CLAUDE.md
├── config.yaml
├── hermes/config.yaml
├── routines/default.yaml
└── services/
    ├── jarvis_brain.py
    ├── jarvis_router.py
    ├── jarvis_mcp_server.py
    ├── jarvis_dashboard.py
    ├── jarvis_timers.py
    ├── jarvis_display.py
    └── firmware/jarvis_matrix/
```

## Prereqs

```bash
# from repo root
uv sync
cp .env-example .env   # then set OPENAI_API_KEY
redis-server &
```

## Run

```bash
uv run python jarvis/services/jarvis_router.py
uv run python jarvis/services/jarvis_brain.py --daemon
uv run python jarvis/services/jarvis_dashboard.py   # http://127.0.0.1:8088
```

One-shot:

```bash
uv run python jarvis/services/jarvis_brain.py "hello, introduce yourself briefly"
```

Simulate a dashboard message:

```bash
uv run python jarvis/services/jarvis_router.py --say "good morning"
```

Provider switch (explicit, not a fallback):

```bash
JARVIS_BRAIN_PROVIDER=local uv run python jarvis/services/jarvis_brain.py --daemon
JARVIS_BRAIN_PROVIDER=anthropic uv run python jarvis/services/jarvis_brain.py --daemon
```
