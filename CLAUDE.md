# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

Athena is a personal text-only assistant that runs on **Hermes Agent 0.20.4**. This repo is the portable *overlay* (markdown instructions, one project skill, an MCP declaration, a cron template, two bash scripts) that gets applied onto a machine-local Hermes install. The agent runtime itself lives at `~/.hermes/hermes-agent` and is upstream software.

There is **no application code in this checkout**. Consequence: no build, no test suite, no linter, no package manifest. "Changing Athena's behavior" means editing prose in `HERMES.md` or a `SKILL.md`, or editing JSON/YAML declarations — not writing functions. The exception is ESP32 firmware under `firmware/` (currently `firmware/athena_matrix/`, see "Firmware" below). It builds with `idf.py` under the global Claude Code `esp-idf` skill (`~/.claude/skills/esp-idf/`, personal, not in this repo, never loaded by Hermes).

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
hermes gateway restart            # after changing terminal.cwd by hand (see Cron model)
. ~/esp/esp-idf/export.sh >/dev/null && cd firmware/<board> && idf.py build   # once a firmware project exists; workflow in the global esp-idf skill
```

Bootstrap sets `terminal.cwd`, trusts project skills, restarts a stale gateway, copies the Google Desktop client JSON into `~/.hermes/`, ensures the Linear MCP is declared, and reports which env vars and browser logins are still missing. It never prints secret values and is safe to re-run.

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

`sync-cron.sh` reconciles the two: it matches a template to a live job **by `name`** (case-insensitive fallback), creates it if absent, and edits it when schedule, prompt, deliver, skills, or workdir drift. It re-reads `jobs.json` after every create/edit and fails loudly if the live job still differs, because `hermes cron create`/`edit` can exit 0 after printing a failure. `workdir: "repo"` resolves to this checkout. Once it succeeds it drops a marker at `~/.hermes/athena.cron-machine`, after which plain `./scripts/bootstrap.sh` on that machine keeps jobs in sync automatically.

Required template fields: `name`, `schedule`, `prompt`. Optional: `deliver` (default `telegram`), `skills`, `workdir`. Delivery targets `TELEGRAM_HOME_CHANNEL`; with no Telegram config, output stays in `~/.hermes/cron/output/`.

**A gateway reads `terminal.cwd` once, at startup.** That value is what makes `.hermes/skills/` resolvable for cron runs; a job's own `workdir` does not cover it, because the scheduler resolves skills while building the prompt, before applying workdir. A gateway started before `terminal.cwd` changed silently drops project skills (`⚠️ Skill(s) not found and skipped: morning-brief`). `bootstrap.sh` detects and restarts such a gateway; after changing `terminal.cwd` by hand, run `hermes gateway restart` yourself.

Editing a cron prompt is a two-part change: commit the template **and** re-run `sync-cron.sh` on the gateway, or the live job keeps the old prompt.

## Integrations

- **Obsidian** — bundled upstream skill. Vault path comes from `OBSIDIAN_VAULT_PATH` in `~/.hermes/.env`; instructions require resolving it to an absolute path before calling file tools (never pass the `$VAR` through).
- **Linear** — remote MCP server (`https://mcp.linear.app/mcp`, OAuth). Declared in `mcp.json` (source of truth in git); the token lives in `~/.hermes/mcp-tokens/linear.json`. Not a skill — it is MCP tools.
- **Gmail / Calendar** — bundled upstream `google-workspace` skill. Desktop OAuth client JSON sits at `google_client_secret.json` in the repo root (gitignored); bootstrap installs it via the skill's `scripts/setup.py`. The user token (`~/.hermes/google_token.json`) needs one browser pass per machine. The skill's `google_api.py` runs only under the Hermes venv interpreter (`~/.hermes/hermes-agent/venv/bin/python`): system `python3` lacks the Google client libraries and `pip install` fails (externally managed). Its calendar CLI has list/create/delete but no update.
- **`morning-brief`** (`.hermes/skills/morning-brief/`) — the only Hermes project skill. Composes Linear MCP + the `google-workspace` `daily-brief` reference into a time-blocked plan for Asia/Almaty (PLAN / TO DO / HEADS-UP, plain text for Telegram). Near-read-only: a brief never sends mail and never mutates Linear. Its one write is a **dedicated Athena calendar** (`ATHENA_CALENDAR_ID` in `~/.hermes/.env`, never the primary one), and only after the user approves that specific plan in chat — approving the feature is not approving a plan, and a reply carrying edits is a revision, not consent. Events carry `[athena-plan]` in the description; re-planning is delete-then-create, scoped to that calendar and the current day.
- **`esp-idf`** — a **global** Claude Code skill at `~/.claude/skills/esp-idf/`. Load it for any ESP32 / ESP-IDF / firmware request; it holds the toolchain pin (`idf-version`, the only authoritative copy), the macOS workflow, the bounded serial reader, and the flash boundary (build freely, flash only on an explicit ask to a known port, erase-flash only after an explicit yes, never `menuconfig`, install the toolchain only when asked). What is specific to Athena is in "Firmware" below.

## Conventions when editing

- **Model policy is fixed:** ChatGPT via `openai-api` (`OPENAI_API_KEY`; `config.example.yaml` pins `gpt-5.6-luna`). Do not add or suggest a fallback provider — `README.md`, `HERMES.md`, and `config.example.yaml` all state this deliberately.
- `config.example.yaml` is a **manual merge target** for `~/.hermes/config.yaml`, not applied by any script and referenced by no code. Merge selected keys; never replace the whole runtime config.
- Skills are `.hermes/skills/<name>/SKILL.md` with YAML frontmatter (`name`, `description`, `version`) and a numbered procedure. `morning-brief` is the pattern to copy: explicit steps, explicit caps ("at most 5 issues"), explicit degradation ("if Linear tools are missing, say so and continue"), and a stated authorization boundary.
- Project plugins require `HERMES_ENABLE_PROJECT_PLUGINS=true` in `~/.hermes/.env`.
- Anything user-facing that changes agent behavior should also be reflected in `HERMES.md` (agent-facing) and, if it changes setup, `SETUP.md` (human-facing). These three docs are kept consistent by hand.
- `.gitignore` guards `.env`, `client_secret*.json`, `google_client_secret.json`, `google_token.json`. Do not relax these.

## Firmware

**State of this checkout:** `firmware/athena_matrix/` exists as a **bit-banged prototype** that builds for both the ESP32-WROOM-32 (`idf.py set-target esp32`) and the ESP32-S3-DevKitC-1 (`set-target esp32s3`, octal PSRAM on): a from-scratch HUB75 driver in `components/hub75/`, a demo in `main/main.c`, a per-target pin map in `main/board_pins.h` (the S3 map is the J1 layout from `RGB-MATRIX.md`), per-target flash and PSRAM keys in `sdkconfig.defaults.<target>`, wiring and troubleshooting in its `README.md`. On the S3 this driver is an interim step; the DMA driver below is still the plan. `firmware/athena_hello/` is a serial smoke test for the S3 (prints chip facts, then one `alive for N s` line per second) to flash on a fresh board or when a port or cable is in doubt; it has no components and no pin map, keep it that way. The serial JSON protocol from `RGB-MATRIX.md` is not implemented yet, and `firmware/athena_audio/` and `firmware/athena_hotspot/` do not exist. The prototype does not use the HUB75 driver submodule: `.gitmodules` declares it, but there is no gitlink and nothing is checked out, so `git submodule update --init` does nothing; materialize it with `git submodule add https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA firmware/athena_matrix/components/ESP32-HUB75-MatrixPanel-I2S-DMA` only when the S3 build switches to it. `RGB-MATRIX.md` and `AUDIO-BOARD.md` are design notes to build against, not descriptions of current behavior; the Mac hub `athena.py` and the `display:` config they reference do not exist either.

The global `esp-idf` skill owns the workflow; board facts, pin maps, and `sdkconfig.defaults` starters live in the design notes; this section holds only the Athena-specific layout. Do not duplicate one into another. Decided so far:

- Two board projects, `firmware/athena_matrix/` (face, `RGB-MATRIX.md`) and `firmware/athena_audio/` (ears and mouth, `AUDIO-BOARD.md`), plus `firmware/athena_hotspot/`, a minimal bring-up project that only starts the access point and logs joins and leaves — keep it that small. All target `esp32s3` on an ESP32-S3-DevKitC-1 with the N16R8 module (16 MB flash, 8 MB octal PSRAM); the matrix project additionally keeps its `esp32` (WROOM-32 DevKit) target for the original prototype board.
- Shared code in `firmware/components/athena_common/` (Wi-Fi, `esp_websocket_client`, hub protocol, status LED), pulled in by each project through `EXTRA_COMPONENT_DIRS`. Board-specific code stays in the board projects.
- Transport differs per board. The matrix is driven over **USB serial** for now (one JSON line per state change on UART0, replies `ok`/`err`; no Wi-Fi, no secrets header) with WebSocket deferred, not dropped. The audio board dials the hub over WebSocket. Registry dependencies: `espressif/esp-sr` (audio only), `espressif/esp_websocket_client`, `espressif/mdns`.
- On the S3 the HUB75 driver is `ESP32-HUB75-MatrixPanel-I2S-DMA` as a git submodule at the path above (its CMake expects that directory name), GFX off, behind one `.cpp` wrapper with `extern "C"` functions. The in-tree `hub75` component (plain GPIO writes, BCM, every pin in GPIO 0..31, refresh task owns core 1) exists because the classic ESP32 has no LCD_CAM; it also runs on the S3 with the same pin map as the DMA driver will use, which is how the S3 board is driven until the submodule is wired in. Everything else is C.
- Secrets: `firmware/components/athena_common/include/athena_secrets.h`, gitignored, one file for both boards, copied from a committed `.example`.
- Board ports are the `## Boards` section at the end of this file, one `<board>: /dev/cu.…` line each (`matrix`, `matrix-wroom`, later `audio`); the `esp-idf` skill reads that section. There is no `CLAUDE.local.md` in this repo any more, it was merged here.
- Waveshare's wiring figures mislead in two ways. They number the ribbon 16 down to 1 (R1 is wire 16, the last GND is wire 1) where `RGB-MATRIX.md` and the matrix `README.md` use HUB75 numbering 1 to 16 (R1 is pin 1): same layout, their wire N is pin 17 − N here, so wire by signal name. And their ESP32-S3 GPIO diagram is the DMA library's default map plus E on GPIO9, not ours; on the WROOM its GPIOs 6, 7 and 8 are the flash. The only wiring sources are `main/board_pins.h` and the tables in `RGB-MATRIX.md` (S3) and `firmware/athena_matrix/README.md` (WROOM).
- `.gitignore` already covers `sdkconfig`, `sdkconfig.old`, `managed_components/`, `build/`, the secrets header, and re-includes `firmware/**/lib/` (the Python template above it ignores every `lib/`).

## Boards

Ports for the `esp-idf` skill. This section replaces the former gitignored `CLAUDE.local.md`; port names are not secrets. Only one board is normally on USB, so check `ls /dev/cu.usb*` before flashing and match `idf.py set-target` to the board (`set-target` wipes `build/`).

matrix: /dev/cu.usbmodem5C390168231
matrix-wroom: /dev/cu.usbserial-A5069RR4

- `matrix` is the ESP32-S3-DevKitC-1 (N16R8), target `esp32s3`. The name is its **native USB** connector and follows the Mac USB port it sits in; replace it with the `/dev/cu.usbserial-*` name once the cable is on the UART connector.
- `matrix-wroom` is the ESP32-WROOM-32 DevKit the prototype was first built on (ESP32-D0WD-V3 rev 3.1, 4 MB flash, 40 MHz crystal), target `esp32`. FTDI bridge with a programmed serial, so the name is stable wherever it is plugged in. Verified 2026-09-04: it was the board on USB, running the `esp32` build of `athena_matrix` from b281ac2 (wiring-test scene, WROOM pin map, 145 Hz refresh).
- `audio:` gets added when `firmware/athena_audio/` and its board exist.
