"""Validated, opt-in JSON snapshot persistence for emulator state.

This module deliberately does not know about the state it stores
(:class:`~aula_emulator.runtime.SyntheticRuntime` passes a plain mapping).
Callers provide a small mapping assembled from their state and reconstruct
that state after loading it.  A ``None`` path disables persistence, which is the default.
"""

from __future__ import annotations

import base64
import errno
import fcntl
import json
import math
import os
import stat
import tempfile
import threading
from collections.abc import Iterator, Mapping
from contextlib import contextmanager
from pathlib import Path
from typing import Any

_UNHANDLED = object()


class PersistenceError(Exception):
    """Base class for persistence failures."""


class PersistenceValidationError(PersistenceError, ValueError):
    """Raised when a value is outside the supported snapshot format."""


class PersistenceLoadError(PersistenceError):
    """Raised when an on-disk snapshot cannot be safely loaded."""


class PersistenceSafetyError(PersistenceLoadError):
    """An unsafe filesystem object must never be recovered or overwritten."""


class PersistenceDurabilityError(PersistenceError):
    """Replacement committed, but a final sync or lock release failed.

    The new state is visible. The message identifies the failed completion step;
    a directory-sync failure makes survival after a system crash uncertain.
    """

    committed = True


class SchemaVersionError(PersistenceLoadError):
    """Raised when a snapshot was written by an incompatible schema version."""


class JsonStateStore:
    """Atomically persist validated mappings to an optional JSON file.

    Bytes are represented by a tagged base64 object.  Other accepted values are
    ``None``, booleans, finite numbers, strings, lists, and mappings with string
    keys.  Limits are enforced on both writes and reads to avoid accepting an
    unexpectedly large or deeply nested state file.
    """

    _BYTES_TAG = "__aula_emulator_bytes__"

    def __init__(
        self,
        path: str | Path | None = None,
        *,
        schema_version: int = 1,
        max_file_bytes: int = 1_048_576,
        max_depth: int = 32,
        max_items: int = 10_000,
        max_string_bytes: int = 262_144,
    ) -> None:
        if not isinstance(schema_version, int) or isinstance(schema_version, bool) or schema_version < 1:
            raise ValueError("schema_version must be a positive integer")
        if any(
            not isinstance(limit, int) or isinstance(limit, bool) or limit < 1
            for limit in (max_file_bytes, max_depth, max_items, max_string_bytes)
        ):
            raise ValueError("persistence limits must be positive integers")
        self.path = Path(path) if path is not None else None
        self.schema_version = schema_version
        self.max_file_bytes = max_file_bytes
        self.max_depth = max_depth
        self.max_items = max_items
        self.max_string_bytes = max_string_bytes
        self._mutex = threading.RLock()
        self._lock_depth = 0
        self._transaction_committed = False

    @property
    def enabled(self) -> bool:
        """Whether this store has a configured target path."""

        return self.path is not None

    def _make_parents(self) -> None:
        assert self.path is not None
        missing: list[Path] = []
        parent = self.path.parent
        while not parent.exists():
            missing.append(parent)
            parent = parent.parent
        for directory in reversed(missing):
            try:
                directory.mkdir(mode=0o700)
            except FileExistsError:
                if not directory.is_dir():
                    raise

    @contextmanager
    def transaction(self) -> Iterator[None]:
        """Serialize reload/check/update/save with a persistent sibling flock.

        The lock inode is never unlinked, including after errors. Nested calls
        on this store are supported; separate stores/processes share the flock.
        """
        with self._mutex:
            if self.path is None or self._lock_depth:
                yield
                return
            descriptor: int | None = None
            self._transaction_committed = False
            try:
                self._make_parents()
                lock_path = self.path.with_name(self.path.name + ".lock")
                flags = os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK
                descriptor = os.open(lock_path, flags, 0o600)
                self._validate_file_status(os.fstat(descriptor), "lock")
                fcntl.flock(descriptor, fcntl.LOCK_EX)
                self._lock_depth += 1
                try:
                    yield
                finally:
                    self._lock_depth -= 1
            except OSError as error:
                raise PersistenceError("unable to acquire or use snapshot lock") from error
            finally:
                if descriptor is not None:
                    try:
                        os.close(descriptor)
                    except OSError as error:
                        if self._transaction_committed:
                            raise PersistenceDurabilityError(
                                "snapshot committed; releasing snapshot lock failed") from error
                        raise PersistenceError("unable to release snapshot lock") from error

    def save(self, state: Mapping[str, Any]) -> bool:
        """Atomically replace *state*, returning False when disabled.

        A directory-sync failure raises PersistenceDurabilityError after commit;
        other errors leave the old snapshot intact. Use transaction() around
        reload and save when performing a read-modify-write operation.
        """
        if not self.enabled:
            return False
        if not isinstance(state, Mapping):
            raise PersistenceValidationError("persisted state must be a mapping")
        document = {
            "schema_version": self.schema_version,
            "state": self._encode_value(dict(state), depth=0, item_count=[0]),
        }
        try:
            encoded = json.dumps(document, allow_nan=False, ensure_ascii=False,
                                 separators=(",", ":")).encode("utf-8")
        except (TypeError, ValueError, UnicodeError) as error:
            raise PersistenceValidationError("state is not JSON serializable") from error
        if len(encoded) > self.max_file_bytes:
            raise PersistenceValidationError("serialized state exceeds max_file_bytes")
        with self.transaction():
            return self._save_encoded(encoded)

    def _check_save_target(self) -> None:
        assert self.path is not None
        try:
            descriptor = self._open_snapshot(self.path)
            if descriptor is not None:
                try:
                    self._validate_file_status(os.fstat(descriptor), "snapshot")
                finally:
                    os.close(descriptor)
        except PersistenceLoadError as error:
            raise PersistenceValidationError(str(error)) from error

    def _save_encoded(self, encoded: bytes) -> bool:
        assert self.path is not None
        temporary_name: str | None = None
        try:
            self._check_save_target()
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{self.path.name}.", suffix=".tmp", dir=self.path.parent)
            with os.fdopen(descriptor, "wb") as temporary_file:
                temporary_file.write(encoded)
                temporary_file.flush()
                os.fsync(temporary_file.fileno())
            self._check_save_target()
            os.replace(temporary_name, self.path)
            self._transaction_committed = True
            temporary_name = None
            try:
                self._fsync_parent_directory()
            except OSError as error:
                raise PersistenceDurabilityError(
                    "snapshot committed; directory sync failed (durability uncertain)") from error
        except OSError as error:
            raise PersistenceError("unable to save snapshot") from error
        finally:
            if temporary_name is not None:
                try:
                    os.unlink(temporary_name)
                except FileNotFoundError:
                    pass
                except OSError as error:
                    raise PersistenceError("unable to remove temporary snapshot") from error
        return True

    def _fsync_parent_directory(self) -> None:
        assert self.path is not None
        flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
        directory = os.open(self.path.parent, flags)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    def load(self) -> dict[str, Any] | None:
        """Load a snapshot, or ``None`` when disabled or no snapshot exists."""

        if not self.enabled:
            return None
        assert self.path is not None
        raw = self._read_snapshot(self.path)
        if raw is None:
            return None
        document = self._parse_snapshot_document(raw)
        self._validate_envelope(document)
        state = self._decode_snapshot_state(document["state"])
        if not isinstance(state, dict):
            raise PersistenceLoadError("snapshot state must be a mapping")
        return state

    def _read_snapshot(self, path: Path) -> bytes | None:
        descriptor = self._open_snapshot(path)
        if descriptor is None:
            return None
        try:
            return self._read_open_snapshot(descriptor)
        except OSError as error:
            raise PersistenceError("unable to read snapshot") from error

    def _open_snapshot(self, path: Path) -> int | None:
        flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK
        try:
            return os.open(path, flags)
        except FileNotFoundError:
            return None
        except OSError as error:
            if error.errno == errno.ELOOP:
                raise PersistenceSafetyError("snapshot must be a regular non-symlink file") from error
            raise PersistenceError("unable to open snapshot") from error

    def _read_open_snapshot(self, descriptor: int) -> bytes:
        try:
            target_status = os.fstat(descriptor)
            self._validate_snapshot_status(target_status)
            with os.fdopen(descriptor, "rb") as snapshot_file:
                descriptor = -1
                raw = snapshot_file.read(self.max_file_bytes + 1)
        finally:
            if descriptor >= 0:
                os.close(descriptor)
        if len(raw) > self.max_file_bytes:
            raise PersistenceLoadError("snapshot exceeds max_file_bytes")
        return raw

    @staticmethod
    def _validate_file_status(target_status: os.stat_result, label: str) -> None:
        if not stat.S_ISREG(target_status.st_mode):
            raise PersistenceSafetyError(f"{label} must be a regular non-symlink file")
        if target_status.st_uid != os.getuid():
            raise PersistenceSafetyError(f"{label} must be owned by the current user")
        if target_status.st_mode & 0o022:
            raise PersistenceSafetyError(f"{label} must not be group/world writable")

    def _validate_snapshot_status(self, target_status: os.stat_result) -> None:
        self._validate_file_status(target_status, "snapshot")
        if target_status.st_size > self.max_file_bytes:
            raise PersistenceLoadError("snapshot exceeds max_file_bytes")

    @staticmethod
    def _parse_snapshot_document(raw: bytes | None) -> Any:
        if raw is None:
            return None
        try:
            return json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError, RecursionError, ValueError) as error:
            raise PersistenceLoadError("snapshot is not valid UTF-8 JSON") from error

    def _validate_envelope(self, document: Any) -> None:
        if not isinstance(document, dict) or set(document) != {"schema_version", "state"}:
            raise PersistenceLoadError("snapshot has an invalid envelope")
        disk_schema_version = document["schema_version"]
        if (
            not isinstance(disk_schema_version, int)
            or isinstance(disk_schema_version, bool)
            or disk_schema_version != self.schema_version
        ):
            raise SchemaVersionError("snapshot schema_version is not supported")

    def _decode_snapshot_state(self, value: Any) -> Any:
        try:
            return self._decode_value(value, depth=0, item_count=[0])
        except PersistenceValidationError as error:
            raise PersistenceLoadError("snapshot contains an invalid state value") from error

    def _encode_value(self, value: Any, *, depth: int, item_count: list[int]) -> Any:
        self._check_depth(depth)
        scalar = self._validated_scalar(value)
        if scalar is not _UNHANDLED:
            return scalar
        if isinstance(value, bytes):
            return {self._BYTES_TAG: base64.b64encode(value).decode("ascii")}
        if isinstance(value, list):
            encoded_list: list[Any] = []
            for item in value:
                self._count_item(item_count)
                encoded_list.append(self._encode_value(item, depth=depth + 1, item_count=item_count))
            return encoded_list
        if isinstance(value, Mapping):
            encoded: dict[str, Any] = {}
            for key, item in value.items():
                self._count_item(item_count)
                if not isinstance(key, str):
                    raise PersistenceValidationError("mapping keys must be strings")
                self._check_string(key)
                if key == self._BYTES_TAG:
                    raise PersistenceValidationError(f"{self._BYTES_TAG} is a reserved mapping key")
                encoded[key] = self._encode_value(item, depth=depth + 1, item_count=item_count)
            return encoded
        raise PersistenceValidationError(f"unsupported state value: {type(value).__name__}")

    def _decode_value(self, value: Any, *, depth: int, item_count: list[int]) -> Any:
        self._check_depth(depth)
        scalar = self._validated_scalar(value)
        if scalar is not _UNHANDLED:
            return scalar
        if isinstance(value, list):
            decoded_list: list[Any] = []
            for item in value:
                self._count_item(item_count)
                decoded_list.append(self._decode_value(item, depth=depth + 1, item_count=item_count))
            return decoded_list
        if isinstance(value, dict):
            if self._BYTES_TAG in value:
                return self._decode_bytes(value)
            decoded: dict[str, Any] = {}
            for key, item in value.items():
                self._count_item(item_count)
                if not isinstance(key, str):
                    raise PersistenceValidationError("mapping keys must be strings")
                self._check_string(key)
                decoded[key] = self._decode_value(item, depth=depth + 1, item_count=item_count)
            return decoded
        raise PersistenceValidationError(f"unsupported JSON value: {type(value).__name__}")

    def _validated_scalar(self, value: Any) -> Any:
        if value is None or isinstance(value, (bool, int)):
            return value
        if isinstance(value, float):
            if not math.isfinite(value):
                raise PersistenceValidationError("floats must be finite")
            return value
        if isinstance(value, str):
            self._check_string(value)
            return value
        return _UNHANDLED

    def _decode_bytes(self, value: dict[Any, Any]) -> bytes:
        if set(value) != {self._BYTES_TAG} or not isinstance(value[self._BYTES_TAG], str):
            raise PersistenceValidationError("invalid bytes encoding")
        try:
            return base64.b64decode(value[self._BYTES_TAG], validate=True)
        except ValueError as error:
            raise PersistenceValidationError("invalid bytes encoding") from error

    def _check_depth(self, depth: int) -> None:
        if depth > self.max_depth:
            raise PersistenceValidationError("state exceeds max_depth")

    def _count_item(self, item_count: list[int]) -> None:
        item_count[0] += 1
        if item_count[0] > self.max_items:
            raise PersistenceValidationError("state exceeds max_items")

    def _check_string(self, value: str) -> None:
        try:
            length = len(value.encode("utf-8"))
        except UnicodeError as error:
            raise PersistenceValidationError("string is not valid UTF-8") from error
        if length > self.max_string_bytes:
            raise PersistenceValidationError("string exceeds max_string_bytes")
