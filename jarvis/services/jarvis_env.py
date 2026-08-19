"""Load repo-root .env and resolve JARVIS_HOME / config."""
from __future__ import annotations

import os
from pathlib import Path

import yaml
from dotenv import load_dotenv

SERVICES_DIR = Path(__file__).resolve().parent
JARVIS_DIR = SERVICES_DIR.parent
REPO_ROOT = JARVIS_DIR.parent


def load_env() -> None:
    load_dotenv(REPO_ROOT / ".env", override=False)


def jarvis_home() -> Path:
    return Path(os.environ.get("JARVIS_HOME") or JARVIS_DIR)


def load_config() -> dict:
    load_env()
    path = jarvis_home() / "config.yaml"
    data = {}
    if path.exists():
        data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    if os.environ.get("REDIS_URL"):
        data["redis_url"] = os.environ["REDIS_URL"]
    ha = data.setdefault("home_assistant", {})
    if os.environ.get("HA_URL"):
        ha["url"] = os.environ["HA_URL"]
    if os.environ.get("HA_TOKEN"):
        ha["token"] = os.environ["HA_TOKEN"]
    return data
