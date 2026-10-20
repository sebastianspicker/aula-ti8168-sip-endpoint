from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("arm_userspace", Path(__file__).parents[1] / "run.py")
assert SPEC is not None and SPEC.loader is not None
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)
SIP_SPEC = importlib.util.spec_from_file_location("arm_sip_smoke", Path(__file__).parents[1] / "sip_smoke.py")
assert SIP_SPEC is not None and SIP_SPEC.loader is not None
sip = importlib.util.module_from_spec(SIP_SPEC)
SIP_SPEC.loader.exec_module(sip)

ARM_ELF = """
Class: ELF32
Data: 2's complement, little endian
Machine: ARM
Flags: 0x5000200, Version5 EABI, soft-float ABI
Tag_CPU_arch: v7
[Requesting program interpreter: /lib/ld-linux.so.3]
"""


class RunnerTests(unittest.TestCase):
    def test_a_skipped_ctest_cannot_produce_a_passing_receipt(self) -> None:
        cases = "".join(f'<testcase name="{name}" status="run"/>' for name in runner.TESTS)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "ctest.xml"
            path.write_text(f'<testsuite tests="8" failures="0" skipped="0">{cases}</testsuite>')
            runner.require_ctest_results(path)
            path.write_text(f'<testsuite tests="8" failures="0" skipped="1">{cases}</testsuite>')
            with self.assertRaises(ValueError):
                runner.require_ctest_results(path)

    def test_lsz1_client_requires_correlated_success_reply(self) -> None:
        payload = b'{"ok":true}'
        with patch.object(sip.socket, "socket") as descriptor:
            connection = descriptor.return_value.__enter__.return_value
            connection.recv.return_value = sip.HEADER.pack(b"LSZ1", 1, 1, 1, 42, len(payload)) + payload
            self.assertEqual(sip.client("/unused.sock", 1, 42, "{}"), {"ok": True})
            for flags, request in ((3, 42), (1, 99)):
                connection.recv.return_value = sip.HEADER.pack(b"LSZ1", 1, 1, flags, request, len(payload)) + payload
                with self.assertRaises(ValueError):
                    sip.client("/unused.sock", 1, 42, "{}")
            connection.recv.return_value = b"LSZ1"
            with self.assertRaises(ValueError):
                sip.client("/unused.sock", 1, 42, "{}")

    def test_host_and_hard_float_artifacts_are_rejected(self) -> None:
        runner.require_arm_elf(ARM_ELF)
        for value in (
            ARM_ELF.replace("Machine: ARM", "Machine: AArch64"),
            ARM_ELF.replace("soft-float ABI", "hard-float ABI"),
            ARM_ELF + "Tag_ABI_VFP_args: VFP registers\n",
            ARM_ELF.replace("ld-linux.so.3", "ld-linux-armhf.so.3"),
            ARM_ELF.replace("Tag_CPU_arch: v7", "Tag_CPU_arch: v8"),
        ):
            with self.subTest(metadata=value), self.assertRaises(ValueError):
                runner.require_arm_elf(value)

    def test_output_traversal_existing_directories_and_symlinks_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            with patch.object(runner, "ROOT", root):
                output = runner.create_output(root / ".work" / "first")
                self.assertTrue(output.is_dir())
                with self.assertRaises(OSError):
                    runner.create_output(output)
                with self.assertRaises(ValueError):
                    runner.create_output(root / ".work" / ".." / "escape")
                (root / ".work" / "link").symlink_to(root, target_is_directory=True)
                with self.assertRaises(ValueError):
                    runner.create_output(root / ".work" / "link" / "escape")

    def test_missing_cross_tool_writes_failure_without_executing_host_tools(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            output = root / ".work" / "missing-tools"
            arguments = ["run.py", "--cross-prefix", "/no-such-arm-toolchain/arm-linux-gnueabi-",
                         "--qemu-arm", "qemu-arm", "--sysroot", "/no-such-sysroot",
                         "--output", str(output)]
            with patch.object(runner, "ROOT", root), patch("sys.argv", arguments), \
                    patch.object(runner.subprocess, "run") as execute:
                self.assertEqual(runner.main(), 1)
                execute.assert_not_called()
            report = json.loads((output / "report.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["commands"], [])
            self.assertIn("required executable is unavailable", report["error"])


if __name__ == "__main__":
    unittest.main()
