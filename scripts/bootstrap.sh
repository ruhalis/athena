#!/usr/bin/env bash
# Apply this Athena checkout onto the local Hermes profile.
# Safe to re-run. Never prints secret values.
# Pass --cron on the gateway Mac to install/update jobs from cron/*.example.json.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HERMES_HOME="${HERMES_HOME:-$HOME/.hermes}"
HERMES_PY="${HERMES_HOME}/hermes-agent/venv/bin/python"
GSETUP="${HERMES_HOME}/skills/productivity/google-workspace/scripts/setup.py"
MCP_JSON="${ROOT}/mcp.json"
MARKER="${HERMES_HOME}/athena.cron-machine"
WANT_CRON=0

for arg in "$@"; do
  case "${arg}" in
    --cron) WANT_CRON=1 ;;
    -h|--help)
      cat <<EOF
Usage: $0 [--cron]

Apply this checkout onto the local Hermes profile (cwd, skill trust, Linear, Google client).

  --cron   also create/update scheduled jobs from cron/*.example.json
           (gateway Mac only; after the first success, later bootstraps keep them in sync)
EOF
      exit 0
      ;;
    *)
      echo "unknown argument: ${arg}" >&2
      echo "Usage: $0 [--cron]" >&2
      exit 1
      ;;
  esac
done

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing command: $1"
    exit 1
  }
}

need hermes
need python3

echo "Athena root: ${ROOT}"
echo "Hermes home: ${HERMES_HOME}"

if [[ ! -d "${HERMES_HOME}" ]]; then
  echo "Hermes is not installed at ${HERMES_HOME}. Install Hermes Agent first."
  exit 1
fi

# PID of the running gateway, or non-zero exit when none is running here.
gateway_pid() {
  local pid
  pid="$(python3 -c "
import json
try: print(json.load(open('${HERMES_HOME}/gateway.pid'))['pid'])
except Exception: pass
" 2>/dev/null)" || true
  [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null && printf '%s' "${pid}"
}

# config.yaml mtime as epoch seconds, or 0 when it does not exist.
config_mtime() {
  python3 -c "
import os
try: print(int(os.path.getmtime('${HERMES_HOME}/config.yaml')))
except OSError: print(0)
" 2>/dev/null || echo 0
}

# True when gateway PID $1 started before epoch $2 -- i.e. it bridged
# TERMINAL_CWD from a config older than the one in effect now. Unknown -> true,
# because restarting needlessly is cheap and a stale gateway fails silently.
# $2 must be sampled BEFORE this script's own `hermes config set`, which
# rewrites config.yaml (and bumps its mtime) even when the value is unchanged.
gateway_predates_config() {
  python3 -c "
import subprocess, sys
from datetime import datetime
pid, written = sys.argv[1], float(sys.argv[2])
try:
    out = subprocess.run(['ps', '-p', pid, '-o', 'lstart='],
                         capture_output=True, text=True, check=True).stdout.strip()
    began = datetime.strptime(out, '%a %b %d %H:%M:%S %Y').timestamp()
except Exception:
    sys.exit(0)
sys.exit(0 if began < written else 1)
" "$1" "$2" 2>/dev/null
}

echo
echo "== overlay =="
PREV_CWD="$(hermes config get terminal.cwd 2>/dev/null | tail -n 1 || true)"
CFG_WRITTEN="$(config_mtime)"
hermes config set terminal.cwd "${ROOT}"
(cd "${ROOT}" && hermes skills trust "${ROOT}")

echo
echo "== athena-face plugin =="
# The plugin lives in this checkout, but Hermes scans ./.hermes/plugins
# relative to the process cwd, and the launchd-supervised gateway always runs
# with cwd ~/.hermes: upstream pins the plist's WorkingDirectory there and
# regenerates the plist, so `hermes gateway restart` from this repo does not
# change it. A symlink in ~/.hermes/plugins/ makes it a *user* plugin, which
# every Hermes process scans regardless of cwd or HERMES_ENABLE_PROJECT_PLUGINS;
# discovery dedupes by name, so a session started here still sees one entry.
FACE_LINK="${HERMES_HOME}/plugins/athena-face"
FACE_SRC="${ROOT}/.hermes/plugins/athena-face"
FACE_LINK_CHANGED=0
mkdir -p "${HERMES_HOME}/plugins"
if [[ -L "${FACE_LINK}" && "$(readlink "${FACE_LINK}")" == "${FACE_SRC}" ]]; then
  echo "already linked: ${FACE_LINK} -> ${FACE_SRC}"
elif [[ -L "${FACE_LINK}" ]]; then
  echo "re-pointing ${FACE_LINK} (was $(readlink "${FACE_LINK}")) -> ${FACE_SRC}"
  ln -sfn "${FACE_SRC}" "${FACE_LINK}"
  FACE_LINK_CHANGED=1
elif [[ -e "${FACE_LINK}" ]]; then
  echo "  !! ${FACE_LINK} exists and is not a symlink -- leaving it alone."
  echo "     Move it away and re-run bootstrap to link this checkout's plugin."
else
  echo "linking ${FACE_LINK} -> ${FACE_SRC}"
  ln -s "${FACE_SRC}" "${FACE_LINK}"
  FACE_LINK_CHANGED=1
fi
# Plugins are also opt-in through plugins.enabled in ~/.hermes/config.yaml.
# The allow-list is merged here through the same config API the CLI uses, so
# it works whether or not `hermes plugins enable` can see the plugin.
if hermes config get plugins.enabled 2>/dev/null | grep -q 'athena-face'; then
  echo "already enabled: athena-face"
else
  echo "enabling plugin: athena-face"
  HERMES_AGENT_DIR="${HERMES_HOME}/hermes-agent" "${HERMES_HOME}/hermes-agent/venv/bin/python" - <<'PY' \
    || echo "  could not enable athena-face: add it to plugins.enabled in ${HERMES_HOME}/config.yaml by hand"
import os, sys
sys.path.insert(0, os.environ["HERMES_AGENT_DIR"])
from hermes_cli.config import load_config, save_config
cfg = load_config()
plugins = cfg.get("plugins")
if not isinstance(plugins, dict):
    plugins = cfg["plugins"] = {}
enabled = plugins.get("enabled")
if not isinstance(enabled, list):
    enabled = []
if "athena-face" not in enabled:
    enabled.append("athena-face")
    plugins["enabled"] = sorted(enabled)
    save_config(cfg)
print("  plugins.enabled:", ", ".join(plugins["enabled"]))
PY
fi

echo
echo "== gateway =="
# A gateway reads two things exactly once, at startup: terminal.cwd (bridged
# to TERMINAL_CWD, which is what makes this repo's project skills in
# .hermes/skills/ resolvable for cron runs) and the plugin directories. A
# gateway already running when either changed keeps serving the old state:
# cron jobs silently drop project skills -- the run reports "Skill(s) not
# found and skipped: morning-brief" and briefs lose the skill's caps and rules
# -- and a freshly linked athena-face never loads. The per-job workdir does
# not cover the skills case: the scheduler resolves skills while building the
# prompt, before it applies the job's workdir.
if gw_pid="$(gateway_pid)"; then
  reason=""
  if [[ "${PREV_CWD}" != "${ROOT}" ]] || gateway_predates_config "${gw_pid}" "${CFG_WRITTEN}"; then
    reason="predates the current terminal.cwd"
  elif [[ "${FACE_LINK_CHANGED}" == "1" ]]; then
    reason="started before athena-face was linked"
  fi
  if [[ -n "${reason}" ]]; then
    echo "  gateway PID ${gw_pid} ${reason} -- restarting it"
    if hermes gateway restart; then
      echo "  gateway restarted; cron jobs resolve this repo's skills and the face plugin is loaded"
    else
      echo "  !! gateway restart FAILED -- restart it yourself, or cron jobs will keep"
      echo "     skipping this repo's skills (morning-brief) and the face stays dark on Telegram."
    fi
  else
    echo "  gateway PID ${gw_pid} already running with terminal.cwd=${ROOT} and athena-face linked"
  fi
else
  echo "  no gateway running here (cron fires only while one is: hermes gateway)"
fi

echo
echo "== Google OAuth client JSON =="
GOOGLE_CLIENT_JSON="${ROOT}/google_client_secret.json"
if [[ ! -f "${GOOGLE_CLIENT_JSON}" ]]; then
  # Google Cloud download name
  shopt -s nullglob
  downloaded=("${ROOT}"/client_secret*.json)
  shopt -u nullglob
  if [[ ${#downloaded[@]} -eq 1 ]]; then
    cp "${downloaded[0]}" "${GOOGLE_CLIENT_JSON}"
    chmod 600 "${GOOGLE_CLIENT_JSON}"
    echo "copied $(basename "${downloaded[0]}") → google_client_secret.json"
  fi
fi
if [[ -f "${GOOGLE_CLIENT_JSON}" ]]; then
  python3 "${HERMES_HOME}/skills/productivity/google-workspace/scripts/setup.py" --client-secret "${GOOGLE_CLIENT_JSON}"
else
  echo "missing ${GOOGLE_CLIENT_JSON} — drop the Desktop client JSON in the repo root"
fi

echo
echo "== Linear MCP declaration =="
if hermes mcp list 2>/dev/null | grep -Eq '(^|[[:space:]])linear[[:space:]]'; then
  echo "already present: linear"
else
  echo "installing catalog MCP: linear"
  hermes mcp install linear
fi
echo "source of truth in git: ${MCP_JSON}"

echo
echo "== env (names only) =="
ENV_FILE="${HERMES_HOME}/.env"
check_env() {
  local key="$1" required="$2"
  if [[ -f "${ENV_FILE}" ]] && grep -E "^${key}=" "${ENV_FILE}" >/dev/null 2>&1; then
    local val
    val="$(grep -E "^${key}=" "${ENV_FILE}" | tail -n1 | cut -d= -f2-)"
    if [[ -n "${val}" && "${val}" != "..." ]]; then
      echo "  ${key}: set"
      return
    fi
  fi
  if [[ "${required}" == "1" ]]; then
    echo "  ${key}: MISSING — add to ${ENV_FILE}"
  else
    echo "  ${key}: not set (optional)"
  fi
}
check_env OPENAI_API_KEY 1
check_env OBSIDIAN_VAULT_PATH 1
check_env HERMES_ENABLE_PROJECT_PLUGINS 1
check_env TELEGRAM_BOT_TOKEN 0
check_env TELEGRAM_ALLOWED_USERS 0
check_env TELEGRAM_HOME_CHANNEL 0

echo
echo "== logins =="
if [[ -f "${HERMES_HOME}/mcp-tokens/linear.json" ]]; then
  echo "  Linear: token file present (if tools are missing, run: hermes mcp login linear)"
else
  echo "  Linear: not logged in — run: hermes mcp login linear"
fi

if [[ -x "${HERMES_PY}" && -f "${GSETUP}" ]]; then
  if "${HERMES_PY}" "${GSETUP}" --check >/dev/null 2>&1; then
    echo "  Google Workspace: AUTHENTICATED"
  else
    echo "  Google Workspace: not authenticated — put google_client_secret.json in the repo root, then finish browser OAuth (see SETUP.md)"
  fi
else
  echo "  Google Workspace: setup script not found (bundled skill missing?)"
fi

echo
echo "== cron routines =="
if [[ "${WANT_CRON}" -eq 1 || -f "${MARKER}" ]]; then
  "${ROOT}/scripts/sync-cron.sh"
else
  echo "not installing jobs (this would duplicate them on a second laptop)."
  echo "On the one always-on gateway Mac:  ./scripts/bootstrap.sh --cron"
  echo "Or just:  ./scripts/sync-cron.sh"
fi

echo
echo "Next: start a new Hermes session from this repo (or rely on terminal.cwd)."
echo "Cron jobs fire only while the gateway is running: hermes gateway"
echo "A gateway reads terminal.cwd once, at startup: after changing it, restart"
echo "the gateway (hermes gateway restart) or cron jobs lose this repo's skills."
echo "See SETUP.md"
