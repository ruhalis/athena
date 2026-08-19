"""Redis channel names and a tiny async pub/sub helper."""
from __future__ import annotations

import json
import os
from typing import Any, AsyncIterator

import redis.asyncio as redis

CH_USER_TEXT = "user_text"
CH_LLM_REQUEST = "llm_request"
CH_ASSISTANT_REPLY = "assistant_reply"
CH_STATE = "state_change"
CH_TIMER_SET = "timer_set"
CH_BRAIN_STATE = "brain_state"
CH_BRAIN_DONE = "brain_done"
CH_DISPLAY = "display_command"

# Kept so old redis-cli habits still work; nothing in v3 consumes audio.
CH_TTS_REQUEST = CH_ASSISTANT_REPLY


def redis_url() -> str:
    return os.environ.get("REDIS_URL", "redis://127.0.0.1:6379/0")


def get_client() -> redis.Redis:
    return redis.from_url(redis_url(), decode_responses=True)


async def publish(client: redis.Redis, channel: str, payload: dict[str, Any]) -> None:
    await client.publish(channel, json.dumps(payload, ensure_ascii=False))


async def subscribe(client: redis.Redis, *channels: str) -> AsyncIterator[tuple[str, dict]]:
    pubsub = client.pubsub()
    await pubsub.subscribe(*channels)
    try:
        async for msg in pubsub.listen():
            if msg.get("type") != "message":
                continue
            chan = msg.get("channel")
            data = msg.get("data")
            try:
                payload = json.loads(data) if data else {}
            except json.JSONDecodeError:
                payload = {"_raw": data}
            yield chan, payload
    finally:
        await pubsub.unsubscribe(*channels)
        await pubsub.aclose()
