"""Synthetic runtime transitions, persistence, and concurrent writers."""

from __future__ import annotations

import asyncio
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from aula_emulator.persistence import (
    JsonStateStore,
    PersistenceDurabilityError,
    PersistenceError,
    PersistenceLoadError,
)
from aula_emulator.runtime import SyntheticRuntime


class SyntheticRuntimeTest(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.state_file = Path(self._directory.name) / "state.json"

    def tearDown(self) -> None:
        self._directory.cleanup()

    def test_initial_snapshot_is_idle(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        self.assertEqual(runtime.snapshot(), {
            "kind": SyntheticRuntime.KIND, "revision": 0,
            "recording": "inactive", "streaming": "inactive"})
        self.assertFalse(self.state_file.exists())

    def test_changes_bump_revision_and_persist(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        self.assertEqual(runtime.set_recording(True)["revision"], 1)
        self.assertEqual(runtime.set_stream_active(True)["revision"], 2)
        self.assertEqual(SyntheticRuntime(self.state_file).snapshot(), {
            "kind": SyntheticRuntime.KIND, "revision": 2,
            "recording": "active", "streaming": "active"})

    def test_unchanged_value_is_not_a_new_revision(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        runtime.set_recording(False)
        self.assertEqual(runtime.snapshot()["revision"], 0)
        self.assertFalse(self.state_file.exists())

    def test_activity_and_boolean_validation(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        with self.assertRaises(ValueError):
            runtime.set_recording(1)  # type: ignore[arg-type]
        for field, value in (("stream_active", "active"), ("streaming", "invalid")):
            with self.assertRaises(ValueError):
                runtime.set_activity(field, value)  # type: ignore[arg-type]
        self.assertEqual(runtime.set_activity("recording", "paused")["recording"], "paused")
        self.assertEqual(runtime.set_activity("streaming", "unknown")["streaming"], "unknown")

    def test_stale_instances_preserve_updates_and_reload_before_noop(self) -> None:
        first = SyntheticRuntime(self.state_file)
        second = SyntheticRuntime(self.state_file)
        first.set_recording(True)
        self.assertEqual(second.set_stream_active(True)["revision"], 2)
        # first's stale stream value was inactive; stopping must still write.
        stopped = first.set_stream_active(False)
        self.assertEqual(stopped["revision"], 3)
        self.assertEqual(stopped["recording"], "active")
        self.assertEqual(second.snapshot(), stopped)

    def test_process_writers_preserve_updates_and_unique_revisions(self) -> None:
        script = """
import sys
from aula_emulator.runtime import SyntheticRuntime
runtime = SyntheticRuntime(sys.argv[1])
print('ready', flush=True)
input()
print(runtime.set_activity(sys.argv[2], 'active')['revision'], flush=True)
"""
        workers = [subprocess.Popen([sys.executable, "-c", script, str(self.state_file), field],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
                   for field in ("recording", "streaming")]
        try:
            for worker in workers:
                assert worker.stdout is not None
                self.assertEqual(worker.stdout.readline().strip(), "ready")
            for worker in workers:
                assert worker.stdin is not None
                worker.stdin.write("go\n")
                worker.stdin.flush()
            revisions: list[int] = []
            for worker in workers:
                output, errors = worker.communicate(timeout=10)
                self.assertEqual(worker.returncode, 0, errors)
                revisions.append(int(output.strip()))
            self.assertEqual(sorted(revisions), [1, 2])
            state = SyntheticRuntime(self.state_file).snapshot()
            self.assertEqual((state["recording"], state["streaming"]), ("active", "active"))
        finally:
            for worker in workers:
                if worker.poll() is None:
                    worker.kill()
                    worker.communicate()

    def test_v1_migration_is_lossless_and_preserves_revision(self) -> None:
        for recording in (True, False):
            for streaming in (True, False):
                JsonStateStore(self.state_file, schema_version=2).save({
                    "kind": "synthetic-local-state-v1", "revision": 7,
                    "recording": recording, "stream_active": streaming})
                runtime = SyntheticRuntime(self.state_file)
                self.assertEqual(runtime.snapshot(), {
                    "kind": SyntheticRuntime.KIND, "revision": 7,
                    "recording": "active" if recording else "inactive",
                    "streaming": "active" if streaming else "inactive"})
                runtime.reset()
                self.assertEqual(json.loads(self.state_file.read_text())["state"]["kind"], runtime.KIND)

    def test_corrupt_snapshot_is_refused_but_explicit_reset_recovers(self) -> None:
        self.state_file.write_text("{not json", encoding="utf-8")
        with self.assertRaises(PersistenceLoadError):
            SyntheticRuntime(self.state_file)
        self.assertEqual(SyntheticRuntime.reset_file(self.state_file)["revision"], 1)
        self.assertEqual(SyntheticRuntime.reset_file(self.state_file)["revision"], 2)

    def test_reset_idle_preserves_revision_and_bypasses_stale_memory(self) -> None:
        first = SyntheticRuntime(self.state_file)
        second = SyntheticRuntime(self.state_file)
        first.set_recording(True)
        self.assertEqual(second.reset()["revision"], 2)
        self.assertEqual(first.reset()["revision"], 3)

    def test_reset_cannot_overwrite_unsafe_targets(self) -> None:
        real = self.state_file.with_name("real.json")
        real.write_text("corrupt")
        self.state_file.symlink_to(real)
        with self.assertRaises(PersistenceError):
            SyntheticRuntime.reset_file(self.state_file)
        self.assertEqual(real.read_text(), "corrupt")
        self.state_file.unlink()
        os.mkfifo(self.state_file)
        with self.assertRaises(PersistenceError):
            SyntheticRuntime.reset_file(self.state_file)
        self.state_file.unlink()
        self.state_file.write_text("corrupt")
        self.state_file.chmod(0o666)
        with self.assertRaises(PersistenceError):
            SyntheticRuntime.reset_file(self.state_file)
        self.assertEqual(self.state_file.read_text(), "corrupt")

    def test_failed_save_preserves_memory_events_and_disk(self) -> None:
        async def exercise() -> None:
            runtime = SyntheticRuntime(self.state_file)
            before = runtime.set_recording(True)
            subscription = await runtime.events.subscribe()
            with patch("aula_emulator.persistence.os.replace", side_effect=OSError("disk failure")), self.assertRaises(PersistenceError):
                runtime.set_stream_active(True)
            self.assertEqual(runtime.snapshot(refresh=False), before)
            self.assertEqual(runtime.snapshot(), before)
            with self.assertRaises(asyncio.TimeoutError):
                await asyncio.wait_for(subscription.get(), timeout=0.01)
            await subscription.unsubscribe()
        asyncio.run(exercise())

    def test_lock_close_failures_are_normalized_including_after_commit(self) -> None:
        runtime = SyntheticRuntime(self.state_file)
        original_close = os.close
        lock_inode = self.state_file.with_name(self.state_file.name + ".lock").stat().st_ino

        def fail_lock_close(descriptor: int) -> None:
            is_lock = os.fstat(descriptor).st_ino == lock_inode
            original_close(descriptor)
            if is_lock:
                raise OSError("lock close failed")

        with (patch("aula_emulator.persistence.os.close", side_effect=fail_lock_close),
              self.assertRaises(PersistenceDurabilityError)):
            runtime.set_recording(True)
        self.assertEqual(runtime.snapshot(refresh=False)["recording"], "active")
        self.assertEqual(SyntheticRuntime(self.state_file).snapshot()["revision"], 1)
        with (patch("aula_emulator.persistence.os.close", side_effect=fail_lock_close),
              self.assertRaises(PersistenceError) as raised):
            runtime.snapshot()
        self.assertNotIsInstance(raised.exception, PersistenceDurabilityError)

    def test_directory_sync_failure_reports_committed_state(self) -> None:
        async def exercise() -> None:
            runtime = SyntheticRuntime(self.state_file)
            subscription = await runtime.events.subscribe()
            with patch.object(JsonStateStore, "_fsync_parent_directory", side_effect=OSError("sync failure")), self.assertRaises(PersistenceDurabilityError):
                runtime.set_recording(True)
            self.assertEqual(runtime.snapshot(refresh=False)["recording"], "active")
            self.assertEqual((await subscription.get()).revision, 1)
            self.assertEqual(SyntheticRuntime(self.state_file).snapshot()["revision"], 1)
        asyncio.run(exercise())


if __name__ == "__main__":
    unittest.main()
