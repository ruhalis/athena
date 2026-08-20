#!/usr/bin/env bash
# Apply this Athena checkout onto the local Hermes profile.
# Safe to re-run. Never prints secret values.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HERMES_HOME="${HERMES_HOME:-$HOME/.hermes}"
HERMES_PY="${HERMES_HOME}/hermes-agent/venv/bin/python"
GSETUP="${HERMES_HOME}/skills/productivity/google-workspace/scripts/setup.py"
MCP_JSON="${ROOT}/mcp.json"

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

echo
echo "== overlay =="
hermes config set terminal.cwd "${ROOT}"
(cd "${ROOT}" && hermes skills trust "${ROOT}")

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
echo "Next: start a new Hermes session from this repo (or rely on terminal.cwd)."
echo "Cron jobs fire only while the gateway is running, on one machine: hermes gateway"
echo "See SETUP.md"
