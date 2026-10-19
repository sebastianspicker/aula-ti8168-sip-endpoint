"""Synthetic runtime state transitions and their persisted round trip."""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from aula_emulator.persistence import PersistenceLoadError
from aula_emulator.runtime import SyntheticRuntime


class SyntheticRuntimeTest(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.state_file = Path(self._directory.name) / "state.json"

    def tearDown(self) -> None:
        self._directory.cleanup()

    def test_initial_snapshot_is_idle(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        self.assertEqual(
            runtime.snapshot(),
            {"kind": SyntheticRuntime.KIND, "revision": 0,
             "recording": False, "stream_active": False},
        )
        self.assertFalse(self.state_file.exists())

    def test_changes_bump_revision_and_persist(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        self.assertEqual(runtime.set_recording(True)["revision"], 1)
        self.assertEqual(runtime.set_stream_active(True)["revision"], 2)
        reloaded = SyntheticRuntime(self.state_file).snapshot()
        self.assertEqual(reloaded["revision"], 2)
        self.assertTrue(reloaded["recording"])
        self.assertTrue(reloaded["stream_active"])

    def test_unchanged_value_is_not_a_new_revision(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        runtime.set_recording(False)
        self.assertEqual(runtime.snapshot()["revision"], 0)
        self.assertFalse(self.state_file.exists())

    def test_non_boolean_is_rejected(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        with self.assertRaises(ValueError):
            runtime.set_recording(1)  # type: ignore[arg-type]

    def test_corrupt_snapshot_is_refused(self) -> None:
        self.state_file.write_text("{not json", encoding="utf-8")
        with self.assertRaises(PersistenceLoadError):
            SyntheticRuntime(self.state_file)


if __name__ == "__main__":
    unittest.main()
