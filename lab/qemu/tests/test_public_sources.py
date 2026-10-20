"""Generated fake-tree/index cases for the public-source artifact boundary."""
from __future__ import annotations

import copy
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / "scripts/check_public_sources.py"
spec = importlib.util.spec_from_file_location("public_sources", SCRIPT)
assert spec is not None and spec.loader is not None
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)
TARGET = "hw/arm/generated.c"
PATCH = (f"diff --git a/{TARGET} b/{TARGET}\nnew file mode 100644\n"
         f"--- /dev/null\n+++ b/{TARGET}\n@@ -0,0 +1 @@\n+/* Generated public test. */\n")
BLOB = "a" * 40


class InventoryTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.files = {
            "lab/qemu/scripts/generated.py": ("authored-source", b"print('generated')\n"),
            "lab/qemu/README.md": ("doc", b"Generated documentation.\n"),
            "lab/arm-userspace/public-lock.json": ("public-dependency-lock", b'{"public":"metadata"}\n'),
            "lab/qemu/patches/model.patch": ("patch", PATCH.encode()),
        }
        for name, (_kind, data) in self.files.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.manifest = {
            "schema": 1, "scopes": list(guard.SCOPES),
            "boundary": "Exact paths prevent artifacts; this is not a secret-content proof.",
            "files": [{"path": name, "class": kind, "provenance": "Generated repository-owned test input."}
                      for name, (kind, _data) in self.files.items()],
            "qemu_patch_targets": [TARGET], "retired_index_paths": [],
        }

    def check(self, manifest: dict[str, object] | None = None, **options: object) -> int:
        return guard.check(self.root, json.dumps(self.manifest if manifest is None else manifest).encode(),
                           index_paths=options.get("index_paths", {}),
                           untracked_paths=options.get("untracked_paths", list(self.files)),
                           index_content=options.get("index_content"))

    def test_exact_generated_sources_and_public_lock_pass(self) -> None:
        self.assertEqual(self.check(), 4)

    def test_unlisted_binary_json_and_archive_working_files_fail(self) -> None:
        for extension in ("bin", "json", "cpio"):
            path = f"lab/qemu/private.{extension}"
            (self.root / path).write_bytes(b"generated artifact")
            with self.subTest(extension=extension), self.assertRaisesRegex(ValueError, "unlisted nonignored"):
                self.check(untracked_paths=[*self.files, path])

    def test_binary_or_unclassified_json_cannot_be_added_as_source(self) -> None:
        for path in ("lab/qemu/kernel.bin", "lab/qemu/root.cpio", "lab/qemu/runtime.json"):
            manifest = copy.deepcopy(self.manifest)
            manifest["files"].append({"path": path, "class": "authored-source", "provenance": "Fixture"})
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "class/extension"):
                self.check(manifest)

    def test_private_and_noncanonical_inventory_paths_fail(self) -> None:
        for path in (".work/kernel.py", "lab/qemu/evidence/kernel.py", "lab/qemu/../kernel.py",
                     "/lab/qemu/kernel.py", "lab//qemu/kernel.py", "lab/qemu/./kernel.py", "lab\\qemu\\kernel.py"):
            manifest = copy.deepcopy(self.manifest)
            manifest["files"][0]["path"] = path
            with self.subTest(path=path), self.assertRaises(ValueError):
                self.check(manifest)

    def test_missing_class_provenance_and_duplicate_inventory_fail(self) -> None:
        for key in ("class", "provenance"):
            manifest = copy.deepcopy(self.manifest)
            del manifest["files"][0][key]
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.check(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["files"][0]["provenance"] = " "
        with self.assertRaisesRegex(ValueError, "missing provenance"):
            self.check(manifest)
        manifest = copy.deepcopy(self.manifest)
        manifest["files"].append(manifest["files"][0])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.check(manifest)

    def test_missing_inventory_and_missing_required_file_fail(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["files"].pop(0)
        with self.assertRaisesRegex(ValueError, "unlisted nonignored"):
            self.check(manifest)
        (self.root / "lab/qemu/README.md").unlink()
        with self.assertRaises(FileNotFoundError):
            self.check()

    def test_symlink_source_and_ancestor_fail(self) -> None:
        source = self.root / "lab/qemu/scripts/generated.py"
        source.unlink()
        source.symlink_to(self.root / "lab/qemu/README.md")
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.check()
        source.unlink()
        directory = source.parent
        directory.rmdir()
        alternate = self.root / "alternate"
        alternate.mkdir()
        (alternate / source.name).write_text("public source")
        directory.symlink_to(alternate, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, "symlink"):
            self.check()

    def test_fifo_is_rejected_without_blocking(self) -> None:
        path = self.root / "lab/qemu/README.md"
        path.unlink()
        os.mkfifo(path)
        code = ("import importlib.util,sys;from pathlib import Path;"
                "s=importlib.util.spec_from_file_location('guard',sys.argv[1]);"
                "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
                "m.regular(Path(sys.argv[2]),'lab/qemu/README.md')")
        result = subprocess.run([sys.executable, "-c", code, str(SCRIPT), str(self.root)],
                                capture_output=True, check=False, timeout=2)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"expected bounded regular source", result.stderr)

    def test_nul_working_source_and_staged_source_fail(self) -> None:
        path = "lab/qemu/scripts/generated.py"
        (self.root / path).write_bytes(b"public\0binary")
        with self.assertRaisesRegex(ValueError, "NUL/binary"):
            self.check()
        (self.root / path).write_bytes(b"public text")
        with self.assertRaisesRegex(ValueError, "NUL/binary"):
            self.check(index_paths={path: BLOB}, index_content=lambda _blob: b"staged\0binary")

    def test_forced_index_private_and_unlisted_scoped_paths_fail(self) -> None:
        for path in (".work/private.bin", "evidence/rootfs.json", "lab/qemu/forced.bin",
                     ".WORK/private.py", "lab/foo/Evidence/private.py", "other/.work/private.py"):
            with self.subTest(path=path), self.assertRaisesRegex(ValueError, "index"):
                self.check(index_paths={path: BLOB})

    def test_extra_overlay_target_or_mismatched_header_fails(self) -> None:
        patch = self.root / "lab/qemu/patches/model.patch"
        patch.write_text(PATCH + "diff --git a/hw/arm/private.c b/hw/arm/private.c\n")
        with self.assertRaisesRegex(ValueError, "unexpected/duplicate QEMU"):
            self.check()
        patch.write_text(PATCH.replace(f"+++ b/{TARGET}", "+++ b/hw/arm/private.c"))
        with self.assertRaisesRegex(ValueError, "disagrees"):
            self.check()

    def test_binary_patch_literal_and_duplicate_targets_fail(self) -> None:
        patch = self.root / "lab/qemu/patches/model.patch"
        for payload in ("GIT binary patch\nliteral 12\n", "literal 12\n", "Binary files a/x and b/x differ\n"):
            patch.write_text(PATCH + payload)
            with self.subTest(payload=payload), self.assertRaisesRegex(ValueError, "binary patch"):
                self.check()
        patch.write_text(PATCH + PATCH)
        with self.assertRaisesRegex(ValueError, "duplicate QEMU"):
            self.check()

    def test_nonregular_patch_modes_and_index_modes_fail(self) -> None:
        patch_file = self.root / "lab/qemu/patches/model.patch"
        for metadata in ("new file mode 120000", "new mode 160000", "old mode 120000",
                         "deleted file mode 120000", "index abcd..efab 120000", "index abcd..efab 160000"):
            patch_file.write_text(PATCH + metadata + "\n")
            with self.subTest(metadata=metadata), self.assertRaisesRegex(ValueError, "patch.*mode"):
                self.check()
        patch_file.write_text(PATCH + "old mode 100644\nnew mode 100755\nindex abcd..efab 100755\n")
        self.assertEqual(self.check(), 4)

    def test_indexed_content_size_bound_and_preflight_before_fetch(self) -> None:
        with patch.object(guard, "MAX_FILE", 8), self.assertRaisesRegex(ValueError, "size bound"):
            self.check(index_paths={"lab/qemu/scripts/generated.py": BLOB},
                       index_content=lambda _blob: b"too much public source")
        with patch.object(guard, "git", return_value=str(guard.MAX_FILE + 1).encode()) as git_call, \
                self.assertRaisesRegex(ValueError, "indexed source exceeds size bound"):
            guard.git_source(self.root, BLOB)
        git_call.assert_called_once_with(self.root, "cat-file", "-s", BLOB)

    def test_missing_allowed_target_and_unsafe_target_fail(self) -> None:
        manifest = copy.deepcopy(self.manifest)
        manifest["qemu_patch_targets"].append("hw/arm/missing.c")
        with self.assertRaisesRegex(ValueError, "targets differ"):
            self.check(manifest)
        manifest["qemu_patch_targets"] = ["../private.c"]
        with self.assertRaisesRegex(ValueError, "unsafe"):
            self.check(manifest)

    def retired(self) -> dict[str, Any]:
        manifest = copy.deepcopy(self.manifest)
        manifest["retired_index_paths"] = [{"path": guard.RETIRED, "git_blob": BLOB,
                                             "provenance": "Generated historical public blob."}]
        return manifest

    def test_only_pinned_missing_historical_public_lock_may_remain_indexed(self) -> None:
        manifest = self.retired()
        self.assertEqual(self.check(manifest, index_paths={guard.RETIRED: BLOB}), 4)
        self.assertEqual(self.check(manifest), 4)
        with self.assertRaisesRegex(ValueError, "blob changed"):
            self.check(manifest, index_paths={guard.RETIRED: "b" * 40})
        path = self.root / guard.RETIRED
        path.parent.mkdir(parents=True)
        path.write_text("restored")
        with self.assertRaisesRegex(ValueError, "restored"):
            self.check(manifest, index_paths={guard.RETIRED: BLOB})
        with self.assertRaisesRegex(ValueError, "restored"):
            self.check(manifest)

    def test_retired_exception_cannot_hide_arbitrary_private_file(self) -> None:
        manifest = self.retired()
        manifest["retired_index_paths"][0]["path"] = "lab/qemu/private.bin"
        with self.assertRaisesRegex(ValueError, "only the explicitly retired"):
            self.check(manifest)

    @unittest.skipUnless(shutil.which("git"), "Git required for actual index discovery")
    def test_git_discovers_ignored_forced_index_independently_of_working_extras(self) -> None:
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        (self.root / ".gitignore").write_text(".work/\nlab/qemu/ignored.bin\n")
        (self.root / "lab/qemu/ignored.bin").write_bytes(b"ignored generated output")
        self.assertEqual(guard.check(self.root, json.dumps(self.manifest).encode()), 4)
        private = self.root / ".work/private.bin"
        private.parent.mkdir()
        private.write_bytes(b"generated fixture")
        subprocess.run(["git", "-C", str(self.root), "add", "-f", str(private)], check=True)
        with self.assertRaisesRegex(ValueError, "private/generated artifact in Git index"):
            guard.check(self.root, json.dumps(self.manifest).encode())

    @unittest.skipUnless(shutil.which("git"), "Git required for actual index discovery")
    def test_git_discovers_nonignored_extra_and_staged_binary_source(self) -> None:
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        extra = self.root / "lab/qemu/rootfs.cpio"
        extra.write_bytes(b"generated fixture")
        with self.assertRaisesRegex(ValueError, "unlisted nonignored"):
            guard.check(self.root, json.dumps(self.manifest).encode())
        extra.unlink()
        path = self.root / "lab/qemu/scripts/generated.py"
        original = path.read_bytes()
        path.write_bytes(b"staged\0fixture")
        subprocess.run(["git", "-C", str(self.root), "add", str(path)], check=True)
        path.write_bytes(original)
        with self.assertRaisesRegex(ValueError, "NUL/binary"):
            guard.check(self.root, json.dumps(self.manifest).encode())


if __name__ == "__main__":
    unittest.main()
