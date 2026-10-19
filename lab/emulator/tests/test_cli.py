"""Command line entry point: .work confinement and status output."""

from __future__ import annotations

import contextlib
import io
import json
import shutil
import tempfile
import unittest
from pathlib import Path

from aula_emulator.__main__ import main

REPO_ROOT = Path(__file__).resolve().parents[3]
CACHE_ROOT = REPO_ROOT / ".work" / "cache"


class CliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._outside = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self._outside.cleanup()

    def test_state_file_outside_work_is_rejected(self) -> None:
        state_file = Path(self._outside.name) / "state.json"
        with contextlib.redirect_stderr(io.StringIO()) as stderr:
            with self.assertRaises(SystemExit) as raised:
                main(["status", "--state-file", str(state_file)])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("below the repository .work directory", stderr.getvalue())
        self.assertFalse(state_file.exists())

    def test_state_file_is_required(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                main(["status"])
        self.assertEqual(raised.exception.code, 2)

    def test_unknown_action_is_rejected(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                main(["explode", "--state-file", "x"])
        self.assertEqual(raised.exception.code, 2)


class CliWorkDirectoryTest(unittest.TestCase):
    def setUp(self) -> None:
        CACHE_ROOT.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(dir=CACHE_ROOT))
        self.state_file = self.directory / "state.json"

    def tearDown(self) -> None:
        resolved = self.directory.resolve()
        # Only remove the single directory this test created under .work/cache.
        # An explicit check, not assert, so it also holds under python -O.
        if resolved.parent != CACHE_ROOT.resolve():
            raise RuntimeError(f"refusing to remove {resolved}")
        shutil.rmtree(resolved)

    def _run(self, action: str) -> tuple[int, dict[str, object]]:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = main([action, "--state-file", str(self.state_file)])
        return code, json.loads(stdout.getvalue())

    def test_record_start_then_status(self) -> None:
        code, started = self._run("record-start")
        self.assertEqual(code, 0)
        self.assertEqual(
            started,
            {"kind": started["kind"], "revision": 1,
             "recording": True, "stream_active": False},
        )
        code, status = self._run("status")
        self.assertEqual(code, 0)
        self.assertEqual(status, started)

    def test_stream_and_stop_actions(self) -> None:
        self._run("record-start")
        self._run("stream-start")
        _, stopped = self._run("record-stop")
        self.assertEqual(stopped["revision"], 3)
        self.assertFalse(stopped["recording"])
        self.assertTrue(stopped["stream_active"])
        _, stopped = self._run("stream-stop")
        self.assertEqual(stopped["revision"], 4)
        self.assertFalse(stopped["stream_active"])

    def test_status_does_not_create_state_file(self) -> None:
        _, status = self._run("status")
        self.assertEqual(status["revision"], 0)
        self.assertFalse(self.state_file.exists())


if __name__ == "__main__":
    unittest.main()
