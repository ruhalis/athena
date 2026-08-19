"""Redis → WiFi WebSocket bridge for the ESP32 HUB75 matrix.

WiFi only. If the panel is unreachable, log the error. Do not open a serial port.
"""
from __future__ import annotations

import asyncio
import json
import logging
import sys
from datetime import datetime
from pathlib import Path

import websockets

sys.path.insert(0, str(Path(__file__).resolve().parent))
import jarvis_bus as bus  # noqa: E402
from jarvis_env import load_config, load_env  # noqa: E402

load_env()
log = logging.getLogger("jarvis.display")


def _frame_for_state(state: str) -> dict:
    if state == "PROCESSING":
        return {"mode": "think"}
    now = datetime.now().strftime("%H:%M")
    return {"mode": "clock", "t": now}


async def _send(url: str, payload: dict) -> None:
    try:
        async with websockets.connect(url, open_timeout=3, close_timeout=1) as ws:
            await ws.send(json.dumps(payload))
    except Exception as exc:
        log.error("matrix unreachable at %s: %s", url, exc)


async def run() -> int:
    cfg = load_config()
    display = cfg.get("display") or {}
    if not display.get("enabled", True):
        print("[display] disabled in config", file=sys.stderr)
        await asyncio.Event().wait()
        return 0
    if (display.get("transport") or "wifi") != "wifi":
        print("[display] transport must be wifi — refusing to start", file=sys.stderr)
        return 2
    url = display.get("url") or "ws://jarvis-matrix.local/ws"
    client = bus.get_client()
    print(f"[display] forwarding state_change to {url}", file=sys.stderr)
    async for chan, payload in bus.subscribe(client, bus.CH_STATE, bus.CH_DISPLAY):
        if chan == bus.CH_STATE:
            frame = _frame_for_state(payload.get("to") or "IDLE")
        else:
            frame = payload
        await _send(url, frame)
    return 0


def main() -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    return asyncio.run(run())


if __name__ == "__main__":
    raise SystemExit(main())
