#!/usr/bin/env bash
# Install or update live Hermes cron jobs from cron/*.example.json.
# Idempotent. Run on the one always-on gateway machine only.
# Safe to re-run after editing an example (prompt, schedule, skills).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HERMES_HOME="${HERMES_HOME:-$HOME/.hermes}"
JOBS_FILE="${HERMES_HOME}/cron/jobs.json"
MARKER="${HERMES_HOME}/athena.cron-machine"
DRY_RUN=0

usage() {
  cat <<EOF
Usage: $0 [--dry-run]

Create or update Hermes cron jobs from ${ROOT}/cron/*.example.json.
Match by job name. Sets workdir to this checkout so project skills load.

Run this on the gateway Mac only (jobs fire while \`hermes gateway\` is up).
Re-running bootstrap on that machine will sync again once this has succeeded.

  --dry-run   print create/update actions without calling hermes
EOF
}

for arg in "$@"; do
  case "${arg}" in
    --dry-run) DRY_RUN=1 ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "unknown argument: ${arg}" >&2
      usage >&2
      exit 1
      ;;
  esac
done

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing command: $1" >&2
    exit 1
  }
}

need hermes
need python3

if [[ ! -d "${HERMES_HOME}" ]]; then
  echo "Hermes is not installed at ${HERMES_HOME}." >&2
  exit 1
fi

echo "Athena root: ${ROOT}"
echo "Hermes home: ${HERMES_HOME}"
if [[ "${DRY_RUN}" -eq 1 ]]; then
  echo "mode: dry-run"
fi
echo

export ATHENA_ROOT="${ROOT}"
export ATHENA_JOBS_FILE="${JOBS_FILE}"
export ATHENA_CRON_DRY_RUN="${DRY_RUN}"

python3 - <<'PY'
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

root = Path(os.environ["ATHENA_ROOT"])
jobs_file = Path(os.environ["ATHENA_JOBS_FILE"])
dry_run = os.environ.get("ATHENA_CRON_DRY_RUN") == "1"
examples_dir = root / "cron"
examples = sorted(examples_dir.glob("*.example.json"))

if not examples:
    print(f"no templates in {examples_dir}/*.example.json")
    raise SystemExit(0)


def load_live():
    if not jobs_file.exists():
        return []
    try:
        data = json.loads(jobs_file.read_text())
    except json.JSONDecodeError as exc:
        print(f"cannot parse {jobs_file}: {exc}", file=sys.stderr)
        raise SystemExit(1)
    if isinstance(data, list):
        return data
    if isinstance(data, dict):
        jobs = data.get("jobs", [])
        if not isinstance(jobs, list):
            return []
        return jobs
    return []


def as_list(value):
    if value is None:
        return []
    if isinstance(value, list):
        return [str(v) for v in value if str(v).strip()]
    text = str(value).strip()
    return [text] if text else []


def live_schedule(job):
    display = job.get("schedule_display")
    if display:
        return str(display)
    schedule = job.get("schedule")
    if isinstance(schedule, dict):
        return str(schedule.get("value") or schedule.get("expr") or "")
    if schedule:
        return str(schedule)
    return ""


def live_deliver(job):
    items = as_list(job.get("deliver"))
    return items[0] if items else "local"


def live_skills(job):
    skills = as_list(job.get("skills"))
    if skills:
        return skills
    return as_list(job.get("skill"))


def load_spec(path: Path) -> dict:
    spec = json.loads(path.read_text())
    if not isinstance(spec, dict):
        raise ValueError(f"{path.name}: expected a JSON object")
    name = str(spec.get("name") or "").strip()
    schedule = str(spec.get("schedule") or "").strip()
    prompt = str(spec.get("prompt") or "").strip()
    if not name or not schedule or not prompt:
        raise ValueError(f"{path.name}: need name, schedule, and prompt")
    deliver_items = as_list(spec.get("deliver"))
    deliver = deliver_items[0] if deliver_items else "telegram"
    skills = as_list(spec.get("skills"))
    workdir_raw = str(spec.get("workdir") or "repo").strip()
    workdir = str(root) if workdir_raw in ("", "repo") else workdir_raw
    if not Path(workdir).is_dir():
        raise ValueError(f"{path.name}: workdir is not a directory: {workdir}")
    return {
        "file": path.name,
        "name": name,
        "schedule": schedule,
        "prompt": prompt,
        "deliver": deliver,
        "skills": skills,
        "workdir": workdir,
    }


def find_existing(jobs, name):
    matches = [j for j in jobs if str(j.get("name") or "") == name]
    if not matches:
        lowered = name.lower()
        matches = [j for j in jobs if str(j.get("name") or "").lower() == lowered]
    if len(matches) > 1:
        ids = ", ".join(str(j.get("id") or "?") for j in matches)
        raise ValueError(f"multiple live jobs named {name!r}: {ids}")
    return matches[0] if matches else None


def needs_update(job, spec):
    return (
        live_schedule(job) != spec["schedule"]
        or (job.get("prompt") or "") != spec["prompt"]
        or live_deliver(job) != spec["deliver"]
        or live_skills(job) != spec["skills"]
        or str(job.get("workdir") or "") != spec["workdir"]
    )


def run_hermes(args):
    print("  $ hermes " + " ".join(shlex.quote(a) for a in args))
    if dry_run:
        return
    result = subprocess.run(["hermes", *args], check=False)
    if result.returncode != 0:
        raise SystemExit(result.returncode)


def verify(spec, expect):
    """`hermes cron create`/`edit` can exit 0 after printing a failure, so
    confirm the live job really matches the template before counting it."""
    if dry_run:
        return True
    job = find_existing(load_live(), spec["name"])
    if job is None:
        print(f"  FAILED ({expect}): no live job named {spec['name']!r}", file=sys.stderr)
        return False
    if needs_update(job, spec):
        print(f"  FAILED ({expect}): live job {spec['name']!r} still differs from template", file=sys.stderr)
        return False
    return True


live = load_live()
errors = 0
created = 0
updated = 0
unchanged = 0

for path in examples:
    try:
        spec = load_spec(path)
    except (OSError, json.JSONDecodeError, ValueError) as exc:
        print(f"skip {path.name}: {exc}", file=sys.stderr)
        errors += 1
        continue

    existing = find_existing(live, spec["name"])
    print(f"== {spec['name']}  ({spec['file']}) ==")
    if existing is None:
        args = [
            "cron",
            "create",
            spec["schedule"],
            spec["prompt"],
            "--name",
            spec["name"],
            "--deliver",
            spec["deliver"],
            "--workdir",
            spec["workdir"],
        ]
        for skill in spec["skills"]:
            args.extend(["--skill", skill])
        run_hermes(args)
        if verify(spec, "create"):
            created += 1
            print("  created")
        else:
            errors += 1
        continue

    if not needs_update(existing, spec):
        unchanged += 1
        print(f"  unchanged  id={existing.get('id')}")
        continue

    args = [
        "cron",
        "edit",
        spec["name"],
        "--schedule",
        spec["schedule"],
        "--prompt",
        spec["prompt"],
        "--deliver",
        spec["deliver"],
        "--workdir",
        spec["workdir"],
    ]
    if spec["skills"]:
        for skill in spec["skills"]:
            args.extend(["--skill", skill])
    else:
        args.append("--clear-skills")
    run_hermes(args)
    if verify(spec, "edit"):
        updated += 1
        print("  updated")
    else:
        errors += 1

print()
print(f"created={created}  updated={updated}  unchanged={unchanged}  errors={errors}")
if errors:
    raise SystemExit(1)
PY

if [[ "${DRY_RUN}" -eq 0 ]]; then
  mkdir -p "${HERMES_HOME}"
  printf '%s\n' "${ROOT}" > "${MARKER}"
  echo "gateway marker: ${MARKER}"
  echo "Jobs fire only while the gateway is running: hermes gateway"
fi
