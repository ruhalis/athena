"""State machine for the text loop.

States:
    IDLE        — waiting for typed input
    PROCESSING  — router / LLM / tools running

Transitions are published on `state_change`.
"""
from __future__ import annotations

import enum
import logging

import jarvis_bus as bus

log = logging.getLogger(__name__)


class State(str, enum.Enum):
    IDLE = "IDLE"
    PROCESSING = "PROCESSING"


_ALLOWED: dict[State, set[State]] = {
    State.IDLE: {State.PROCESSING},
    State.PROCESSING: {State.IDLE},
}


class StateMachine:
    def __init__(self, client, initial: State = State.IDLE) -> None:
        self._client = client
        self._state = initial

    @property
    def state(self) -> State:
        return self._state

    def can(self, target: State) -> bool:
        return target in _ALLOWED.get(self._state, set())

    async def transition(self, target: State, *, force: bool = False) -> bool:
        if self._state is target:
            return True
        if not force and not self.can(target):
            log.warning("rejected transition %s -> %s", self._state, target)
            return False
        prev, self._state = self._state, target
        await bus.publish(
            self._client,
            bus.CH_STATE,
            {"from": prev.value, "to": target.value},
        )
        log.info("state %s -> %s", prev.value, target.value)
        return True
