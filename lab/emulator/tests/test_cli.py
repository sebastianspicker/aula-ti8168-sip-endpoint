"""CLI confinement, cwd discovery, installed layout, and recovery."""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import aula_emulator
from aula_emulator.__main__ import discover_work_root, main


class CliTest(unittest.TestCase):
    def setUp(self) -> None:
        self._directory = tempfile.TemporaryDirectory()
        self.directory = Path(self._directory.name).resolve()
        self.work = self.directory / "work"
        self.state_file = self.work / "state.json"

    def tearDown(self) -> None:
        self._directory.cleanup()

    def _run(self, action: str) -> tuple[int, dict[str, object]]:
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout):
            code = main([action, "--state-file", str(self.state_file), "--work-root", str(self.work)])
        return code, json.loads(stdout.getvalue()) if stdout.getvalue() else {}

    def test_state_file_outside_work_is_rejected(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()) as stderr, self.assertRaises(SystemExit) as raised:
            main(["status", "--state-file", str(self.directory / "outside.json"), "--work-root", str(self.work)])
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("below the repository .work directory", stderr.getvalue())

    def test_missing_state_file_and_unknown_action_are_usage_errors(self) -> None:
        for argv in (["status"], ["explode", "--state-file", "x"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                main(argv)
            self.assertEqual(raised.exception.code, 2)

    def test_actions_and_status(self) -> None:
        self.assertEqual(self._run("record-start")[1]["revision"], 1)
        self.assertEqual(self._run("stream-start")[1]["revision"], 2)
        stopped = self._run("record-stop")[1]
        self.assertEqual((stopped["revision"], stopped["recording"], stopped["streaming"]),
                         (3, "inactive", "active"))
        self.assertEqual(self._run("record-pause")[1]["recording"], "paused")
        self.assertEqual(self._run("stream-unknown")[1]["streaming"], "unknown")
        self.assertEqual(self._run("status")[1]["revision"], 5)
        self.assertEqual(self._run("stream-stop")[1]["streaming"], "inactive")
        self.assertEqual(self._run("stream-pause")[1]["streaming"], "paused")
        self.assertEqual(self._run("record-unknown")[1]["recording"], "unknown")
        self.assertEqual(self._run("reset")[1]["revision"], 9)

    def test_status_does_not_create_snapshot(self) -> None:
        self.assertEqual(self._run("status")[1]["revision"], 0)
        self.assertFalse(self.state_file.exists())

    def test_corruption_exit_code_and_recovery_without_traceback(self) -> None:
        self.work.mkdir()
        self.state_file.write_text("bad JSON")
        with contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.assertEqual(self._run("status")[0], 3)
        self.assertNotIn("Traceback", stderr.getvalue())
        self.assertIn("aula-emulator:", stderr.getvalue())
        self.assertEqual(self._run("reset")[1]["revision"], 1)

    def test_symlink_escape_rejected_and_inside_symlink_not_followed(self) -> None:
        self.work.mkdir()
        outside = self.directory / "outside.json"
        self.state_file.symlink_to(outside)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            self._run("reset")
        self.assertFalse(outside.exists())
        self.state_file.unlink()
        real = self.work / "real.json"
        real.write_text("corrupt")
        self.state_file.symlink_to(real)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(self._run("reset")[0], 3)
        self.assertEqual(real.read_text(), "corrupt")

    def test_discovery_uses_nearest_marker_including_git_file(self) -> None:
        root = self.directory / "repo"
        nested = root / "sub" / "child"
        nested.mkdir(parents=True)
        (root / ".git").write_text("gitdir: ../actual")
        self.assertEqual(discover_work_root(nested), root / ".work")
        (root / "sub" / ".work").mkdir()
        self.assertEqual(discover_work_root(nested), root / "sub" / ".work")
        with patch("aula_emulator.__main__.Path.cwd", return_value=nested):
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(main(["status", "--state-file", str(root / "sub" / ".work" / "state")]), 0)

    def test_installed_layout_uses_cwd_instead_of_module_location(self) -> None:
        installed = self.directory / "site-packages"
        installed.mkdir()
        assert aula_emulator.__file__ is not None
        shutil.copytree(Path(aula_emulator.__file__).parent, installed / "aula_emulator")
        repository = self.directory / "checkout"
        cwd = repository / "deep" / "directory"
        cwd.mkdir(parents=True)
        (repository / ".git").write_text("gitdir: somewhere")
        target = repository / ".work" / "state.json"
        result = subprocess.run([sys.executable, "-m", "aula_emulator", "record-pause",
                                 "--state-file", str(target)], cwd=cwd,
                                env={**os.environ, "PYTHONPATH": str(installed)},
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["recording"], "paused")


if __name__ == "__main__":
    unittest.main()
