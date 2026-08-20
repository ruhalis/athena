# Athena setup (this Mac and the next one)

This repo is the versioned recipe. `~/.hermes/` is the machine-local runtime. Secrets and OAuth tokens never go in git.

On a **new device**, install Hermes first, then clone this repo, then run the bootstrap. On **this device**, the same script is safe to re-run.

```bash
cd /path/to/athena
./scripts/bootstrap.sh
```

The script sets `terminal.cwd` and project-skill trust to this checkout, ensures Linear is declared in Hermes config, and prints what still needs a browser login.

## What lives where

| Thing | Git (this repo) | Each machine |
|---|---|---|
| Instructions | `HERMES.md` | Loaded when Hermes cwd is this repo |
| Skills / plugins | `.hermes/skills/`, `.hermes/plugins/` | After `hermes skills trust` |
| Linear MCP declaration | `mcp.json` | OAuth token in `~/.hermes/mcp-tokens/` |
| Gmail / Calendar client | `google_client_secret.json` (repo root, gitignored) | Bootstrap copies to `~/.hermes/google_client_secret.json` |
| Gmail / Calendar login | never | `~/.hermes/google_token.json` (one browser login per machine) |
| Obsidian vault path | `.env-example` | `OBSIDIAN_VAULT_PATH` in `~/.hermes/.env` |
| API keys / Telegram | `.env-example` | `~/.hermes/.env` |
| Cron *definitions* | `cron/*.example.json` | Live jobs in `~/.hermes/cron/jobs.json` |
| Personality | optional copy of `~/.hermes/SOUL.md` | `~/.hermes/SOUL.md` (not loaded from the repo) |
| Sessions / memory | never | `~/.hermes/sessions/`, `memories/` |

## New machine checklist

1. Install [Hermes Agent](https://hermes-agent.nousresearch.com/) (this project targets **0.20.4**).
2. Clone this repo. Sync the Obsidian vault to a local path (Obsidian Sync, iCloud, or the vault’s own git).
3. Copy keys into `~/.hermes/.env` from 1Password (or fill `.env-example`). Required names:
   - `OPENAI_API_KEY`
   - `OBSIDIAN_VAULT_PATH` (absolute path **on this machine**)
   - `HERMES_ENABLE_PROJECT_PLUGINS=true`
   - Telegram, if you use it: `TELEGRAM_BOT_TOKEN`, `TELEGRAM_ALLOWED_USERS`, `TELEGRAM_HOME_CHANNEL`
4. Put the Google Desktop client JSON in the repo root as `google_client_secret.json` (keep it gitignored).
5. Run `./scripts/bootstrap.sh`.
6. Linear: `hermes mcp login linear`, then start a new Hermes session.
7. Google: if this machine has no `~/.hermes/google_token.json` yet, finish the skill’s `--auth-url` / `--auth-code` flow once.
8. Point cron at **one always-on machine**. Jobs only fire while the Hermes gateway is running (`hermes gateway`). Do not enable the same jobs on two laptops.

Full machine move (same you, new computer): `hermes backup` / `hermes import`, then encrypt the zip. That copies secrets. For a second laptop, use this repo + re-login instead.

## After bootstrap, smoke-test

In a **new** Hermes session (cwd = this repo):

- Obsidian: “search the vault for X”
- Linear: “list my Linear issues”
- Gmail / Calendar: “what’s on my calendar tomorrow?” / “any unread mail I should see?”

## Adding a routine later

1. Copy `cron/morning-brief.example.json` (or write a new example next to it).
2. Create the live job on the scheduler machine only:

   ```bash
   hermes cron create "every 1d at 08:00" "$(cat cron/morning-brief.example.json | python3 -c 'import json,sys; print(json.load(sys.stdin)["prompt"])')" \
     --name morning-brief --deliver telegram --skill google-workspace --skill morning-brief
   ```

   Or ask Hermes in chat: “create this cron job from `cron/morning-brief.example.json`”.
3. Commit the example JSON. Leave `~/.hermes/cron/jobs.json` untracked.

Delivery uses `TELEGRAM_HOME_CHANNEL`. If nothing is configured, output stays local under `~/.hermes/cron/output/`.
