# Jarvis

Text-only assistant for a 16 GB MacBook. Default brain is **ChatGPT** (`OPENAI_API_KEY` in `.env`). Local Qwen3.8-27B and Anthropic are explicit switches. No automatic fallback. No STT/TTS/wake word in this build.

See `TECHNICAL.md` and `jarvis/README.md`.

## Setup

[uv](https://docs.astral.sh/uv/) manages Python 3.12 and dependencies (macOS, Linux, Windows).

```bash
# once: brew install uv
git clone git@github.com:<your-user>/jarvis.git
cd jarvis
uv sync
cp .env-example .env   # set OPENAI_API_KEY
```

Redis is required:

```bash
brew install redis && brew services start redis
```

Optional Hermes CLI (explicit harness, `JARVIS_BRAIN_HARNESS=hermes`):

```bash
curl -fsSL https://hermes-agent.nousresearch.com/install.sh | bash
# merge jarvis/hermes/config.yaml into ~/.hermes/config.yaml
```

## Run

```bash
uv run python jarvis/services/jarvis_router.py
uv run python jarvis/services/jarvis_brain.py --daemon
uv run python jarvis/services/jarvis_dashboard.py
```
