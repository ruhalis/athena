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
python3.12 -m venv .venv
source .venv/bin/activate
pip install -r ../requirements.txt
cp ../.env-example ../.env   # then set OPENAI_API_KEY
redis-server &
```

## Run

```bash
cd jarvis
python services/jarvis_router.py
python services/jarvis_brain.py --daemon
python services/jarvis_dashboard.py   # http://127.0.0.1:8088
```

One-shot:

```bash
python services/jarvis_brain.py "hello, introduce yourself briefly"
```

Simulate a dashboard message:

```bash
python services/jarvis_router.py --say "good morning"
```

Provider switch (explicit, not a fallback):

```bash
JARVIS_BRAIN_PROVIDER=local python services/jarvis_brain.py --daemon
JARVIS_BRAIN_PROVIDER=anthropic python services/jarvis_brain.py --daemon
```
