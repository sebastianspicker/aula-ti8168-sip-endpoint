"""Private output CLI boundaries, using only generated trees and absent inputs."""
from __future__ import annotations

import hashlib
import json
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from private_output import prepare_output

SOURCE = Path(__file__).resolve().parents[1]


class PrivateOutputTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.repository = self.base / "Checkout"
        self.repository.mkdir()

    def output(self, value: Path, *, container: bool = False) -> Path:
        return prepare_output(value, repository=self.repository, container=container)

    def test_creates_private_parents_and_leaves_fresh_builder_leaf_absent(self) -> None:
        target = self.repository / ".work/private/nested/archive"
        self.assertEqual(self.output(target), target)
        for directory in (target.parent, target.parent.parent, self.repository / ".work"):
            self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
        self.assertFalse(target.exists())

    def test_shared_work_root_may_be_readable_but_private_parents_must_be_0700(self) -> None:
        work = self.repository / ".work"
        work.mkdir(mode=0o755)
        work.chmod(0o755)
        self.output(work / "private/archive")
        self.assertEqual(stat.S_IMODE(work.stat().st_mode), 0o755)
        public = work / "public"
        public.mkdir(mode=0o755)
        public.chmod(0o755)
        with self.assertRaisesRegex(ValueError, "private directories"):
            self.output(public / "archive")
        self.assertFalse((public / "archive").exists())

    def test_rejects_public_outside_work_and_traversal_paths(self) -> None:
        for target in (self.repository / "lab/archive", self.base / "outside",
                       self.repository / ".work", self.repository / ".work/../leak"):
            with self.subTest(target=target), self.assertRaises(ValueError):
                self.output(target)
        self.assertFalse((self.repository / ".work").exists())

    def test_rejects_work_symlink_even_when_its_target_is_inside_checkout(self) -> None:
        (self.repository / "storage").mkdir()
        (self.repository / ".work").symlink_to("storage", target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.output(self.repository / ".work/archive")
        self.assertEqual(list((self.repository / "storage").iterdir()), [])

    def test_rejects_inner_aliases_into_work_and_escaping_work(self) -> None:
        work = self.repository / ".work"
        work.mkdir(mode=0o700)
        inside = work / "private"
        inside.mkdir(mode=0o700)
        for target in (inside, self.base):
            link = work / "alias"
            link.symlink_to(target, target_is_directory=True)
            with self.subTest(target=target), self.assertRaisesRegex(ValueError, "symlink"):
                self.output(link / "archive")
            link.unlink()

    def test_rejects_alias_of_checkout_itself(self) -> None:
        alias = self.base / "alias"
        alias.symlink_to(self.repository, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.output(alias / ".work/archive")
        self.assertFalse((self.repository / ".work").exists())

    def test_accepts_filesystem_case_equivalent_checkout_spelling(self) -> None:
        alternate = self.repository.with_name("checkout")
        if not alternate.exists() or not alternate.samefile(self.repository):
            self.skipTest("filesystem is case-sensitive")
        self.assertEqual(self.output(alternate / ".work/private/archive"),
                         self.repository / ".work/private/archive")

    def test_never_overwrites_existing_leaf_or_follows_final_symlink(self) -> None:
        target = self.repository / ".work/private/archive"
        self.output(target)
        target.write_bytes(b"keep")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.output(target)
        self.assertEqual(target.read_bytes(), b"keep")
        target.unlink()
        target.mkdir(mode=0o700)
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.output(target)
        target.rmdir()
        target.symlink_to(self.base / "absent")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.output(target)

    def test_runner_reuses_private_container_without_overwriting_evidence(self) -> None:
        target = self.repository / ".work/private/runs"
        self.output(target, container=True)
        (target / "prior.json").write_bytes(b"keep")
        self.assertEqual(self.output(target, container=True), target)
        self.assertEqual((target / "prior.json").read_bytes(), b"keep")
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o700)

    def test_rejects_writable_work_root_and_non_directory_parent(self) -> None:
        work = self.repository / ".work"
        work.mkdir(mode=0o700)
        work.chmod(0o777)
        with self.assertRaisesRegex(ValueError, "private directories"):
            self.output(work / "private/archive")
        work.chmod(0o700)
        (work / "file").write_bytes(b"keep")
        with self.assertRaises(OSError):
            self.output(work / "file/archive")

    def cli(self, name: str, output: Path) -> subprocess.CompletedProcess[str]:
        scripts = self.repository / "lab/qemu/firmware"
        scripts.mkdir(parents=True, exist_ok=True)
        for source in SOURCE.glob("*.py"):
            shutil.copyfile(source, scripts / source.name)
        absent = self.base / "absent-input.json"
        if name == "compose_nand.py":
            arguments = [str(absent), str(output)]
        else:
            arguments = ["--inputs", str(absent), "--output-dir", str(output)]
            if name == "run_original.py":
                arguments.extend(["--qemu", str(self.base / "absent-qemu"), "--append", "console=synthetic"])
        return subprocess.run([sys.executable, str(scripts / name), *arguments],
                              capture_output=True, text=True, check=False, timeout=5)

    def test_all_clis_reject_public_output_before_opening_absent_inputs(self) -> None:
        for name in ("rescue_initramfs.py", "run_original.py", "compose_nand.py"):
            with self.subTest(cli=name):
                result = self.cli(name, self.repository / "lab/private-output")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("private output must", result.stderr)
                self.assertNotIn("absent-input", result.stderr)
                self.assertNotIn("absent-qemu", result.stderr)
                self.assertFalse((self.repository / "lab/private-output").exists())

    def test_all_clis_accept_work_boundary_then_report_absent_input(self) -> None:
        for index, name in enumerate(("rescue_initramfs.py", "run_original.py", "compose_nand.py")):
            with self.subTest(cli=name):
                result = self.cli(name, self.repository / f".work/private/output-{index}")
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("private output must", result.stderr)
                self.assertIn("absent-", result.stderr)
                self.assertEqual(stat.S_IMODE((self.repository / ".work/private").stat().st_mode), 0o700)

    def test_rescue_cli_writes_only_generated_fixture_archive_below_work(self) -> None:
        # Install the CLI in a disposable checkout using the same helper as the
        # ordering tests; the absent-input invocation creates no archive.
        target = self.repository / ".work/private/rescue"
        self.cli("rescue_initramfs.py", target)
        runtime = self.base / "inputs/runtime"
        (runtime / "bin").mkdir(parents=True)
        elf = bytearray(52)
        elf[:7] = b"\x7fELF\x01\x01\x01"
        struct.pack_into("<H", elf, 18, 40)
        (runtime / "bin/busybox").write_bytes(elf)
        kernel = self.base / "inputs/kernel"
        kernel.write_bytes(b"generated-kernel-placeholder")
        manifest = self.base / "inputs/manifest.json"
        manifest.write_text(json.dumps({
            "kind": "aula-rescue-inputs-v1",
            "kernel": {"path": str(kernel), "sha256": hashlib.sha256(kernel.read_bytes()).hexdigest()},
            "runtime_root": str(runtime), "busybox": "bin/busybox",
            "files": [{"path": "bin/busybox", "kind": "file", "sha256": hashlib.sha256(elf).hexdigest()}],
        }))
        script = self.repository / "lab/qemu/firmware/rescue_initramfs.py"
        command = [sys.executable, str(script), "--inputs", str(manifest), "--output-dir", str(target)]
        result = subprocess.run(command, capture_output=True, text=True, check=False, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = target / "rescue.cpio"
        self.assertTrue(archive.read_bytes().startswith(b"070701"))
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(archive.stat().st_mode), 0o600)
        before = archive.read_bytes()
        manifest.unlink()
        repeated = subprocess.run(command, capture_output=True, text=True, check=False, timeout=5)
        self.assertNotEqual(repeated.returncode, 0)
        self.assertIn("already exists", repeated.stderr)
        self.assertEqual(archive.read_bytes(), before)

    def test_all_clis_reject_symlink_alias_before_opening_inputs(self) -> None:
        work = self.repository / ".work"
        work.mkdir(mode=0o700)
        (work / "escape").symlink_to(self.base, target_is_directory=True)
        for name in ("rescue_initramfs.py", "run_original.py", "compose_nand.py"):
            with self.subTest(cli=name):
                result = self.cli(name, work / "escape/output")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("symlink", result.stderr)
                self.assertNotIn("absent-input", result.stderr)
                self.assertFalse((self.base / "output").exists())


if __name__ == "__main__":
    unittest.main()
