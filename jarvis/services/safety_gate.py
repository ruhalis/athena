"""Fail-closed safety checks for Home Assistant actions.

Locks, alarms, and garage/covers require a typed "yes" on the next turn.
Nothing here bypasses a deny. There is no override flag.
"""
from __future__ import annotations

import json
import os
import time

import redis

PENDING_KEY = "jarvis:pending_confirmation"
PENDING_TTL_S = 90

CONFIRM_WORDS = frozenset({"yes", "y", "confirm", "do it", "proceed"})

_DEFAULT_DOMAINS = ("lock", "alarm_control_panel", "cover", "garage_door")
_DEFAULT_SERVICES = (
    "lock.lock",
    "lock.unlock",
    "alarm_control_panel.alarm_arm_away",
    "alarm_control_panel.alarm_disarm",
    "cover.open_cover",
    "cover.close_cover",
)


def _client() -> redis.Redis:
    url = os.environ.get("REDIS_URL", "redis://127.0.0.1:6379/0")
    return redis.from_url(url, decode_responses=True)


def requires_confirmation(
    entity_id: str,
    service: str,
    *,
    domains: list[str] | None = None,
    services: list[str] | None = None,
) -> bool:
    domains = tuple(domains) if domains else _DEFAULT_DOMAINS
    services = tuple(services) if services else _DEFAULT_SERVICES
    domain = (entity_id or "").split(".", 1)[0]
    svc = service or ""
    if domain in domains:
        return True
    if svc in services:
        return True
    if "garage" in (entity_id or "").lower():
        return True
    return False


def is_confirm_text(text: str) -> bool:
    return (text or "").strip().lower() in CONFIRM_WORDS


def set_pending(action: dict) -> None:
    payload = json.dumps({**action, "ts": time.time()})
    r = _client()
    r.set(PENDING_KEY, payload, ex=PENDING_TTL_S)


def get_pending() -> dict | None:
    r = _client()
    raw = r.get(PENDING_KEY)
    if not raw:
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return None


def clear_pending() -> None:
    _client().delete(PENDING_KEY)
