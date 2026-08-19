"""Brain — Hermes-style tool loop. Default provider is ChatGPT (OpenAI API).

No automatic fallback. The selected provider is the only provider. If the
key is missing or the API errors, the turn fails.

Usage:

    python jarvis_brain.py --daemon
    python jarvis_brain.py "hello, introduce yourself briefly"
"""
from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any

from openai import AsyncOpenAI

sys.path.insert(0, str(Path(__file__).resolve().parent))
import jarvis_bus as bus  # noqa: E402
from jarvis_env import jarvis_home, load_config, load_env  # noqa: E402
from jarvis_tools import OPENAI_TOOLS, dispatch_tool, log_event, speak  # noqa: E402

load_env()

JARVIS_HOME = jarvis_home()
CLAUDE_MD = JARVIS_HOME / "CLAUDE.md"
SESSION_DIR = JARVIS_HOME / "sessions"
SESSION_DIR.mkdir(parents=True, exist_ok=True)

MAX_TOOL_ITERS = 8


def _session_path() -> Path:
    return SESSION_DIR / f"{dt.date.today().isoformat()}.json"


def _load_session() -> list[dict]:
    p = _session_path()
    if not p.exists():
        return []
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError) as exc:
        print(f"[brain] failed to load session ({exc}); starting fresh", file=sys.stderr)
        return []


def _save_session(messages: list[dict]) -> None:
    _session_path().write_text(
        json.dumps(messages, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )


def _load_system() -> str:
    if CLAUDE_MD.exists():
        return CLAUDE_MD.read_text(encoding="utf-8")
    return "You are Jarvis. Always respond using the `speak` tool."


def resolve_provider(cfg: dict) -> tuple[str, dict]:
    name = (os.environ.get("JARVIS_BRAIN_PROVIDER") or cfg.get("brain", {}).get("default_provider") or "openai").strip()
    providers = (cfg.get("brain") or {}).get("providers") or {}
    if name not in providers:
        raise SystemExit(f"[brain] unknown provider {name!r}. Set JARVIS_BRAIN_PROVIDER to one of: {', '.join(providers)}")
    return name, providers[name]


def require_credentials(name: str, spec: dict) -> None:
    if name == "openai":
        if not os.environ.get("OPENAI_API_KEY"):
            raise SystemExit("[brain] OPENAI_API_KEY not set")
    elif name == "anthropic":
        if not os.environ.get("ANTHROPIC_API_KEY"):
            raise SystemExit("[brain] ANTHROPIC_API_KEY not set")
    elif name == "local":
        if not spec.get("base_url"):
            raise SystemExit("[brain] local provider is missing base_url")
    else:
        raise SystemExit(f"[brain] unsupported provider {name!r}")


def _openai_client(name: str, spec: dict) -> AsyncOpenAI:
    if name == "openai":
        return AsyncOpenAI()
    if name == "local":
        return AsyncOpenAI(base_url=spec["base_url"], api_key=os.environ.get("OPENAI_API_KEY") or "local")
    raise SystemExit(f"[brain] unsupported provider {name!r}")


async def _run_openai_turn(client: AsyncOpenAI, model: str, user_text: str) -> None:
    system = _load_system()
    history = _load_session()
    messages: list[dict[str, Any]] = [{"role": "system", "content": system}, *history]
    messages.append({"role": "user", "content": user_text})
    log_event({"type": "user", "text": user_text, "model": model})

    spoke = False
    for _ in range(MAX_TOOL_ITERS):
        response = await client.chat.completions.create(
            model=model,
            messages=messages,
            tools=OPENAI_TOOLS,
            tool_choice="auto",
        )
        choice = response.choices[0]
        msg = choice.message
        usage = response.usage
        log_event(
            {
                "type": "usage",
                "model": model,
                "input_tokens": getattr(usage, "prompt_tokens", 0) or 0,
                "output_tokens": getattr(usage, "completion_tokens", 0) or 0,
                "finish_reason": choice.finish_reason,
            }
        )
        tool_calls = msg.tool_calls or []
        assistant_msg: dict[str, Any] = {
            "role": "assistant",
            "content": msg.content,
        }
        if tool_calls:
            assistant_msg["tool_calls"] = [
                {
                    "id": tc.id,
                    "type": "function",
                    "function": {"name": tc.function.name, "arguments": tc.function.arguments},
                }
                for tc in tool_calls
            ]
        messages.append(assistant_msg)

        if not tool_calls:
            break

        for tc in tool_calls:
            args = json.loads(tc.function.arguments or "{}")
            if tc.function.name == "speak":
                spoke = True
            result = dispatch_tool(tc.function.name, args)
            messages.append(
                {
                    "role": "tool",
                    "tool_call_id": tc.id,
                    "content": result,
                }
            )

        if choice.finish_reason == "stop" and spoke:
            break

    persist = [m for m in messages if m.get("role") != "system"]
    _save_session(persist)
    if not spoke:
        log_event({"type": "no_speak", "user_text": user_text})
        raise RuntimeError("model finished without calling speak")


def _hermes_cmd(spec: dict, user_text: str) -> list[str]:
    hermes = shutil.which("hermes")
    if not hermes:
        raise SystemExit("[brain] hermes is not installed")
    provider = spec.get("provider") or "openai-api"
    model = spec.get("model")
    cmd = [
        hermes,
        "chat",
        "--quiet",
        "-q",
        user_text,
        "--provider",
        provider,
    ]
    if model:
        cmd.extend(["--model", model])
    return cmd


def _anthropic_tools() -> list[dict[str, Any]]:
    out = []
    for item in OPENAI_TOOLS:
        fn = item["function"]
        out.append(
            {
                "name": fn["name"],
                "description": fn.get("description") or "",
                "input_schema": fn.get("parameters") or {"type": "object", "properties": {}},
            }
        )
    return out


async def _run_anthropic_turn(model: str, user_text: str) -> None:
    import httpx

    system = _load_system()
    history = [m for m in _load_session() if m.get("role") in ("user", "assistant")]
    messages: list[dict[str, Any]] = [*history, {"role": "user", "content": user_text}]
    log_event({"type": "user", "text": user_text, "model": model})
    spoke = False
    headers = {
        "x-api-key": os.environ["ANTHROPIC_API_KEY"],
        "anthropic-version": "2023-06-01",
        "content-type": "application/json",
    }
    async with httpx.AsyncClient(timeout=60.0) as http:
        for _ in range(MAX_TOOL_ITERS):
            body = {
                "model": model,
                "max_tokens": 1024,
                "system": system,
                "tools": _anthropic_tools(),
                "messages": messages,
            }
            r = await http.post("https://api.anthropic.com/v1/messages", headers=headers, json=body)
            r.raise_for_status()
            data = r.json()
            content = data.get("content") or []
            messages.append({"role": "assistant", "content": content})
            tool_uses = [b for b in content if b.get("type") == "tool_use"]
            if not tool_uses:
                break
            results = []
            for block in tool_uses:
                name = block.get("name") or ""
                args = block.get("input") or {}
                if name == "speak":
                    spoke = True
                result = dispatch_tool(name, args)
                results.append(
                    {
                        "type": "tool_result",
                        "tool_use_id": block.get("id"),
                        "content": result,
                    }
                )
            messages.append({"role": "user", "content": results})
    persist = [m for m in messages]
    _save_session(persist)
    if not spoke:
        log_event({"type": "no_speak", "user_text": user_text})
        raise RuntimeError("model finished without calling speak")


async def _run_hermes_turn(spec: dict, user_text: str) -> None:
    """Optional explicit Hermes CLI path (JARVIS_BRAIN_HARNESS=hermes)."""
    cmd = _hermes_cmd(spec, user_text)
    log_event({"type": "user", "text": user_text, "harness": "hermes", "cmd": cmd[0]})
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    stdout, stderr = await proc.communicate()
    if proc.returncode != 0:
        err = stderr.decode("utf-8", errors="replace")
        log_event({"type": "error", "where": "hermes", "error": err})
        raise RuntimeError(f"hermes exited {proc.returncode}: {err}")
    text = stdout.decode("utf-8", errors="replace").strip()
    if text:
        speak(text, "en")


async def handle_user_turn(user_text: str, redis_client) -> None:
    cfg = load_config()
    name, spec = resolve_provider(cfg)
    require_credentials(name, spec)
    model = os.environ.get("JARVIS_BRAIN_MODEL") or spec.get("model")
    harness = os.environ.get("JARVIS_BRAIN_HARNESS") or "openai"

    if redis_client is not None:
        await bus.publish(redis_client, bus.CH_BRAIN_STATE, {"provider": name, "model": model})

    try:
        if harness == "hermes":
            await _run_hermes_turn(spec, user_text)
        elif name == "anthropic":
            await _run_anthropic_turn(model, user_text)
        else:
            client = _openai_client(name, spec)
            await _run_openai_turn(client, model, user_text)
    except Exception:
        if redis_client is not None:
            await bus.publish(redis_client, bus.CH_BRAIN_DONE, {"ok": False})
        raise
    if redis_client is not None:
        await bus.publish(redis_client, bus.CH_BRAIN_DONE, {"ok": True})


async def daemon() -> int:
    redis_client = bus.get_client()
    print("[brain] subscribed to llm_request", file=sys.stderr)
    async for _chan, payload in bus.subscribe(redis_client, bus.CH_LLM_REQUEST):
        text = (payload.get("text") or "").strip()
        if not text:
            continue
        try:
            await handle_user_turn(text, redis_client)
        except Exception as exc:
            log_event({"type": "error", "where": "brain", "error": str(exc)})
            print(f"[brain] error: {exc}", file=sys.stderr)
    return 0


async def one_shot(user_text: str) -> int:
    try:
        redis_client = bus.get_client()
        await redis_client.ping()
    except Exception:
        redis_client = None
        print("[brain] redis unavailable; speak will log only", file=sys.stderr)
    await handle_user_turn(user_text, redis_client)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Jarvis brain (ChatGPT / explicit provider)")
    parser.add_argument("--daemon", action="store_true")
    parser.add_argument("text", nargs="*", help="one-shot input text")
    args = parser.parse_args()

    cfg = load_config()
    name, spec = resolve_provider(cfg)
    require_credentials(name, spec)

    if args.daemon:
        return asyncio.run(daemon())
    if not args.text:
        parser.print_usage(sys.stderr)
        return 2
    return asyncio.run(one_shot(" ".join(args.text)))


if __name__ == "__main__":
    raise SystemExit(main())
