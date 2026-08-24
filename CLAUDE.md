# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Athena is a personal text-only assistant that runs on **Hermes Agent 0.20.4** — this repo contains **no application code**. It is the portable *overlay* (markdown instructions, skills, MCP declarations, cron templates, bash bootstrap) that gets applied onto a machine-local Hermes install. The agent runtime itself lives at `~/.hermes/hermes-agent` and is upstream software.

Consequence: there is no build, no test suite, no linter, and no package manifest. "Changing Athena's behavior" means editing prose in `HERMES.md` or a `SKILL.md`, or editing JSON/YAML declarations — not writing functions.

## The repo / `~/.hermes` split

This is the single most important thing to understand before editing anything.

| Layer | Location | Rule |
|---|---|---|
| Overlay (versioned) | this checkout | `HERMES.md`, `.hermes/skills/`, `.hermes/plugins/`, `mcp.json`, `cron/*.example.json`, `scripts/` |
| Runtime (machine-local, never in git) | `~/.hermes/` | `.env` secrets, `SOUL.md` personality, `mcp-tokens/`, `google_token.json`, `sessions/`, `memories/`, live `cron/jobs.json` |
| Upstream engine | `~/.hermes/hermes-agent` | **Never fork or patch.** Extend via `.hermes/plugins/` instead, so `hermes update` stays safe. |

`~/.hermes/SOUL.md` (personality) is deliberately *not* loaded from this repo. `HERMES.md` is loaded only when Hermes' cwd is this checkout — `bootstrap.sh` sets `terminal.cwd` so CLI and Telegram sessions both land here.

## Commands

```bash
./scripts/bootstrap.sh            # apply this checkout onto the local Hermes profile (idempotent)
./scripts/bootstrap.sh --cron     # same, plus install/update cron jobs — gateway Mac only
./scripts/sync-cron.sh            # sync live cron jobs from cron/*.example.json
./scripts/sync-cron.sh --dry-run  # print create/update actions without calling hermes
```

Bootstrap sets `terminal.cwd`, trusts project skills, copies the Google Desktop client JSON into `~/.hermes/`, ensures the Linear MCP is declared, and reports which env vars and browser logins are still missing. It never prints secret values and is safe to re-run.

Hermes CLI operations this repo depends on:

```bash
hermes chat                       # start a session (or rely on terminal.cwd)
hermes gateway                    # cron only fires while this runs
hermes mcp login linear           # browser OAuth; then start a NEW session
hermes skills trust <path>
hermes plugins enable <name>
```

There is no automated verification. To validate a change, start a **new** Hermes session with cwd = this repo and exercise it (`SETUP.md` has the smoke tests: vault search, Linear issue list, calendar/mail query).

## Cron model

Job *definitions* are versioned here as `cron/*.example.json`; *live* jobs exist only in `~/.hermes/cron/jobs.json` on **one always-on gateway machine**. Enabling the same job on two laptops double-fires it.

`sync-cron.sh` reconciles the two: it matches a template to a live job **by `name`** (case-insensitive fallback), creates it if absent, and edits it when schedule, prompt, deliver, skills, or workdir drift. `workdir: "repo"` resolves to this checkout so project skills load. Once it succeeds it drops a marker at `~/.hermes/athena.cron-machine`, after which plain `./scripts/bootstrap.sh` on that machine keeps jobs in sync automatically.

Required template fields: `name`, `schedule`, `prompt`. Optional: `deliver` (default `telegram`), `skills`, `workdir`. Delivery targets `TELEGRAM_HOME_CHANNEL`; with no Telegram config, output stays in `~/.hermes/cron/output/`.

Editing a cron prompt is a two-part change: commit the template **and** re-run `sync-cron.sh` on the gateway, or the live job keeps the old prompt.

## Integrations

- **Obsidian** — bundled upstream skill. Vault path comes from `OBSIDIAN_VAULT_PATH` in `~/.hermes/.env`; instructions require resolving it to an absolute path before calling file tools (never pass the `$VAR` through).
- **Linear** — remote MCP server (`https://mcp.linear.app/mcp`, OAuth). Declared in `mcp.json` (source of truth in git); the token lives in `~/.hermes/mcp-tokens/linear.json`. Not a skill — it is MCP tools.
- **Gmail / Calendar** — bundled upstream `google-workspace` skill. Desktop OAuth client JSON sits at `google_client_secret.json` in the repo root (gitignored); bootstrap installs it via the skill's `scripts/setup.py`. The user token (`~/.hermes/google_token.json`) needs one browser pass per machine.
- **`morning-brief`** (`.hermes/skills/morning-brief/`) — the only project-owned skill. Composes Linear MCP + the `google-workspace` `daily-brief` reference. It is explicitly read-only: a brief request never authorizes sending mail, creating events, or mutating Linear.

## Conventions when editing

- **Model policy is fixed:** ChatGPT via `openai-api` (`OPENAI_API_KEY`). Do not add or suggest a fallback provider — `README.md`, `HERMES.md`, and `config.example.yaml` all state this deliberately.
- `config.example.yaml` is a **manual merge target** for `~/.hermes/config.yaml`, not applied by any script and referenced by no code. Merge selected keys; never replace the whole runtime config.
- Skills are `.hermes/skills/<name>/SKILL.md` with YAML frontmatter (`name`, `description`, `version`) and a numbered procedure. `morning-brief` is the pattern to copy: explicit steps, explicit caps ("at most 5 issues"), explicit degradation ("if Linear tools are missing, say so and continue"), and a stated authorization boundary.
- Project plugins require `HERMES_ENABLE_PROJECT_PLUGINS=true` in `~/.hermes/.env`.
- Anything user-facing that changes agent behavior should also be reflected in `HERMES.md` (agent-facing) and, if it changes setup, `SETUP.md` (human-facing). These three docs are kept consistent by hand.
- `.gitignore` guards `.env`, `client_secret*.json`, `google_client_secret.json`, `google_token.json`. Do not relax these.

## Not implemented

`RGB-MATRIX.md` describes a planned ESP32-S3 / HUB75 64×64 matrix display driven over WebSocket. The `athena.py`, `firmware/athena_matrix/`, and `display:` config it references **do not exist in this repo**. Treat it as a design note, not as documentation of current behavior.
