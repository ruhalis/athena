# Athena setup (this Mac and the next one)

This repo is the versioned recipe. `~/.hermes/` is the machine-local runtime. Secrets and OAuth tokens never go in git.

On a **new device**, install Hermes first, then clone this repo, then run the bootstrap. On **this device**, the same script is safe to re-run.

```bash
cd /path/to/athena
./scripts/bootstrap.sh
```

The script sets `terminal.cwd` and project-skill trust to this checkout, ensures Linear is declared in Hermes config, and prints what still needs a browser login. On the **one always-on gateway Mac**, pass `--cron` so routines in `cron/*.example.json` are created or updated.

If a gateway is already running with an older `terminal.cwd`, bootstrap restarts it. A gateway reads that value once, at startup, and `terminal.cwd` is what makes this repo's project skills resolvable — so a stale gateway makes cron runs silently skip them (`⚠️ Skill(s) not found and skipped: morning-brief`). If you ever change `terminal.cwd` by hand, run `hermes gateway restart` yourself.

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
| LED face (optional) | `.hermes/plugins/athena-face/`, `scripts/face.py`, `firmware/athena_matrix/` | bootstrap enables it and symlinks it into `~/.hermes/plugins/` so the launchd gateway loads it; board port under `## Boards` in `CLAUDE.md`; Wi-Fi name and password for the firmware build in `firmware/components/athena_common/include/athena_secrets.h` (gitignored, copy the `.example`); optional `ATHENA_MATRIX_HOST`, `ATHENA_MATRIX_PORT`, `ATHENA_FACE_BRIGHTNESS`, `ATHENA_FACE_SLEEP` in `~/.hermes/.env` |
| Firmware toolchain (optional) | never — the global Claude Code `esp-idf` skill lives in `~/.claude/skills/esp-idf/`; Hermes does not load it | `~/esp/esp-idf`; recipe in `~/.claude/skills/esp-idf/setup-macos.md` |

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
8. LED face, if this Mac drives the matrix (over Wi-Fi, or on USB): bootstrap enables the `athena-face` plugin and symlinks it into `~/.hermes/plugins/`, so it loads in every Hermes process, including the launchd-managed gateway (which always runs with cwd `~/.hermes` and would not see the checkout copy otherwise); bootstrap restarts a running gateway when the link is new. The board answers as `athena-matrix.local` once it runs a firmware built with the network in `athena_secrets.h` (see `RGB-MATRIX.md`); put `ATHENA_MATRIX_HOST=athena-matrix.local` in `~/.hermes/.env` on the gateway Mac so the plugin never depends on the cable. Check the board with `scripts/face.py --demo` (`--host athena-matrix.local` to insist on Wi-Fi); over Wi-Fi this works while a gateway is up. Over USB only one process can hold the port, so with a gateway running send a Telegram message to see the face change.
9. Point cron at **one always-on machine**. On that Mac: `./scripts/bootstrap.sh --cron` (or `./scripts/sync-cron.sh`). Jobs only fire while the Hermes gateway is running (`hermes gateway`). Do not enable the same jobs on two laptops. After the first `--cron` / `sync-cron.sh` success, later bootstraps on that machine keep jobs in sync.

Full machine move (same you, new computer): `hermes backup` / `hermes import`, then encrypt the zip. That copies secrets. For a second laptop, use this repo + re-login instead.

## After bootstrap, smoke-test

In a **new** Hermes session (cwd = this repo):

- Obsidian: “search the vault for X”
- Linear: “list my Linear issues”
- Gmail / Calendar: “what’s on my calendar tomorrow?” / “any unread mail I should see?”
- LED face (if attached): the panel should show `think` while the answer is generated and `speak` when it lands; `ATHENA_FACE_DRY_RUN=1 hermes chat` prints the same lines to stderr without a board

## Adding a routine later

1. Copy `cron/morning-brief.example.json` (or write a new JSON next to it). Required fields: `name`, `schedule`, `prompt`. Optional: `deliver` (default `telegram`), `skills`, `workdir` (`repo` = this checkout).
2. On the gateway Mac only:

   ```bash
   ./scripts/sync-cron.sh
   ```

   That creates missing jobs and updates existing ones that share the same `name`. Use `--dry-run` first if you want to see the actions.
3. Commit the example JSON. Leave `~/.hermes/cron/jobs.json` untracked.

Delivery uses `TELEGRAM_HOME_CHANNEL`. If nothing is configured, output stays local under `~/.hermes/cron/output/`.
