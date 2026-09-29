"""Local synthetic recording and stream state for development checks."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import TypedDict

from .events import EventBroker
from .persistence import JsonStateStore, PersistenceLoadError


class Snapshot(TypedDict):
    kind: str
    revision: int
    recording: bool
    stream_active: bool


@dataclass(frozen=True)
class StateEvent:
    revision: int
    field: str
    value: bool


class SyntheticRuntime:
    """Owns only projected local state; it does not control any hardware."""

    KIND = "synthetic-local-state-v1"

    def __init__(self, state_file: str | Path | None = None) -> None:
        self._store = JsonStateStore(state_file, schema_version=2)
        self.events: EventBroker[StateEvent] = EventBroker()
        saved = self._store.load()
        if saved is None:
            self._state: Snapshot = {
                "kind": self.KIND,
                "revision": 0,
                "recording": False,
                "stream_active": False,
            }
        else:
            self._state = self._validate(saved)

    @classmethod
    def _validate(cls, value: dict[str, object]) -> Snapshot:
        if (set(value) != {"kind", "revision", "recording", "stream_active"}
                or value.get("kind") != cls.KIND
                or type(value.get("revision")) is not int
                or value["revision"] < 0
                or type(value.get("recording")) is not bool
                or type(value.get("stream_active")) is not bool):
            raise PersistenceLoadError("snapshot is not synthetic local state")
        return dict(value)  # type: ignore[return-value]

    def snapshot(self) -> Snapshot:
        return dict(self._state)

    def set_recording(self, active: bool) -> Snapshot:
        return self._set("recording", active)

    def set_stream_active(self, active: bool) -> Snapshot:
        return self._set("stream_active", active)

    def _set(self, field: str, active: bool) -> Snapshot:
        if type(active) is not bool:
            raise ValueError("active must be a boolean")
        if self._state[field] == active:
            return self.snapshot()
        next_state = self.snapshot()
        next_state[field] = active  # type: ignore[literal-required]
        next_state["revision"] += 1
        self._store.save(next_state)
        self._state = next_state
        self.events.publish(StateEvent(next_state["revision"], field, active))
        return self.snapshot()
