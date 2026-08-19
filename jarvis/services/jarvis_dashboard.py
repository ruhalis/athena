"""Text dashboard — chat box, live replies, provider badge."""
from __future__ import annotations

import asyncio
import json
import sys
from pathlib import Path

from fastapi import FastAPI, Form
from fastapi.responses import HTMLResponse, RedirectResponse, StreamingResponse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import jarvis_bus as bus  # noqa: E402
from jarvis_env import load_config, load_env  # noqa: E402
from jarvis_tools import log_event  # noqa: E402

load_env()
app = FastAPI(title="Jarvis")

PAGE = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8"/>
  <title>Jarvis</title>
  <style>
    body { font-family: ui-sans-serif, system-ui, sans-serif; margin: 0; background: #111; color: #eee; }
    header { display: flex; justify-content: space-between; align-items: center; padding: 1rem 1.5rem; border-bottom: 1px solid #333; }
    .badge { background: #1f6feb; color: white; padding: .2rem .6rem; border-radius: 999px; font-size: .8rem; letter-spacing: .04em; }
    main { max-width: 720px; margin: 0 auto; padding: 1.5rem; }
    #log { min-height: 50vh; white-space: pre-wrap; line-height: 1.45; }
    .user { color: #9cdcfe; }
    .jarvis { color: #ce9178; }
    .sys { color: #6a9955; }
    form { display: flex; gap: .5rem; margin-top: 1rem; }
    input[type=text] { flex: 1; padding: .7rem .8rem; border-radius: 8px; border: 1px solid #444; background: #1a1a1a; color: #eee; }
    button { padding: .7rem 1rem; border: 0; border-radius: 8px; background: #1f6feb; color: white; cursor: pointer; }
  </style>
</head>
<body>
  <header>
    <strong>Jarvis</strong>
    <span class="badge" id="badge">__PROVIDER__</span>
  </header>
  <main>
    <div id="log"></div>
    <form method="post" action="/chat">
      <input type="text" name="text" placeholder="Talk to Jarvis…" autofocus required/>
      <button type="submit">Send</button>
    </form>
  </main>
  <script>
    const log = document.getElementById('log');
    const es = new EventSource('/events');
    es.onmessage = (ev) => {
      const row = JSON.parse(ev.data);
      const div = document.createElement('div');
      div.className = row.cls;
      div.textContent = row.line;
      log.appendChild(div);
      log.scrollTop = log.scrollHeight;
    };
  </script>
</body>
</html>
"""

_subscribers: list[asyncio.Queue] = []


def _broadcast(cls: str, line: str) -> None:
    payload = json.dumps({"cls": cls, "line": line})
    for q in list(_subscribers):
        q.put_nowait(payload)


@app.get("/", response_class=HTMLResponse)
def index() -> str:
    cfg = load_config()
    name = cfg.get("brain", {}).get("default_provider", "openai").upper()
    return PAGE.replace("__PROVIDER__", name)


@app.post("/chat")
async def chat(text: str = Form(...)) -> RedirectResponse:
    text = (text or "").strip()
    if text:
        log_event({"type": "dashboard_user", "text": text})
        client = bus.get_client()
        await bus.publish(client, bus.CH_USER_TEXT, {"text": text, "lang": "en"})
        await client.aclose()
        _broadcast("user", f"You: {text}")
    return RedirectResponse("/", status_code=303)


@app.get("/events")
async def events() -> StreamingResponse:
    queue: asyncio.Queue = asyncio.Queue()
    _subscribers.append(queue)

    async def gen():
        try:
            while True:
                item = await queue.get()
                yield f"data: {item}\n\n"
        finally:
            if queue in _subscribers:
                _subscribers.remove(queue)

    return StreamingResponse(gen(), media_type="text/event-stream")


async def _bus_pump() -> None:
    client = bus.get_client()
    async for chan, payload in bus.subscribe(
        client, bus.CH_ASSISTANT_REPLY, bus.CH_STATE, bus.CH_BRAIN_STATE
    ):
        if chan == bus.CH_ASSISTANT_REPLY:
            _broadcast("jarvis", f"Jarvis: {payload.get('text') or ''}")
        elif chan == bus.CH_STATE:
            _broadcast("sys", f"state {payload.get('from')} → {payload.get('to')}")
        elif chan == bus.CH_BRAIN_STATE:
            _broadcast("sys", f"provider {payload.get('provider')} ({payload.get('model')})")


@app.on_event("startup")
async def _startup() -> None:
    asyncio.create_task(_bus_pump())


def main() -> None:
    import uvicorn

    cfg = load_config()
    dash = cfg.get("dashboard") or {}
    uvicorn.run(
        app,
        host=dash.get("host") or "127.0.0.1",
        port=int(dash.get("port") or 8088),
        reload=False,
    )


if __name__ == "__main__":
    main()
