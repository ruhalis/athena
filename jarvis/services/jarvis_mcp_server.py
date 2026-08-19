"""MCP server — Jarvis tools over stdio for Hermes / other MCP clients."""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from jarvis_env import load_env  # noqa: E402
from jarvis_tools import get_weather as _get_weather  # noqa: E402
from jarvis_tools import ha_control as _ha_control
from jarvis_tools import ha_query as _ha_query
from jarvis_tools import memory_write as _memory_write
from jarvis_tools import set_timer as _set_timer
from jarvis_tools import speak as _speak
from mcp.server.fastmcp import FastMCP  # noqa: E402

load_env()
server = FastMCP("jarvis")


@server.tool()
def speak(text: str, language: str = "en") -> str:
    """Show a reply to the user. REQUIRED for every response. Never reply with bare text."""
    return _speak(text, language)


@server.tool()
def ha_control(entity_id: str, service: str, attributes: dict | None = None) -> str:
    """Call a Home Assistant service. Locks, alarms, and garage need a typed yes."""
    return _ha_control(entity_id, service, attributes or {})


@server.tool()
def ha_query(entity_id: str) -> str:
    """Read a Home Assistant entity state."""
    return _ha_query(entity_id)


@server.tool()
def set_timer(seconds: int, label: str = "timer") -> str:
    """Set a timer in seconds. Jarvis will reply in text when it fires."""
    return _set_timer(seconds, label)


@server.tool()
def get_weather() -> str:
    """Current weather for the configured location."""
    return _get_weather()


@server.tool()
def memory_write(bullet: str) -> str:
    """Append one short memory bullet to CLAUDE.md."""
    return _memory_write(bullet)


if __name__ == "__main__":
    server.run()
