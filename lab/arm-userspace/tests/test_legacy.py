from __future__ import annotations

import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import acquire_legacy_sysroot as acquisition
import prepare_legacy_runtime as preparation


class LegacyTests(unittest.TestCase):
    def test_download_hash_mismatch_is_not_persisted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "unverified.deb"
            with patch.object(acquisition.urllib.request, "urlopen", return_value=io.BytesIO(b"tampered")):
                with self.assertRaises(ValueError):
                    acquisition.acquire("https://invalid.example/package", path, "0" * 64)
            self.assertFalse(path.exists())

    def test_duplicate_metadata_fields_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            acquisition.stanzas("Package: libc6\nPackage: replacement\n")

    def test_forged_cache_receipt_cannot_replace_pinned_release(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cache = root / "cache"
            (cache / "metadata").mkdir(parents=True)
            (cache / "metadata/Release").write_text("Origin: forged\n")
            output = root / "output"
            output.mkdir()
            with patch.object(acquisition.subprocess, "run") as execute:
                with self.assertRaises(ValueError):
                    acquisition.reconstruct_verified_sysroot(cache, output, root / "keyring", root / "gpgv", root / "dpkg")
                execute.assert_not_called()

    def test_only_named_runtime_libraries_are_copied(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "evidence/lib"
            source.mkdir(parents=True)
            for soname, name in preparation.LIBRARIES.items():
                content = b"\x7fELF\x01\x01 synthetic test input"
                if soname == "libc.so.6":
                    content += b" stable release version 2.12.2"
                (source / name).write_bytes(content)
                (source / (soname + ".rclonelink")).write_text(name)
            (source / "libgcc_s.so.1").write_bytes(b"test support library")
            (source / "unrelated-private-data").write_text("must not be copied")
            output = root / "output"
            output.mkdir()
            report = preparation.prepare(source.parent, output)
            self.assertEqual(report["status"], "prepared")
            self.assertFalse((output / "runtime/lib/unrelated-private-data").exists())
            self.assertEqual((output / "runtime/lib/ld-linux.so.3").readlink(), Path("ld-2.12.2.so"))
            (source / "ld-linux.so.3.rclonelink").write_text("../escape")
            other = root / "rejected"
            other.mkdir()
            with self.assertRaises(ValueError):
                preparation.prepare(source.parent, other)


if __name__ == "__main__":
    unittest.main()
