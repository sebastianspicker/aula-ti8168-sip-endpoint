"""Independent small-geometry checks of manifest composition behavior."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import stat
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

SOURCE = Path(__file__).resolve().parents[1] / "compose_nand.py"
SPEC = importlib.util.spec_from_file_location("compose_nand", SOURCE)
assert SPEC and SPEC.loader
composer = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = composer
SPEC.loader.exec_module(composer)


class CompositionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.manifest = self.root / "manifest.json"
        self.output = self.root / "output"
        self.bank_bytes = 4096

    def tearDown(self):
        self.temporary.cleanup()

    def segment(self, name, data, offset):
        (self.root / name).write_bytes(data)
        return {
            "path": name,
            "offset": offset,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
        }

    def compose(self, segments):
        self.manifest.write_text(
            json.dumps({"schema_version": 1, "segments": segments})
        )
        return composer.compose(self.manifest, self.output, bank_bytes=self.bank_bytes)

    def test_empty_banks_and_oob_are_entirely_ff(self):
        report = self.compose([])
        for index in (0, 1):
            self.assertEqual(
                (self.output / f"bank{index}.bin").read_bytes(), b"\xff" * 4096
            )
            self.assertEqual(
                (self.output / f"bank{index}.oob.bin").read_bytes(), b"\xff" * 128
            )
        self.assertTrue(report["synthetic"])
        self.assertEqual(report["fill_byte"], 255)
        self.assertIn("no captured OOB", report["oob_provenance"])
        for item in report["files"]:
            path = self.output / item["path"]
            self.assertEqual(
                hashlib.sha256(path.read_bytes()).hexdigest(), item["sha256"]
            )
            self.assertEqual(path.stat().st_size, item["size"])
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o400)
        self.assertEqual(
            json.loads((self.output / "composition.json").read_text()), report
        )

    def test_explicit_offsets_relative_paths_cross_bank_and_source_unchanged(self):
        first = self.segment("first.bin", b"ABC", "0x7")
        crossing = self.segment("cross.bin", b"123456", 4094)
        last = self.segment("last.bin", b"Z", 8191)
        expected = bytearray(b"\xff" * 8192)
        expected[7:10], expected[4094:4100], expected[8191:8192] = (
            b"ABC",
            b"123456",
            b"Z",
        )
        self.compose([last, crossing, first])
        actual = b"".join(
            (self.output / f"bank{bank}.bin").read_bytes() for bank in (0, 1)
        )
        self.assertEqual(actual, expected)
        self.assertEqual((self.root / "first.bin").read_bytes(), b"ABC")
        self.assertEqual((self.root / "cross.bin").read_bytes(), b"123456")
        self.assertEqual((self.root / "last.bin").read_bytes(), b"Z")

    def test_rejects_overlap_without_outputs(self):
        first = self.segment("a", b"abc", 5)
        second = self.segment("b", b"xyz", 7)
        with self.assertRaisesRegex(composer.CompositionError, "overlap"):
            self.compose([first, second])
        self.assertFalse(self.output.exists())

    def test_rejects_out_of_bounds_zero_size_bad_offsets_and_size_mismatch(self):
        valid = self.segment("a", b"abc", 0)
        for mutation in (
            {"offset": 8190},
            {"offset": 8192},
            {"offset": -1},
            {"offset": True},
            {"offset": "-1"},
            {"offset": "8190"},
            {"size": 0},
            {"size": 1},
            {"size": True},
        ):
            with (
                self.subTest(mutation=mutation),
                self.assertRaises(composer.CompositionError),
            ):
                self.compose([{**valid, **mutation}])
            self.assertFalse(self.output.exists())

    def test_rejects_sha_mismatch_before_outputs(self):
        segment = self.segment("a", b"abc", 0)
        segment["sha256"] = "0" * 64
        with self.assertRaisesRegex(composer.CompositionError, "SHA256"):
            self.compose([segment])
        self.assertFalse(self.output.exists())

    def test_rejects_unknown_keys_duplicate_keys_and_wrong_schema(self):
        segment = self.segment("a", b"abc", 0)
        with self.assertRaises(composer.CompositionError):
            self.compose([{**segment, "guess_partition": True}])
        for raw in (
            '{"schema_version":1,"schema_version":1,"segments":[]}',
            '{"schema_version":true,"segments":[]}',
            '{"schema_version":1,"segments":[],"unknown":true}',
            '{"schema_version":2,"segments":[]}',
            '{"schema_version":1,"segments":{}}',
        ):
            with self.subTest(raw=raw):
                self.manifest.write_text(raw)
                with self.assertRaises(composer.CompositionError):
                    composer.compose(self.manifest, self.output, bank_bytes=4096)
                self.assertFalse(self.output.exists())

    def test_never_overwrites_existing_output(self):
        self.output.mkdir()
        sentinel = self.output / "bank0.bin"
        sentinel.write_bytes(b"original")
        with self.assertRaises(FileExistsError):
            self.compose([])
        self.assertEqual(sentinel.read_bytes(), b"original")

    def test_source_changed_after_validation_rejects_and_removes_partial_outputs(self):
        segment = self.segment("a", b"abc", 0)
        original = composer.read_manifest

        def mutate(*args, **kwargs):
            result = original(*args, **kwargs)
            (self.root / "a").write_bytes(b"xyz")
            return result

        with (
            patch.object(composer, "read_manifest", side_effect=mutate),
            self.assertRaisesRegex(composer.CompositionError, "changed"),
        ):
            self.compose([segment])
        self.assertFalse(self.output.exists())

    def test_fill_failure_cleans_own_partial_files(self):
        with (
            patch.object(composer, "_fill", side_effect=OSError("full disk")),
            self.assertRaises(OSError),
        ):
            self.compose([])
        self.assertFalse(self.output.exists())

    def test_rejects_embedded_null_and_unencodable_path(self):
        segment = self.segment("a", b"abc", 0)
        for path in ("a\x00b", "\ud800"):
            with (
                self.subTest(path=repr(path)),
                self.assertRaises(composer.CompositionError),
            ):
                self.compose([{**segment, "path": path}])
            self.assertFalse(self.output.exists())

    def test_sources_must_be_regular_and_manifest_is_bounded(self):
        source = self.segment("a", b"abc", 0)
        source["path"] = "."
        with self.assertRaises(composer.CompositionError):
            self.compose([source])
        self.manifest.write_bytes(b" " * (composer.MAX_MANIFEST_BYTES + 1))
        with self.assertRaisesRegex(composer.CompositionError, "exceeds"):
            composer.compose(self.manifest, self.output, bank_bytes=4096)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
