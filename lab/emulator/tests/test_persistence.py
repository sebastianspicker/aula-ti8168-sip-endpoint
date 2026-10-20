"""JsonStateStore round trip, validation limits and file safety checks."""

from __future__ import annotations

import json
import os
import stat
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from aula_emulator.persistence import (
    JsonStateStore,
    PersistenceError,
    PersistenceLoadError,
    PersistenceValidationError,
    SchemaVersionError,
)


class JsonStateStoreTest(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.path = Path(self._directory.name) / "state.json"

    def tearDown(self) -> None:
        self._directory.cleanup()

    def _write(self, content: bytes | str, mode: int = 0o600) -> None:
        data = content.encode("utf-8") if isinstance(content, str) else content
        self.path.write_bytes(data)
        os.chmod(self.path, mode)

    def test_round_trip_including_bytes(self) -> None:
        store = JsonStateStore(self.path)
        state = {"a": 1, "b": [True, None, 1.5, "x"], "c": {"d": b"\x00\xff"}}
        self.assertTrue(store.save(state))
        self.assertEqual(store.load(), state)

    def test_disabled_store(self) -> None:
        store = JsonStateStore(None)
        self.assertFalse(store.enabled)
        self.assertFalse(store.save({"a": 1}))
        self.assertIsNone(store.load())

    def test_missing_file_loads_none(self) -> None:
        self.assertIsNone(JsonStateStore(self.path).load())

    def test_saved_file_is_private_and_envelope_is_versioned(self) -> None:
        JsonStateStore(self.path, schema_version=2).save({"a": 1})
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode), 0o600)
        self.assertEqual(
            json.loads(self.path.read_text()),
            {"schema_version": 2, "state": {"a": 1}},
        )
        self.assertEqual(sorted(p.name for p in self.path.parent.iterdir()),
                         ["state.json", "state.json.lock"])

    def test_schema_version_mismatch(self) -> None:
        JsonStateStore(self.path, schema_version=1).save({"a": 1})
        with self.assertRaises(SchemaVersionError):
            JsonStateStore(self.path, schema_version=2).load()

    def test_invalid_constructor_arguments(self) -> None:
        for kwargs in ({"schema_version": 0}, {"schema_version": True},
                       {"max_depth": 0}, {"max_items": -1}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                JsonStateStore(self.path, **kwargs)  # type: ignore[arg-type]

    def test_corrupt_or_wrong_envelope_is_refused(self) -> None:
        cases = [
            b"{not json",
            b"\xff\xfe",
            b"[]",
            b'{"schema_version":1}',
            b'{"schema_version":1,"state":{},"extra":1}',
            b'{"schema_version":1,"state":[1]}',
            b'{"schema_version":1,"state":{"__aula_emulator_bytes__":"!!"}}',
        ]
        for content in cases:
            with self.subTest(content=content):
                self._write(content)
                with self.assertRaises(PersistenceLoadError):
                    JsonStateStore(self.path).load()

    def test_non_integer_schema_version_is_a_version_error(self) -> None:
        for version in ("1", True, 1.0):
            with self.subTest(version=version):
                self._write(json.dumps({"schema_version": version, "state": {}}))
                with self.assertRaises(SchemaVersionError):
                    JsonStateStore(self.path).load()

    def test_save_limits(self) -> None:
        cases = [
            (JsonStateStore(self.path, max_depth=2), {"a": {"b": {"c": 1}}}),
            (JsonStateStore(self.path, max_items=2), {"a": 1, "b": 2, "c": 3}),
            (JsonStateStore(self.path, max_string_bytes=3), {"a": "four"}),
            (JsonStateStore(self.path, max_string_bytes=3), {"four": 1}),
            (JsonStateStore(self.path, max_file_bytes=10), {"a": "x" * 20}),
            (JsonStateStore(self.path), {"a": float("nan")}),
            (JsonStateStore(self.path), {1: "non-string key"}),
            (JsonStateStore(self.path), {"a": object()}),
            (JsonStateStore(self.path), {"__aula_emulator_bytes__": "x"}),
        ]
        for store, state in cases:
            with self.subTest(state=state), self.assertRaises(PersistenceValidationError):
                store.save(state)  # type: ignore[arg-type]
        self.assertFalse(self.path.exists())

    def test_save_limits_accept_boundary_values(self) -> None:
        store = JsonStateStore(self.path, max_depth=2, max_items=3, max_string_bytes=4)
        state = {"a": {"b": "four"}, "c": 1}
        store.save(state)
        self.assertEqual(store.load(), state)

    def test_save_requires_mapping(self) -> None:
        with self.assertRaises(PersistenceValidationError):
            JsonStateStore(self.path).save([1])  # type: ignore[arg-type]

    def test_load_limits(self) -> None:
        JsonStateStore(self.path).save({"a": {"b": {"c": "long string"}}, "d": 1})
        for kwargs in ({"max_depth": 2}, {"max_items": 3},
                       {"max_string_bytes": 5}, {"max_file_bytes": 10}):
            with self.subTest(kwargs=kwargs), self.assertRaises(PersistenceLoadError):
                JsonStateStore(self.path, **kwargs).load()  # type: ignore[arg-type]

    def test_symlinked_snapshot_is_refused(self) -> None:
        real = Path(self._directory.name) / "real.json"
        JsonStateStore(real).save({"a": 1})
        self.path.symlink_to(real)
        with self.assertRaises(PersistenceLoadError):
            JsonStateStore(self.path).load()
        with self.assertRaises(PersistenceValidationError):
            JsonStateStore(self.path).save({"a": 2})
        self.assertEqual(JsonStateStore(real).load(), {"a": 1})

    def test_group_or_world_writable_snapshot_is_refused(self) -> None:
        for mode in (0o620, 0o602, 0o666):
            with self.subTest(mode=oct(mode)):
                self._write(json.dumps({"schema_version": 1, "state": {}}), mode)
                with self.assertRaises(PersistenceLoadError):
                    JsonStateStore(self.path).load()
                with self.assertRaises(PersistenceValidationError):
                    JsonStateStore(self.path).save({"a": 1})

    def test_readable_snapshot_mode_is_accepted(self) -> None:
        self._write(json.dumps({"schema_version": 1, "state": {"a": 1}}), 0o644)
        self.assertEqual(JsonStateStore(self.path).load(), {"a": 1})

    def test_non_regular_target_is_refused(self) -> None:
        self.path.mkdir()
        with self.assertRaises(PersistenceValidationError):
            JsonStateStore(self.path).save({"a": 1})
        with self.assertRaises(PersistenceLoadError):
            JsonStateStore(self.path).load()

    def test_save_creates_parent_directories(self) -> None:
        nested = Path(self._directory.name) / "x" / "y" / "state.json"
        self.assertTrue(JsonStateStore(nested).save({"a": 1}))
        self.assertEqual(JsonStateStore(nested).load(), {"a": 1})
        for directory in (nested.parent, nested.parent.parent):
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)

    def test_fifo_snapshot_and_unsafe_lock_are_refused_without_blocking(self) -> None:
        os.mkfifo(self.path)
        with self.assertRaises(PersistenceLoadError):
            JsonStateStore(self.path).load()
        self.path.unlink()
        lock = self.path.with_name(self.path.name + ".lock")
        for kind in ("fifo", "symlink", "writable"):
            with self.subTest(kind=kind):
                if kind == "fifo":
                    os.mkfifo(lock)
                elif kind == "symlink":
                    lock.symlink_to(self.path)
                else:
                    lock.write_text("")
                    lock.chmod(0o666)
                with self.assertRaises(PersistenceError):
                    JsonStateStore(self.path).save({"a": 1})
                self.assertFalse(self.path.exists())
                lock.unlink()

    def test_lock_inode_persists_between_transactions(self) -> None:
        store = JsonStateStore(self.path)
        store.save({"a": 1})
        lock = self.path.with_name(self.path.name + ".lock")
        inode = lock.stat().st_ino
        store.save({"a": 2})
        self.assertEqual(lock.stat().st_ino, inode)
        self.assertEqual(stat.S_IMODE(lock.stat().st_mode), 0o600)

    def test_wrong_owner_is_refused(self) -> None:
        self._write('{"schema_version":1,"state":{}}')
        with patch("aula_emulator.persistence.os.getuid", return_value=os.getuid() + 1):
            with self.assertRaises(PersistenceError):
                JsonStateStore(self.path).load()
            with self.assertRaises(PersistenceError):
                JsonStateStore(self.path).save({"a": 1})

    def test_io_errors_are_persistence_errors(self) -> None:
        for operation in ("os.open", "tempfile.mkstemp", "os.replace", "os.fsync"):
            with (self.subTest(operation=operation),
                  patch("aula_emulator.persistence." + operation, side_effect=OSError("failure")),
                  self.assertRaises(PersistenceError)):
                JsonStateStore(self.path).save({"a": 1})
        with patch("aula_emulator.persistence.os.open", side_effect=PermissionError("denied")), self.assertRaises(PersistenceError):
            JsonStateStore(self.path).load()

    def test_unpaired_surrogates_are_validation_errors_on_read_and_write(self) -> None:
        for state in ({"a": "\ud800"}, {"\ud800": "a"}):
            with self.subTest(state=ascii(state)):
                with self.assertRaises(PersistenceValidationError):
                    JsonStateStore(self.path).save(state)
                self._write(json.dumps({"schema_version": 1, "state": state}))
                with self.assertRaises(PersistenceLoadError):
                    JsonStateStore(self.path).load()


if __name__ == "__main__":
    unittest.main()
