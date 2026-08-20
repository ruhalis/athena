# Athena

Text-only assistant for a 16 GB MacBook. Runs on **Hermes Agent 0.20.4**. Default brain is **ChatGPT** (`openai-api`, `OPENAI_API_KEY`). No automatic fallback.

## Integrations

- **Obsidian** — bundled skill; vault at `/Users/ruhalis/obsidian/ruhalis` via `OBSIDIAN_VAULT_PATH` in `~/.hermes/.env`
- **Linear** — catalog MCP (`hermes mcp install linear`). Authenticate once with `hermes mcp login linear`

See `HERMES.md` for agent-facing instructions.

## Connect this repo to Hermes

Keep the official engine at `~/.hermes/hermes-agent` (`hermes update` stays easy). This repo is only our overlay: `HERMES.md`, `.hermes/skills/`, `.hermes/plugins/`.

One-time on this machine:

```bash
cd /Users/ruhalis/projects/athena
hermes config set terminal.cwd /Users/ruhalis/projects/athena
hermes skills trust
# already set in ~/.hermes/.env: HERMES_ENABLE_PROJECT_PLUGINS=true
```

Then start Hermes from here (`hermes chat`) or rely on `terminal.cwd` so Telegram/CLI both use this project.

| Customize | Where | Notes |
|---|---|---|
| Personality | `~/.hermes/SOUL.md` | Not loaded from the repo |
| Instructions | `HERMES.md` | Loaded when cwd is this repo |
| Skills | `.hermes/skills/` | After `hermes skills trust` |
| Tools / hooks | `.hermes/plugins/` | Enable with `hermes plugins enable <name>` |
| Core Hermes | don't fork | Use plugins instead of patching `~/.hermes/hermes-agent` |
