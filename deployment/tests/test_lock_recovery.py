"""A crash between mkdir and the pid write must not wedge the operation lock or autostart."""
import os
import tempfile
import time
import unittest
from pathlib import Path

from _fragments import PRELUDE, TI8168, function_text, run_sh

SCRIPTS = ["install.sh", "start.sh", "rollback.sh", "remove.sh"]


class LockRecoveryTest(unittest.TestCase):
    def acquire(self, script, prepare):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            lock = tmp / "lock"
            prepare(lock)
            result = run_sh(tmp / "run.sh", PRELUDE + f"LOCK={lock}\n"
                            + function_text(script, "acquire_lock") + "\n"
                            + function_text(script, "release_lock") + "\nacquire_lock\n")
            pid = (lock / "pid").read_text().strip() if (lock / "pid").exists() else None
            return result, pid

    def test_stale_empty_lock_is_reclaimed(self):
        def prepare(lock):
            lock.mkdir()
            old = time.time() - 120
            os.utime(lock, (old, old))
        for script in SCRIPTS:
            with self.subTest(script=script):
                result, pid = self.acquire(script, prepare)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(pid and pid.isdigit(), pid)

    def test_fresh_empty_lock_and_live_lock_are_kept(self):
        def fresh(lock):
            lock.mkdir()

        def live(lock):
            lock.mkdir()
            (lock / "pid").write_text(f"{os.getpid()}\n")
            old = time.time() - 120
            os.utime(lock, (old, old))
        for script in SCRIPTS:
            for prepare in (fresh, live):
                with self.subTest(script=script, case=prepare.__name__):
                    result, _ = self.acquire(script, prepare)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("another live operation is active", result.stderr)


class AutostartOnceTest(unittest.TestCase):
    def test_failed_start_is_attempted_once_per_boot(self):
        text = (TI8168 / "autostart.sh").read_text()
        tail = text[text.index('[ ! -e "$SENTINEL" ]'):]
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            starter = tmp / "starter.sh"
            starter.write_text(f'#!/bin/sh\n[ "$1" = start ] && echo x >> {tmp}/starts\nexit 1\n')
            starter.chmod(0o755)
            stubs = "".join(f"{n}() {{ :; }}\n" for n in (
                "validate_current", "safe_executable", "validated_preflight",
                "record_healthy_sentinel", "prepare_startup_log"))
            body = (f"set -eu\nSENTINEL={tmp}/ok\nATTEMPT={tmp}/attempt\nSTARTUP_LOG={tmp}/log\n"
                    f"STARTER={starter}\n{stubs}{tail}")
            for _ in range(3):
                run_sh(tmp / "run.sh", body)
            self.assertEqual((tmp / "starts").read_text(), "x\n")


if __name__ == "__main__":
    unittest.main()
