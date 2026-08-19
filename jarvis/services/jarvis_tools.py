"""Shared Jarvis tools. Used by the MCP server and the brain tool loop."""
from __future__ import annotations

import datetime as dt
import json
import os
import re
import sys
from pathlib import Path
from typing import Any

import httpx
import redis

import jarvis_bus as bus
from jarvis_env import jarvis_home, load_config
from safety_gate import (
    clear_pending,
    get_pending,
    requires_confirmation,
    set_pending,
)

LOG_DIR = jarvis_home() / "logs"
LOG_DIR.mkdir(parents=True, exist_ok=True)

MEMORY_START = "<!-- jarvis-memory:start -->"
MEMORY_END = "<!-- jarvis-memory:end -->"
MEMORY_CAP_BYTES = 2048


def _sync_redis() -> redis.Redis | None:
    url = os.environ.get("REDIS_URL", "redis://127.0.0.1:6379/0")
    try:
        client = redis.from_url(url, decode_responses=True)
        client.ping()
        return client
    except Exception as exc:
        print(f"[tools] redis unavailable ({exc})", file=sys.stderr)
        return None


def log_event(event: dict) -> None:
    log_file = LOG_DIR / f"{dt.date.today().isoformat()}.jsonl"
    entry = {"ts": dt.datetime.now(dt.timezone.utc).isoformat(), **event}
    with log_file.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")


def publish_reply(text: str, language: str = "en") -> None:
    client = _sync_redis()
    if client is None:
        return
    client.publish(
        bus.CH_ASSISTANT_REPLY,
        json.dumps({"text": text, "lang": language, "priority": "normal"}, ensure_ascii=False),
    )


def speak(text: str, language: str = "en") -> str:
    text = (text or "").strip()
    language = language or "en"
    log_event({"type": "speak", "language": language, "text": text})
    print(f"[SPEAK:{language}] {text}", file=sys.stderr, flush=True)
    if text:
        publish_reply(text, language)
    return f"[Shown in {language}]: {text}"


def _ha_headers(cfg: dict) -> dict[str, str]:
    token = cfg.get("home_assistant", {}).get("token") or os.environ.get("HA_TOKEN") or ""
    return {"Authorization": f"Bearer {token}", "Content-Type": "application/json"}


def _ha_base(cfg: dict) -> str:
    return (cfg.get("home_assistant", {}).get("url") or os.environ.get("HA_URL") or "").rstrip("/")


def ha_control(entity_id: str, service: str, attributes: dict | None = None) -> str:
    cfg = load_config()
    ha_cfg = cfg.get("home_assistant") or {}
    attributes = attributes or {}
    pending = get_pending()
    if pending and pending.get("entity_id") == entity_id and pending.get("service") == service:
        clear_pending()
    elif requires_confirmation(
        entity_id,
        service,
        domains=ha_cfg.get("confirm_domains"),
        services=ha_cfg.get("confirm_services"),
    ):
        set_pending({"entity_id": entity_id, "service": service, "attributes": attributes})
        msg = (
            f"That action ({service} on {entity_id}) needs confirmation. "
            "Type yes to proceed. It will not run otherwise."
        )
        log_event({"type": "ha_blocked", "entity_id": entity_id, "service": service})
        return msg

    base = _ha_base(cfg)
    token = ha_cfg.get("token") or os.environ.get("HA_TOKEN")
    if not base or "192.168.1.x" in base or not token:
        log_event({"type": "ha_control_unconfigured", "entity_id": entity_id, "service": service})
        return f"Home Assistant is not configured. Would have called {service} on {entity_id}."

    domain = entity_id.split(".", 1)[0]
    svc = service.split(".")[-1]
    url = f"{base}/api/services/{domain}/{svc}"
    body = {"entity_id": entity_id, **attributes}
    try:
        with httpx.Client(timeout=10.0) as client:
            r = client.post(url, headers=_ha_headers(cfg), json=body)
            r.raise_for_status()
    except Exception as exc:
        log_event({"type": "ha_error", "error": str(exc), "entity_id": entity_id})
        return f"Home Assistant error: {exc}"
    log_event({"type": "ha_control", "entity_id": entity_id, "service": service, "attributes": attributes})
    return f"Called {service} on {entity_id}."


def ha_query(entity_id: str) -> str:
    cfg = load_config()
    base = _ha_base(cfg)
    token = (cfg.get("home_assistant") or {}).get("token") or os.environ.get("HA_TOKEN")
    if not base or "192.168.1.x" in base or not token:
        return f"Home Assistant is not configured. Cannot query {entity_id}."
    url = f"{base}/api/states/{entity_id}"
    try:
        with httpx.Client(timeout=10.0) as client:
            r = client.get(url, headers=_ha_headers(cfg))
            r.raise_for_status()
            data = r.json()
    except Exception as exc:
        return f"Home Assistant error: {exc}"
    state = data.get("state")
    attrs = data.get("attributes") or {}
    log_event({"type": "ha_query", "entity_id": entity_id, "state": state})
    return json.dumps({"entity_id": entity_id, "state": state, "attributes": attrs}, ensure_ascii=False)


def set_timer(seconds: int, label: str = "timer") -> str:
    seconds = int(seconds)
    if seconds <= 0:
        return "Timer duration must be a positive number of seconds."
    client = _sync_redis()
    payload = {"seconds": seconds, "label": label or "timer"}
    log_event({"type": "timer_set", **payload})
    if client is not None:
        client.publish(bus.CH_TIMER_SET, json.dumps(payload, ensure_ascii=False))
    return f"Timer set for {seconds} seconds ({label})."


def get_weather() -> str:
    cfg = load_config()
    w = cfg.get("weather") or {}
    lat = w.get("latitude")
    lon = w.get("longitude")
    tz = w.get("timezone") or "UTC"
    if lat is None or lon is None:
        return "Weather location is not configured."
    url = "https://api.open-meteo.com/v1/forecast"
    params = {
        "latitude": lat,
        "longitude": lon,
        "current": "temperature_2m,weather_code,wind_speed_10m",
        "timezone": tz,
    }
    try:
        with httpx.Client(timeout=10.0) as client:
            r = client.get(url, params=params)
            r.raise_for_status()
            data = r.json()
    except Exception as exc:
        return f"Weather lookup failed: {exc}"
    current = data.get("current") or {}
    log_event({"type": "weather", "current": current})
    return json.dumps(current, ensure_ascii=False)


def memory_write(bullet: str) -> str:
    bullet = (bullet or "").strip().lstrip("- ").strip()
    if not bullet:
        return "Empty memory bullet rejected."
    path = jarvis_home() / "CLAUDE.md"
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    if MEMORY_START not in text or MEMORY_END not in text:
        text += f"\n\n## Memory\n{MEMORY_START}\n{MEMORY_END}\n"
    pattern = re.compile(
        re.escape(MEMORY_START) + r"(.*?)" + re.escape(MEMORY_END),
        re.DOTALL,
    )
    match = pattern.search(text)
    body = (match.group(1) if match else "").strip()
    lines = [ln for ln in body.splitlines() if ln.strip()]
    lines.append(f"- {bullet}")
    section = "\n".join(lines) + "\n"
    if len(section.encode("utf-8")) > MEMORY_CAP_BYTES:
        while lines and len(("\n".join(lines) + "\n").encode("utf-8")) > MEMORY_CAP_BYTES:
            lines.pop(0)
        section = "\n".join(lines) + "\n"
    replacement = f"{MEMORY_START}\n{section}{MEMORY_END}"
    if match:
        text = pattern.sub(replacement, text, count=1)
    path.write_text(text, encoding="utf-8")
    log_event({"type": "memory_write", "bullet": bullet})
    return f"Remembered: {bullet}"


TOOL_HANDLERS = {
    "speak": lambda **kw: speak(kw.get("text") or "", kw.get("language") or "en"),
    "ha_control": lambda **kw: ha_control(
        kw.get("entity_id") or "",
        kw.get("service") or "",
        kw.get("attributes") or {},
    ),
    "ha_query": lambda **kw: ha_query(kw.get("entity_id") or ""),
    "set_timer": lambda **kw: set_timer(int(kw.get("seconds") or 0), kw.get("label") or "timer"),
    "get_weather": lambda **kw: get_weather(),
    "memory_write": lambda **kw: memory_write(kw.get("bullet") or ""),
}

OPENAI_TOOLS: list[dict[str, Any]] = [
    {
        "type": "function",
        "function": {
            "name": "speak",
            "description": "Show a reply to the user. REQUIRED for every user-visible response.",
            "parameters": {
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "Exact words to show."},
                    "language": {"type": "string", "enum": ["en", "ru"], "default": "en"},
                },
                "required": ["text"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "ha_control",
            "description": "Call a Home Assistant service.",
            "parameters": {
                "type": "object",
                "properties": {
                    "entity_id": {"type": "string"},
                    "service": {"type": "string", "description": "e.g. turn_on, light.turn_on"},
                    "attributes": {"type": "object"},
                },
                "required": ["entity_id", "service"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "ha_query",
            "description": "Read a Home Assistant entity state.",
            "parameters": {
                "type": "object",
                "properties": {"entity_id": {"type": "string"}},
                "required": ["entity_id"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "set_timer",
            "description": "Set a timer in seconds. Jarvis will speak when it fires.",
            "parameters": {
                "type": "object",
                "properties": {
                    "seconds": {"type": "integer"},
                    "label": {"type": "string"},
                },
                "required": ["seconds"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "get_weather",
            "description": "Current weather for the configured location.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "memory_write",
            "description": "Append one short memory bullet to CLAUDE.md.",
            "parameters": {
                "type": "object",
                "properties": {"bullet": {"type": "string"}},
                "required": ["bullet"],
            },
        },
    },
]


def dispatch_tool(name: str, arguments: dict) -> str:
    handler = TOOL_HANDLERS.get(name)
    if handler is None:
        return f"Unknown tool: {name}"
    try:
        return str(handler(**(arguments or {})))
    except Exception as exc:
        log_event({"type": "tool_error", "tool": name, "error": str(exc)})
        return f"Tool {name} failed: {exc}"
