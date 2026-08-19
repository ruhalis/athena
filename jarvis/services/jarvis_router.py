"""Command router + routine engine (text-only).

Subscribes to `user_text`. Fuzzy-matches YAML routines or defers to the brain.
`speak` actions publish `assistant_reply`.
"""
from __future__ import annotations

import argparse
import asyncio
import logging
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml
from rapidfuzz import fuzz

sys.path.insert(0, str(Path(__file__).resolve().parent))
import jarvis_bus as bus  # noqa: E402
from jarvis_env import jarvis_home, load_config  # noqa: E402
from jarvis_state import State, StateMachine  # noqa: E402
from jarvis_tools import ha_control, log_event, speak  # noqa: E402
from safety_gate import clear_pending, get_pending, is_confirm_text  # noqa: E402

log = logging.getLogger("jarvis.router")
JARVIS_HOME = jarvis_home()


@dataclass
class Routine:
    name: str
    triggers: list[str]
    actions: list[dict]


def _load_yaml(path: Path) -> dict:
    if not path.exists():
        return {}
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def load_routines(paths: list[Path]) -> list[Routine]:
    out: list[Routine] = []
    for p in paths:
        data = _load_yaml(p)
        for i, item in enumerate(data.get("routines") or []):
            triggers = item.get("trigger") or []
            if isinstance(triggers, str):
                triggers = [triggers]
            out.append(
                Routine(
                    name=item.get("name") or f"{p.stem}_{i}",
                    triggers=[t.lower() for t in triggers],
                    actions=item.get("actions") or [],
                )
            )
    log.info("loaded %d routines", len(out))
    return out


def _word_bounded(text: str, trigger: str) -> bool:
    pattern = r"(?:^|\W)" + re.escape(trigger) + r"(?:$|\W)"
    return re.search(pattern, text, flags=re.UNICODE) is not None


def match_routine(text: str, routines: list[Routine], threshold: int) -> tuple[Routine, int] | None:
    if not text:
        return None
    norm = text.lower().strip()
    best: tuple[Routine, int] | None = None
    for r in routines:
        for trig in r.triggers:
            if len(trig.split()) == 1 and len(trig) <= 8:
                score = 100 if _word_bounded(norm, trig) else 0
            else:
                score = int(fuzz.partial_ratio(trig, norm))
            if score >= threshold and (best is None or score > best[1]):
                best = (r, score)
    return best


class Router:
    def __init__(self, client, routines: list[Routine], threshold: int) -> None:
        self._client = client
        self._routines = routines
        self._threshold = threshold
        self._sm = StateMachine(client)

    async def on_user_text(self, text: str, lang: str = "en") -> None:
        log_event({"type": "user", "text": text, "lang": lang})
        await self._sm.transition(State.PROCESSING)

        pending = get_pending()
        if pending and is_confirm_text(text):
            result = ha_control(pending["entity_id"], pending["service"], pending.get("attributes") or {})
            speak(result, "en")
            await self._sm.transition(State.IDLE)
            return
        if pending and not is_confirm_text(text):
            clear_pending()

        hit = match_routine(text, self._routines, self._threshold)
        if hit:
            routine, score = hit
            log_event({"type": "router_match", "routine": routine.name, "score": score})
            await self._execute_actions(routine.actions, user_text=text)
            await self._sm.transition(State.IDLE)
            return

        log_event({"type": "router_miss", "text": text})
        await bus.publish(self._client, bus.CH_LLM_REQUEST, {"text": text})

    async def on_brain_done(self) -> None:
        if self._sm.state is State.PROCESSING:
            await self._sm.transition(State.IDLE)

    async def _execute_actions(self, actions: list[dict], user_text: str) -> None:
        for action in actions:
            if not isinstance(action, dict) or len(action) != 1:
                log.warning("skipping malformed action: %r", action)
                continue
            ((kind, args),) = action.items()
            args = args or {}
            try:
                await self._run_action(kind, args, user_text)
            except Exception as exc:
                log_event({"type": "action_error", "kind": kind, "error": str(exc)})
                log.exception("action %s failed", kind)

    async def _run_action(self, kind: str, args: dict, user_text: str) -> None:
        if kind == "speak":
            speak(args.get("text") or "", args.get("language") or "en")
        elif kind == "ha_control":
            result = ha_control(
                args.get("entity_id") or "",
                args.get("service") or "",
                args.get("attributes") or {},
            )
            log.info("ha_control: %s", result)
        elif kind == "system":
            log_event({"type": "system", "command": args.get("command")})
        elif kind == "defer_to_llm":
            prompt = args.get("prompt") or user_text
            log_event({"type": "defer_to_llm", "prompt": prompt})
            await bus.publish(self._client, bus.CH_LLM_REQUEST, {"text": prompt})
        else:
            log.warning("unknown action kind: %s", kind)


async def run(config: dict) -> int:
    client = bus.get_client()
    routines_paths = [JARVIS_HOME / p for p in config["router"]["routines_paths"]]
    routines = load_routines(routines_paths)
    threshold = int(config["router"]["fuzzy_threshold"])
    router = Router(client, routines, threshold)
    channels = (bus.CH_USER_TEXT, bus.CH_BRAIN_DONE)
    print(f"[router] subscribed to {channels}", file=sys.stderr)
    async for chan, payload in bus.subscribe(client, *channels):
        try:
            if chan == bus.CH_USER_TEXT:
                await router.on_user_text(
                    (payload.get("text") or "").strip(),
                    payload.get("lang") or "en",
                )
            elif chan == bus.CH_BRAIN_DONE:
                await router.on_brain_done()
        except Exception as exc:
            log.exception("handler error on %s: %s", chan, exc)
    return 0


def main() -> int:
    logging.basicConfig(
        level=logging.INFO if not os.environ.get("JARVIS_DEBUG") else logging.DEBUG,
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
    )
    parser = argparse.ArgumentParser(description="Jarvis command router")
    parser.add_argument("--say", help="publish a user_text message and exit")
    args = parser.parse_args()
    cfg = load_config()

    if args.say:
        async def _once() -> int:
            client = bus.get_client()
            await bus.publish(client, bus.CH_USER_TEXT, {"text": args.say, "lang": "en"})
            await client.aclose()
            print(f"published user_text: {args.say!r}")
            return 0

        return asyncio.run(_once())

    return asyncio.run(run(cfg))


if __name__ == "__main__":
    raise SystemExit(main())
