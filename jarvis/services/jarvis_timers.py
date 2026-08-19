"""Timer service — consumes `timer_set`, publishes `assistant_reply` on expiry."""
from __future__ import annotations

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import jarvis_bus as bus  # noqa: E402
from jarvis_env import load_env  # noqa: E402
from jarvis_tools import log_event, speak  # noqa: E402

load_env()


async def run() -> int:
    client = bus.get_client()
    print("[timers] subscribed to timer_set", file=sys.stderr)

    async def _fire(seconds: int, label: str) -> None:
        await asyncio.sleep(seconds)
        speak(f"Timer done: {label}.", "en")
        log_event({"type": "timer_fired", "label": label, "seconds": seconds})

    async for _chan, payload in bus.subscribe(client, bus.CH_TIMER_SET):
        seconds = int(payload.get("seconds") or 0)
        label = payload.get("label") or "timer"
        if seconds <= 0:
            continue
        asyncio.create_task(_fire(seconds, label))
    return 0


def main() -> int:
    return asyncio.run(run())


if __name__ == "__main__":
    raise SystemExit(main())
