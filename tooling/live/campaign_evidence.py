"""Private evidence publication and recovery journal support."""

from __future__ import annotations

import contextlib
import dataclasses
import fcntl
import hashlib
import json
import os
import re
import stat
import time
from pathlib import Path
from typing import Any

from campaign_types import CampaignError, ROOT, SECRET_PATTERN
from session import LiveSession

def _secure_directory(path: Path, *, create: bool = False, label: str) -> Path:
    """Create or validate a single private directory without following a link."""
    if create:
        try:
            path.mkdir(mode=0o700)
        except FileExistsError:
            pass
    try:
        info = path.lstat()
    except OSError as error:
        raise CampaignError(f"{label} is unavailable") from error
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.getuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise CampaignError(f"{label} must be an operator-owned private 0700 directory")
    return path


def _session_root(session: LiveSession) -> Path:
    work = ROOT / ".work"
    info = work.lstat()
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.getuid()
        or stat.S_IMODE(info.st_mode) & 0o022
    ):
        raise CampaignError(".work is unsafe")
    live_root = _secure_directory(work / "live", create=True, label="live evidence root")
    return _secure_directory(live_root / session.session_id, create=True, label="live session root")


def _private_root(session: LiveSession) -> Path:
    return _secure_directory(_session_root(session) / "private", create=True, label="private session evidence root")


@contextlib.contextmanager
def _session_lock(session: LiveSession):
    path = _session_root(session) / 'campaign.lock'
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        info = os.fstat(descriptor)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or
                stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1):
            raise CampaignError('campaign lock must be an owned private regular file')
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise CampaignError('another campaign phase is active for this session') from error
        yield
    finally:
        os.close(descriptor)


def _new_path(directory: Path, stem: str, suffix: str) -> tuple[Path, int]:
    previous = [int(path.name.split('-', 1)[0]) for path in directory.glob(f'*{suffix}')
                if path.name.split('-', 1)[0].isdigit()]
    timestamp = max(time.time_ns(), max(previous, default=0) + 1)
    for sequence in range(1000):
        candidate = directory / f"{timestamp}-{sequence:03d}-{stem}{suffix}"
        try:
            descriptor = os.open(
                candidate,
                os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0),
                0o600,
            )
        except FileExistsError:
            continue
        return candidate, descriptor
    raise CampaignError("cannot allocate a create-new result path")


def _fsync_directory(directory: Path) -> None:
    descriptor = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_CLOEXEC", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _write_json_new(directory: Path, stem: str, value: dict[str, Any]) -> Path:
    path, descriptor = _new_path(directory, stem, ".json")
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or stat.S_IMODE(info.st_mode) != 0o600
            or info.st_size != 0
            or info.st_uid != os.getuid()
            or info.st_nlink != 1
        ):
            raise CampaignError("result publication path is not create-new")
        rendered = (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("utf-8")
        view = memoryview(rendered)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise CampaignError("create-new result write did not complete")
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    _fsync_directory(directory)
    return path


@dataclasses.dataclass
class PhaseEvidence:
    action: str
    started_ns: int
    completed_ns: int = 0
    checks: dict[str, bool] = dataclasses.field(default_factory=dict)
    private_observations: dict[str, Any] = dataclasses.field(default_factory=dict)

    def sanitized(self, session: LiveSession) -> dict[str, Any]:
        return {
            "schema": "aula-ti8168-sip-endpoint-live-sanitized-phase-v1",
            "session_id_sha256": hashlib.sha256(session.session_id.encode()).hexdigest(),
            "action": self.action,
            "passed": bool(self.checks) and all(type(value) is bool and value for value in self.checks.values()),
            "checks": self.checks,
            "started_ns": self.started_ns,
            "completed_ns": self.completed_ns,
            "sensitive_values_included": False,
        }


def _publish(evidence: PhaseEvidence, session: LiveSession) -> tuple[Path, Path]:
    root = _private_root(session)
    evidence.completed_ns = time.time_ns()
    if not all(type(value) is bool for value in evidence.checks.values()):
        raise CampaignError("phase checks must be strict booleans")
    private = _write_json_new(root, evidence.action, dataclasses.asdict(evidence))
    sanitized_dir = _secure_directory(_session_root(session) / "sanitized", create=True, label="sanitized session evidence root")
    sanitized = evidence.sanitized(session)
    rendered = json.dumps(sanitized, sort_keys=True, indent=2) + "\n"
    if SECRET_PATTERN.search(rendered):
        raise CampaignError("sanitized result failed the secret-field gate")
    summary = _write_json_new(sanitized_dir, evidence.action, sanitized)
    return private, summary


def _journal_root(session: LiveSession) -> Path:
    return _secure_directory(_session_root(session) / "journal", create=True, label="campaign journal root")


def _record_journal(session: LiveSession, action: str, state: str, *, failure_type: str | None = None) -> Path:
    if state not in {"started", "completed", "failed", "interrupted"}:
        raise CampaignError("campaign journal state is invalid")
    record: dict[str, Any] = {
        "schema": "aula-ti8168-sip-endpoint-live-campaign-journal-v1",
        "action": action,
        "state": state,
        "recorded_ns": time.time_ns(),
    }
    if failure_type is not None:
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,63}", failure_type):
            raise CampaignError("campaign journal failure type is invalid")
        record["failure_type"] = failure_type
    return _write_json_new(_journal_root(session), f"{action}-{state}", record)


def _valid_journal_schema(value: Any, allowed: set[str], has_failure: bool) -> bool:
    return (
        isinstance(value, dict)
        and set(value) == allowed
        and value.get("schema") == "aula-ti8168-sip-endpoint-live-campaign-journal-v1"
        and isinstance(value.get("action"), str)
        and value.get("state") in {"started", "completed", "failed", "interrupted"}
        and type(value.get("recorded_ns")) is int
        and (not has_failure or isinstance(value.get("failure_type"), str))
    )


def _read_journal_record(record: Path) -> tuple[str, str]:
    info = record.lstat()
    if not stat.S_ISREG(info.st_mode) or stat.S_ISLNK(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o600:
        raise CampaignError("campaign journal contains an unsafe record")
    try:
        value = json.loads(record.read_bytes())
    except (OSError, ValueError, UnicodeDecodeError) as error:
        raise CampaignError("campaign journal record is malformed") from error
    allowed = {"schema", "action", "state", "recorded_ns"}
    has_failure = isinstance(value, dict) and "failure_type" in value
    if has_failure:
        allowed.add("failure_type")
    if not _valid_journal_schema(value, allowed, has_failure):
        raise CampaignError("campaign journal record has an inexact schema")
    return value["action"], value["state"]


def _reconcile_journal(session: LiveSession) -> None:
    root = _journal_root(session)
    latest: dict[str, str] = {}
    for record in sorted(root.glob("*.json")):
        action, state = _read_journal_record(record)
        latest[action] = state
    for action, state in latest.items():
        if state == "started":
            _record_journal(session, action, "interrupted")
            raise CampaignError("a previous campaign phase was interrupted; reconcile target state manually before continuing")
