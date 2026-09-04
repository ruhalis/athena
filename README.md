# Athena

Text-only assistant for a 16 GB MacBook. Runs on **Hermes Agent 0.20.4**. Default brain is **ChatGPT** (`openai-api`, `OPENAI_API_KEY`). No automatic fallback.

This repo is the portable overlay. Secrets and OAuth tokens stay in `~/.hermes/` on each machine. New device: [SETUP.md](SETUP.md).

## Integrations

- **Obsidian** — bundled skill; vault path via `OBSIDIAN_VAULT_PATH` in `~/.hermes/.env`
- **Linear** — catalog MCP declared in `mcp.json`. Authenticate once with `hermes mcp login linear`
- **Gmail / Google Calendar** — bundled `google-workspace` skill; Desktop client JSON at `google_client_secret.json` in the repo root (gitignored). Bootstrap installs it into `~/.hermes/`.
- **LED face** — a 64×64 HUB75 matrix on an ESP32 mirrors what the agent is doing (idle, listen, think, work, speak, alert, error, sleep). The `athena-face` project plugin maps Hermes hooks to those states and sends them over USB serial through `scripts/face.py`; design and protocol in `RGB-MATRIX.md`, firmware in `firmware/athena_matrix/`.
- **ESP32 boards** — face (`RGB-MATRIX.md`, running on a WROOM-32 prototype) and audio (`AUDIO-BOARD.md`, planned), built with pure ESP-IDF through the global Claude Code `esp-idf` skill (`~/.claude/skills/esp-idf/`, not in this repo; Hermes does not load it). Athena-specific conventions are in `CLAUDE.md`.

See `HERMES.md` for agent-facing instructions.

## Connect this repo to Hermes

Keep the official engine at `~/.hermes/hermes-agent` (`hermes update` stays easy). This repo is only our overlay: `HERMES.md`, `.hermes/skills/`, `.hermes/plugins/`, `mcp.json`, `cron/`.

```bash
cd /path/to/athena
./scripts/bootstrap.sh
```

That sets `terminal.cwd` and skill trust to this checkout, ensures Linear is declared, and reports missing env/logins. On the **gateway** Mac, add `--cron` to install scheduled jobs from `cron/*.example.json`. Then start Hermes from here (`hermes chat`) or rely on `terminal.cwd` so Telegram/CLI both use this project.

| Customize | Where | Notes |
|---|---|---|
| Personality | `~/.hermes/SOUL.md` | Not loaded from the repo |
| Instructions | `HERMES.md` | Loaded when cwd is this repo |
| Skills | `.hermes/skills/` | After `hermes skills trust` |
| Tools / hooks | `.hermes/plugins/` | Enable with `hermes plugins enable <name>`; bootstrap enables `athena-face` |
| MCP servers | `mcp.json` | Login tokens stay in `~/.hermes/mcp-tokens/` |
| Cron templates | `cron/*.example.json` | Live jobs via `./scripts/sync-cron.sh` on one gateway machine |
| Core Hermes | don't fork | Use plugins instead of patching `~/.hermes/hermes-agent` |
| Firmware | `firmware/` + "Firmware conventions" in `CLAUDE.md` | Workflow is the global Claude Code `esp-idf` skill (`~/.claude/skills/esp-idf/`); never Arduino or PlatformIO |
