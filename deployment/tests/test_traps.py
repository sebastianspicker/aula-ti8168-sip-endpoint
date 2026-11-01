"""A signal trap must end the script; cleanup belongs on EXIT (or 0) only."""
import re
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TRAP = re.compile(r"\btrap\s+('[^']*'|\"[^\"]*\"|\S+)\s+((?:[A-Za-z0-9]+[ \t]+)*[A-Za-z0-9]+)")
SIGNAL_NAMES = {"HUP", "INT", "TERM", "SIGHUP", "SIGINT", "SIGTERM", "1", "2", "15"}


def trap_sites():
    tracked = subprocess.run(["git", "ls-files", "*.sh", "tooling/live/*.py"], cwd=ROOT,
                             capture_output=True, text=True, check=True).stdout.split()
    for name in tracked:
        for match in TRAP.finditer((ROOT / name).read_text()):
            yield name, match.group(0), match.group(1).strip("'\""), match.group(2).split()


class TrapTest(unittest.TestCase):
    def test_signal_traps_exit(self):
        sites = list(trap_sites())
        self.assertTrue(sites)
        bad = [f"{name}: {text}" for name, text, action, signals in sites
               if action not in ("-", "") and SIGNAL_NAMES & set(signals) and not action.startswith("exit")]
        self.assertEqual(bad, [])

    def test_fixed_pattern_stops_on_sighup(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            script, ticks = tmp / "t.sh", tmp / "ticks"
            script.write_text(
                f"trap 'echo cleanup >> {tmp}/log' EXIT\n"
                "trap 'exit 129' HUP\ntrap 'exit 130' INT\ntrap 'exit 143' TERM\n"
                f"while :; do echo x >> {ticks}; sleep 0.05; done\n")
            process = subprocess.Popen(["sh", str(script)])
            try:
                while not ticks.exists():
                    time.sleep(0.02)
                process.send_signal(signal.SIGHUP)
                self.assertEqual(process.wait(timeout=10), 129)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
            count = ticks.read_text().count("x")
            time.sleep(0.3)
            self.assertEqual(ticks.read_text().count("x"), count)
            self.assertEqual((tmp / "log").read_text(), "cleanup\n")


if __name__ == "__main__":
    unittest.main()
