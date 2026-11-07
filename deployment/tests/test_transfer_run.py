"""transfer-run.sh: the remote command must leave a first-install $BASE traversable (0755)."""
import re
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "targets/ti8168/transfer-run.sh"
REMOTE_BASE = "/var/lib/cbox/aula-ti8168-sip-endpoint"


def remote_setup():
    line = next(l for l in SCRIPT.read_text().splitlines() if l.startswith("umask 077; base="))
    return line[:line.index("; trap ")].replace("\\$", "$")


def mode(path):
    return stat.S_IMODE(path.stat().st_mode)


class TransferRunTest(unittest.TestCase):
    def run_setup(self, base):
        command = remote_setup().replace(REMOTE_BASE, str(base))
        # Never run the remote fragment (it has an rm -rf trap) against the real device path.
        assert REMOTE_BASE not in command and str(base) in command, "remote base was not redirected"
        return subprocess.run(["sh", "-c", command], capture_output=True, text=True, timeout=60, check=False)

    def test_first_install_creates_base_0755_and_incoming_0700(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "aula"
            result = self.run_setup(base)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(mode(base), 0o755)
            incoming = [p for p in base.iterdir() if p.name.startswith(".live-incoming-")]
            self.assertEqual([mode(p) for p in incoming], [0o700])

    def test_existing_base_mode_is_not_changed(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "aula"
            base.mkdir()
            base.chmod(0o750)
            self.assertEqual(self.run_setup(base).returncode, 0)
            self.assertEqual(mode(base), 0o750)

    def test_extract_does_not_carry_sender_owner(self):
        self.assertTrue(re.search(r"tar -xof - -C", SCRIPT.read_text()))


if __name__ == "__main__":
    unittest.main()
