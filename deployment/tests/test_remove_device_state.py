"""remove.sh --purge-device-state: validate $BASE/device before deleting anything."""
import subprocess
import tempfile
import unittest
from pathlib import Path

REMOVE = Path(__file__).resolve().parents[1] / "targets/ti8168/remove.sh"


def device_functions():
    text = REMOVE.read_text()
    start = text.index("validate_device_state() {")
    end = text.index("validate_runtime_state() {")
    return text[start:end]


def run_fragment(base, flag, call):
    script = base.parent / "run.sh"
    script.write_text(
        "set -eu\n"
        "die() { printf '%s\\n' \"$*\" >&2; exit 1; }\n"
        f"BASE={base}\nDEVICE_UID=$(id -u)\npurge_device={flag}\n" + device_functions() + call + "\n")
    return subprocess.run(["sh", str(script)], capture_output=True, text=True, timeout=60, check=False)


class DeviceStateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name) / "base"
        self.device = self.base / "device"
        self.device.mkdir(parents=True)
        self.device.chmod(0o700)
        for name in ("jobs.json", "oem-credentials.json", "service.log"):
            (self.device / name).write_text("x")

    def test_without_flag_fails_early_and_deletes_nothing(self):
        result = run_fragment(self.base, 0, "validate_device_state")
        self.assertEqual(result.returncode, 1)
        self.assertIn("--purge-device-state", result.stderr)
        self.assertEqual(len(list(self.device.iterdir())), 3)

    def test_flag_removes_known_files_and_directory(self):
        result = run_fragment(self.base, 1, "remove_device_state")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.device.exists())
        self.assertTrue(self.base.is_dir())

    def test_unexpected_entry_blocks_before_deleting(self):
        (self.device / "surprise").write_text("x")
        result = run_fragment(self.base, 1, "remove_device_state")
        self.assertEqual(result.returncode, 1)
        self.assertIn("unexpected object", result.stderr)
        self.assertEqual(len(list(self.device.iterdir())), 4)

    def test_symlink_entry_and_symlink_directory_are_rejected(self):
        (self.device / "jobs.pending").symlink_to(self.base)
        self.assertEqual(run_fragment(self.base, 1, "remove_device_state").returncode, 1)
        (self.device / "jobs.pending").unlink()
        real = self.base / "real"
        self.device.rename(real)
        self.device.symlink_to(real)
        result = run_fragment(self.base, 1, "remove_device_state")
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(list(real.iterdir())), 3)

    def test_wrong_mode_is_rejected(self):
        self.device.chmod(0o755)
        self.assertEqual(run_fragment(self.base, 1, "remove_device_state").returncode, 1)


if __name__ == "__main__":
    unittest.main()
