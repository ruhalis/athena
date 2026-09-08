# athena-face, plugin only

This branch carries only the Hermes plugin that mirrors the agent's state on the
Athena LED face, plus the script it drives the board with. Nothing else from
`main` is here: no `HERMES.md`, no skills, no MCP or cron declarations, no
firmware. Clone it on a machine whose Hermes should drive the face but must
otherwise stay exactly as it is.

The layout is fixed: the plugin finds `scripts/face.py` three directories above
its own file.

    .hermes/plugins/athena-face/   the plugin (__init__.py, plugin.yaml)
    scripts/face.py                the board protocol, standard library only; also a CLI

## Install

    git clone -b face-plugin https://github.com/ruhalis/athena.git ~/athena-face
    mkdir -p ~/.hermes/plugins
    ln -s ~/athena-face/.hermes/plugins/athena-face ~/.hermes/plugins/athena-face
    hermes plugins enable athena-face   # or add athena-face to plugins.enabled in ~/.hermes/config.yaml
    hermes plugins list                 # athena-face should be listed and enabled

The symlink makes it a *user* plugin, which every Hermes process loads whatever
its working directory is; `HERMES_ENABLE_PROJECT_PLUGINS` is not needed.

Point it at the board in `~/.hermes/.env`:

    ATHENA_MATRIX_HOST=athena-matrix.local   # the board over Wi-Fi, host[:port], port 7075 by default
    ATHENA_FACE_SLEEP=22:00-08:00            # optional: idle shows as sleep inside this window
    ATHENA_FACE_BRIGHTNESS=64                # optional: 0..255

Check it without touching a running gateway:

    python3 scripts/face.py --host athena-matrix.local --ping    # expects: ok
    ATHENA_FACE_DRY_RUN=1 hermes chat -q "reply with one word"  # stderr shows: athena-face -> {"mode":...}

Then restart the gateway, which reads plugins once at startup:

    hermes gateway restart
    grep -E "athena-face|face board" ~/.hermes/logs/gateway.log | tail

The face shows the idle clock as soon as the plugin loads, then listen, think,
work and speak follow the agent.

## Rules

- One machine drives the face. A second gateway running this plugin makes the
  panel flicker between two agents; set `ATHENA_FACE=0` on the other one.
- Hermes 0.17 and later work. On 0.17 a failed session ends in idle instead of
  error, because `on_session_end` carries no `failed` there.
- Nothing here changes the agent itself: no system prompt, no skills, no tools.

## Sync from main

The plugin is developed on `main`. To bring changes here:

    git checkout face-plugin
    git checkout main -- .hermes/plugins/athena-face scripts/face.py
    git commit -m "Sync athena-face from main"

then `git pull` on the face machine and restart its gateway.

Environment reference: `ATHENA_FACE=0` disables the plugin, `ATHENA_FACE_DRY_RUN=1`
prints each line instead of sending it, `ATHENA_MATRIX_PORT` picks a serial device
over USB, `ATHENA_MATRIX_HOST` the board over Wi-Fi.
