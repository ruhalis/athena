"""Connect the brain to extra MCP servers from mcp_config.json.

The `jarvis` server is skipped (that process is this assistant). Optional
servers such as Obsidian and Linear are skipped when their keys are missing.
"""
from __future__ import annotations

import json
import os
import re
import sys
from contextlib import AsyncExitStack
from pathlib import Path
from typing import Any

from mcp import types
from mcp.client.session_group import ClientSessionGroup, StreamableHttpParameters
from mcp.client.stdio import StdioServerParameters

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jarvis_env import jarvis_home, load_env  # noqa: E402

INTERNAL_SERVERS = {"jarvis"}
_VAR = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
_OPENAI_NAME = re.compile(r"[^a-zA-Z0-9_-]+")


def _expand(value: str) -> str:
    return _VAR.sub(lambda m: os.environ.get(m.group(1), m.group(0)), value)


def _unresolved(value: str) -> bool:
    return bool(_VAR.search(value))


def _sanitize_name(name: str) -> str:
    cleaned = _OPENAI_NAME.sub("_", name).strip("_") or "tool"
    return cleaned[:64]


def _component_name(name: str, server_info: types.Implementation) -> str:
    prefix = _sanitize_name(server_info.name or "mcp")
    raw = _sanitize_name(name)
    if raw.startswith(f"{prefix}_"):
        return raw[:64]
    return f"{prefix}_{raw}"[:64]


def _stdio_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not v.startswith("()")}
    if extra:
        env.update(extra)
    return env


def _schema(tool: types.Tool) -> dict[str, Any]:
    raw = tool.inputSchema
    if raw is None:
        return {"type": "object", "properties": {}}
    if isinstance(raw, dict):
        return raw
    dump = getattr(raw, "model_dump", None)
    if callable(dump):
        return dump()
    return {"type": "object", "properties": {}}


def _result_text(result: types.CallToolResult) -> str:
    chunks: list[str] = []
    for block in result.content or []:
        text = getattr(block, "text", None)
        if text:
            chunks.append(text)
        else:
            chunks.append(str(block))
    body = "\n".join(chunks).strip() or "(empty)"
    if result.isError:
        return f"MCP error: {body}"
    return body


class McpHub:
    def __init__(self) -> None:
        self._stack = AsyncExitStack()
        self._group: ClientSessionGroup | None = None

    async def __aenter__(self) -> McpHub:
        load_env()
        await self._stack.__aenter__()
        self._group = await self._stack.enter_async_context(
            ClientSessionGroup(component_name_hook=_component_name)
        )
        await self._connect_configured()
        return self

    async def __aexit__(self, *exc: Any) -> None:
        await self._stack.__aexit__(*exc)
        self._group = None

    @property
    def tool_names(self) -> set[str]:
        if self._group is None:
            return set()
        return set(self._group.tools)

    def has_tool(self, name: str) -> bool:
        return name in self.tool_names

    def openai_tools(self) -> list[dict[str, Any]]:
        if self._group is None:
            return []
        out: list[dict[str, Any]] = []
        for name, tool in self._group.tools.items():
            out.append(
                {
                    "type": "function",
                    "function": {
                        "name": name,
                        "description": tool.description or name,
                        "parameters": _schema(tool),
                    },
                }
            )
        return out

    async def call(self, name: str, arguments: dict[str, Any] | None = None) -> str:
        if self._group is None or name not in self._group.tools:
            return f"Unknown tool: {name}"
        try:
            result = await self._group.call_tool(name, arguments or {})
        except Exception as exc:
            return f"MCP tool {name} failed: {exc}"
        return _result_text(result)

    async def _connect_configured(self) -> None:
        group = self._group
        if group is None:
            return
        path = jarvis_home() / "mcp_config.json"
        if not path.exists():
            return
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as exc:
            print(f"[mcp] failed to read {path}: {exc}", file=sys.stderr)
            return
        servers = data.get("mcpServers") or {}
        for name, spec in servers.items():
            if name in INTERNAL_SERVERS or not isinstance(spec, dict):
                continue
            try:
                params = self._server_params(name, spec)
            except _SkipServer as exc:
                print(f"[mcp] skip {name}: {exc}", file=sys.stderr)
                continue
            try:
                await group.connect_to_server(params)
            except Exception as exc:
                print(f"[mcp] {name} failed to connect ({exc})", file=sys.stderr)
                continue
            print(f"[mcp] connected {name}", file=sys.stderr)
        n = len(group.tools)
        if n:
            print(f"[mcp] {n} extra tools ready", file=sys.stderr)

    def _server_params(self, name: str, spec: dict) -> StdioServerParameters | StreamableHttpParameters:
        url = spec.get("url")
        if url:
            url = _expand(str(url))
            if _unresolved(url):
                raise _SkipServer("unresolved ${VAR} in url")
            token = os.environ.get("LINEAR_API_KEY", "").strip()
            if "linear.app" in url and not token:
                raise _SkipServer("LINEAR_API_KEY not set")
            headers = {}
            if token:
                headers["Authorization"] = f"Bearer {token}"
            return StreamableHttpParameters(url=url, headers=headers or None)

        command = spec.get("command")
        if not command:
            raise _SkipServer("no command or url")
        command = _expand(str(command))
        args = [_expand(str(a)) for a in (spec.get("args") or [])]
        env_spec = spec.get("env") or {}
        env = {str(k): _expand(str(v)) for k, v in env_spec.items()}
        blob = " ".join([command, *args, *env.values()])
        if _unresolved(blob):
            raise _SkipServer("unresolved ${VAR} in command/args/env")
        if name == "obsidian" and not os.environ.get("OBSIDIAN_API_KEY", "").strip():
            raise _SkipServer("OBSIDIAN_API_KEY not set")
        if name == "linear" and not os.environ.get("LINEAR_API_KEY", "").strip():
            raise _SkipServer("LINEAR_API_KEY not set")
        return StdioServerParameters(command=command, args=args, env=_stdio_env(env))


class _SkipServer(Exception):
    pass
