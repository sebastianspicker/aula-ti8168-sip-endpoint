"""Synthetic activity state, shared safely by local development processes."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Literal, TypedDict, cast

from .events import EventBroker
from .persistence import (
    JsonStateStore,
    PersistenceDurabilityError,
    PersistenceLoadError,
    PersistenceSafetyError,
)

Activity = Literal["active", "inactive", "paused", "unknown"]
ActivityField = Literal["recording", "streaming"]
ACTIVITIES = ("active", "inactive", "paused", "unknown")


class Snapshot(TypedDict):
    kind: str
    revision: int
    recording: Activity
    streaming: Activity


@dataclass(frozen=True)
class StateEvent:
    revision: int
    field: str
    value: Activity


class SyntheticRuntime:
    """Owns only projected local state; it does not control any hardware.

    In-process events report mutations committed by this instance. snapshot()
    refreshes from disk by default; changes by other processes do not generate
    in-process events. Callers should compare snapshot revisions when polling.
    """

    KIND = "synthetic-local-state-v2"

    def __init__(self, state_file: str | Path | None = None) -> None:
        self._store = JsonStateStore(state_file, schema_version=2)
        self.events: EventBroker[StateEvent] = EventBroker()
        with self._store.transaction():
            self._state = self._load()

    @classmethod
    def _idle(cls, revision: int = 0) -> Snapshot:
        return {"kind": cls.KIND, "revision": revision,
                "recording": "inactive", "streaming": "inactive"}

    @classmethod
    def _validate(cls, value: dict[str, object]) -> Snapshot:
        revision = value.get("revision")
        if type(revision) is not int or cast(int, revision) < 0:
            raise PersistenceLoadError("snapshot revision must be a nonnegative integer")
        if (set(value) == {"kind", "revision", "recording", "stream_active"}
                and value.get("kind") == "synthetic-local-state-v1"
                and type(value.get("recording")) is bool
                and type(value.get("stream_active")) is bool):
            return {"kind": cls.KIND, "revision": cast(int, revision),
                    "recording": "active" if value["recording"] else "inactive",
                    "streaming": "active" if value["stream_active"] else "inactive"}
        if (set(value) != {"kind", "revision", "recording", "streaming"}
                or value.get("kind") != cls.KIND
                or value.get("recording") not in ACTIVITIES
                or value.get("streaming") not in ACTIVITIES):
            raise PersistenceLoadError("snapshot is not synthetic local state")
        return {"kind": cls.KIND, "revision": cast(int, revision),
                "recording": cast(Activity, value["recording"]),
                "streaming": cast(Activity, value["streaming"])}

    def _load(self) -> Snapshot:
        saved = self._store.load()
        return self._idle() if saved is None else self._validate(saved)

    def snapshot(self, refresh: bool = True) -> Snapshot:
        with self._store.transaction():
            if refresh and self._store.enabled:
                self._state = self._load()
            return self._state.copy()

    def set_recording(self, active: bool) -> Snapshot:
        if type(active) is not bool:
            raise ValueError("active must be a boolean")
        return self.set_activity("recording", "active" if active else "inactive")

    def set_stream_active(self, active: bool) -> Snapshot:
        if type(active) is not bool:
            raise ValueError("active must be a boolean")
        return self.set_activity("streaming", "active" if active else "inactive")

    def set_activity(self, field: ActivityField, value: Activity) -> Snapshot:
        if field not in ("recording", "streaming") or value not in ACTIVITIES:
            raise ValueError("activity requires recording/streaming and active/inactive/paused/unknown")
        with self._store.transaction():
            current = self._load() if self._store.enabled else self._state
            if current[field] == value:
                self._state = current
                return current.copy()
            next_state = current.copy()
            next_state[field] = value
            next_state["revision"] += 1
            return self._commit(next_state, [StateEvent(next_state["revision"], field, value)])

    def _commit(self, state: Snapshot, events: list[StateEvent]) -> Snapshot:
        try:
            self._store.save(state)
        except PersistenceDurabilityError:
            # rename succeeded: report the visible state even though the caller
            # must be told it may not survive a crash. Never pretend rollback.
            self._accept(state, events)
            raise
        self._accept(state, events)
        return state.copy()

    def _accept(self, state: Snapshot, events: list[StateEvent]) -> None:
        self._state = state
        for event in events:
            self.events.publish(event)

    def reset(self, *, recover: bool = False) -> Snapshot:
        """Write idle state at revision + 1, even when already idle.

        Explicit recovery discards malformed payloads at baseline revision 1;
        only a fully validated current or v1 snapshot has a trusted revision.
        Filesystem safety and I/O errors are never bypassed.
        """
        with self._store.transaction():
            try:
                current = self._load() if self._store.enabled else self._state
            except PersistenceSafetyError:
                raise
            except PersistenceLoadError:
                if not recover:
                    raise
                current = self._idle()
            state = self._idle(current["revision"] + 1)
            events = [StateEvent(state["revision"], field, "inactive")
                      for field in ("recording", "streaming")]
            return self._commit(state, events)

    @classmethod
    def reset_file(cls, state_file: str | Path) -> Snapshot:
        """Recover an opt-in state path without first loading its payload."""
        runtime = cls.__new__(cls)
        runtime._store = JsonStateStore(state_file, schema_version=2)
        runtime.events = EventBroker()
        runtime._state = cls._idle()
        return runtime.reset(recover=True)
