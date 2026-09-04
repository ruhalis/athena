# Athena

Personal text-only assistant. Brain is ChatGPT via `openai-api` (`OPENAI_API_KEY`). Do not use a fallback provider.

This git repo is the Athena customization layer. Do not edit `~/.hermes/hermes-agent` (upstream). Put our changes here:

- Project rules: this file (`HERMES.md`)
- Skills: `.hermes/skills/<name>/SKILL.md`
- Plugins (tools, hooks): `.hermes/plugins/<name>/plugin.yaml`
- MCP declarations: `mcp.json`
- Cron templates: `cron/*.example.json`

New machine: follow `SETUP.md` and run `./scripts/bootstrap.sh`.

## Integrations

### Obsidian

Vault path: `OBSIDIAN_VAULT_PATH` in `~/.hermes/.env`. Use the bundled `obsidian` skill for notes: read, search, create, edit, append, wikilinks. Resolve that absolute path first; never pass `$OBSIDIAN_VAULT_PATH` into file tools.

Layout: `0. Files`, `1. Projects`, `2. Areas`, `Drone`.

### Linear

Linear is connected as the `linear` MCP server (`https://mcp.linear.app/mcp`, OAuth). Declaration lives in `mcp.json`. Use those tools to find, create, and update issues, projects, and comments.

If Linear tools are missing this session, the user still needs `hermes mcp login linear` (browser OAuth), then a new Hermes session.

### Gmail and Google Calendar

Connected via the bundled `google-workspace` skill. The Desktop OAuth client JSON lives at `google_client_secret.json` in this repo root (gitignored). Bootstrap copies it to `~/.hermes/google_client_secret.json`. The user login token stays in `~/.hermes/google_token.json`. Use that skill to search/read/send mail and to list/create/delete calendar events.

If Google tools fail with `NOT_AUTHENTICATED`, the client JSON is missing or the user still needs one browser OAuth pass. See `SETUP.md`.

When the user asks for a morning brief, meeting prep, or "what's on my plate today," load the project `morning-brief` skill. That skill uses Linear MCP plus the `google-workspace` `daily-brief` reference (Gmail and Calendar). It outputs a time-blocked plan and closes by offering to write that plan to Google Calendar. Load the skill again when the user answers that offer — writing the plan is its rules, not improvisation. Writes go only to the dedicated calendar in `ATHENA_CALENDAR_ID`, never the primary one, and only after the user approves that specific plan; a reply carrying edits is a revision to show back, not a green light.

### LED face

A small LED matrix on the desk mirrors your state: listen, think, work, speak, alert (waiting for the user), error, sleep. The `athena-face` plugin drives it from your hooks; you do nothing for it in chat. If the user asks why the face shows something, that is the mapping. Never send serial commands to it yourself.

### Cron

Job *definitions* belong in `cron/*.example.json` in this repo. Live jobs belong on one always-on machine (`~/.hermes/cron/jobs.json`) with the Hermes gateway running. Do not create the same job on two laptops.

To install or refresh live jobs from the examples (gateway machine only): `./scripts/sync-cron.sh`, or `./scripts/bootstrap.sh --cron`. After the first success, later bootstraps on that machine keep jobs in sync.
