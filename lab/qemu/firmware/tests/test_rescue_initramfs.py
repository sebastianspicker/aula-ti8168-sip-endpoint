"""Focused pin, source-boundary, archive and UART-receipt checks; no firmware needed."""
from __future__ import annotations

import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rescue_initramfs import build, init_script, regular, sha256, verification_script
from run_original import (
    composition,
    evidence,
    expected_banks,
    file_receipt,
    safe_option,
)


def elf_fixture() -> bytes:
    data = bytearray(52)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<H", data, 18, 40)
    return bytes(data)


def members(data: bytes) -> dict[str, tuple[int, bytes, int, int]]:
    result = {}
    offset = 0
    while True:
        if data[offset:offset + 6] != b"070701":
            raise AssertionError("invalid newc header")
        fields = [int(data[offset + 6 + index * 8:offset + 14 + index * 8], 16) for index in range(13)]
        offset += 110
        name = data[offset:offset + fields[11] - 1].decode()
        if data[offset + fields[11] - 1] != 0:
            raise AssertionError("unterminated newc name")
        offset += fields[11]
        offset += -offset % 4
        payload = data[offset:offset + fields[6]]
        offset += fields[6]
        offset += -offset % 4
        if name == "TRAILER!!!":
            if any(data[offset:]):
                raise AssertionError("nonzero archive trailer padding")
            return result
        if name in result or fields[2] != 0 or fields[3] != 0 or fields[5] != 0:
            raise AssertionError("duplicate member or unexpected ownership/time")
        result[name] = (fields[1], payload, fields[9], fields[10])


class RescueArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.base = Path(self.directory.name)
        self.root = self.base / "runtime"
        (self.root / "bin").mkdir(parents=True)
        (self.root / "lib").mkdir()
        (self.root / "bin/busybox").write_bytes(elf_fixture())
        (self.root / "lib/loader.so").write_bytes(elf_fixture() + b"loader")
        (self.root / "lib/ld-alias.so.rclonelink").write_bytes(b"loader.so")
        self.kernel = self.base / "kernel-input/kernel.img"
        self.kernel.parent.mkdir()
        self.kernel.write_bytes(b"unit-test-kernel-not-executed")
        self.inputs = self.base / "inputs.json"
        self.document = {
            "kind": "aula-rescue-inputs-v1",
            "kernel": {"path": str(self.kernel), "sha256": sha256(self.kernel.read_bytes())},
            "runtime_root": str(self.root), "busybox": "bin/busybox",
            "files": [
                {"path": name, "kind": kind, "sha256": sha256((self.root / source).read_bytes())}
                for name, kind, source in (
                    ("bin/busybox", "file", "bin/busybox"),
                    ("lib/loader.so", "file", "lib/loader.so"),
                    ("lib/ld-alias.so", "rclonelink", "lib/ld-alias.so.rclonelink"),
                )
            ],
        }

    def save(self) -> None:
        self.inputs.write_text(json.dumps(self.document))

    def pack(self, name: str = "archive") -> dict:
        self.save()
        return build(self.inputs, self.base / name)

    def test_deterministic_archive_alias_devices_and_original_bytes(self) -> None:
        before = {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
        first, second = self.pack("first"), self.pack("second")
        self.assertEqual(first["archive"]["sha256"], second["archive"]["sha256"])
        entries = members(Path(first["archive"]["path"]).read_bytes())
        self.assertEqual(entries["lib/ld-alias.so"][1], b"loader.so")
        self.assertTrue(stat.S_ISLNK(entries["lib/ld-alias.so"][0]))
        self.assertEqual(entries["bin/sh"][1], b"/bin/busybox")
        self.assertEqual(entries["dev/console"][2:], (5, 1))
        self.assertTrue(stat.S_ISCHR(entries["dev/console"][0]))
        self.assertIn(b"sha256sum", entries["init"][1])
        self.assertEqual({path: path.read_bytes() for path in before}, before)
        self.assertFalse((self.root / "lib/ld-alias.so").exists())
        result = subprocess.run(["sh", "-n"], input=entries["init"][1], capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_hash_mismatch_rejected_before_output(self) -> None:
        self.document["files"][0]["sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.pack()
        self.assertFalse((self.base / "archive").exists())

    def test_link_escape_and_cycles_rejected(self) -> None:
        alias = self.root / "lib/ld-alias.so.rclonelink"
        for target in (b"../../outside", b"ld-alias.so"):
            with self.subTest(target=target):
                alias.write_bytes(target)
                self.document["files"][2]["sha256"] = sha256(target)
                with self.assertRaises(ValueError):
                    self.pack()

    def test_selected_parent_symlink_and_traversal_rejected(self) -> None:
        self.document["files"][0]["path"] = "../outside"
        with self.assertRaises(ValueError):
            self.pack()
        self.document["files"][0]["path"] = "bin/busybox"
        (self.root / "lib").rename(self.base / "outside-lib")
        (self.root / "lib").symlink_to(self.base / "outside-lib", target_is_directory=True)
        with self.assertRaises(OSError):
            self.pack()

    def test_existing_and_source_tree_outputs_rejected(self) -> None:
        self.pack()
        with self.assertRaises(FileExistsError):
            self.pack()
        self.save()
        with self.assertRaisesRegex(ValueError, "outside"):
            build(self.inputs, self.root / "generated")
        self.assertFalse((self.root / "generated").exists())

    def test_owned_executable_and_private_data_modes_and_checks(self) -> None:
        binary, config = self.base / "owned.elf", self.base / "owned.conf"
        binary.write_bytes(elf_fixture() + b"owned")
        config.write_bytes(b"example=true\n")
        self.document["supplemental"] = [
            {"source": str(binary), "path": "opt/test", "sha256": sha256(binary.read_bytes())},
            {"source": str(config), "path": "opt/test.conf", "sha256": sha256(config.read_bytes()), "kind": "data"},
        ]
        self.document["checks"] = [["/opt/test", "--check-config", "/opt/test.conf"]]
        packed = self.pack()
        entries = members(Path(packed["archive"]["path"]).read_bytes())
        self.assertEqual(stat.S_IMODE(entries["opt/test"][0]), 0o755)
        self.assertEqual(stat.S_IMODE(entries["opt/test.conf"][0]), 0o600)
        self.assertIn(b"/opt/test --check-config /opt/test.conf || return 1", entries["init"][1])
        self.assertIn(b"CHECK:0:EXIT:0", entries["init"][1])
        self.document["checks"] = [["/opt/test.conf"]]
        with self.assertRaisesRegex(ValueError, "check executable"):
            self.pack("invalid")

    def test_fifo_sources_fail_without_blocking(self) -> None:
        fifo = self.root / "bin/fifo"
        os.mkfifo(fifo)
        module_dir = str(Path(__file__).resolve().parents[1])
        code = ("import sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
                "from rescue_initramfs import regular,selected; "
                "regular(Path(sys.argv[2])) if sys.argv[3]=='regular' else "
                "selected(Path(sys.argv[2]),'bin/fifo','file')")
        for operation, path in (("regular", fifo), ("selected", self.root)):
            with self.subTest(operation=operation):
                result = subprocess.run([sys.executable, "-c", code, module_dir, str(path), operation],
                                        capture_output=True, check=False, timeout=2)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(b"ValueError: expected bounded", result.stderr)

    def test_duplicate_manifest_keys_and_archive_ancestor_collisions_rejected(self) -> None:
        self.inputs.write_text('{"kind":"aula-rescue-inputs-v1","kind":"bad"}')
        with self.assertRaises(ValueError):
            build(self.inputs, self.base / "duplicate")
        self.document["files"].append({"path": "bin", "kind": "file", "sha256": "0" * 64})
        with self.assertRaises((OSError, ValueError)):
            self.pack()


class RunReceiptTests(unittest.TestCase):
    def test_exact_ordered_nonce_receipt_and_mount_required(self) -> None:
        digest, nonce = "a" * 64, "testnonce"
        prefix = f"AULA:RESCUE:{nonce}:"
        lines = [prefix + "INIT", prefix + "UNAME:fixture", prefix + "BUSYBOX:" + digest,
                 prefix + "ROOT:ubifs:ro", prefix + "READY"]
        self.assertIsNotNone(evidence("\n".join(lines).encode(), digest, nonce, True))
        self.assertIsNone(evidence("\n".join(lines).encode(), digest, "othernonce", True))
        self.assertIsNone(evidence("\n".join(lines[:-2] + lines[-1:]).encode(), digest, nonce, True))
        self.assertIsNone(evidence("\n".join(reversed(lines)).encode(), digest, nonce, True))
        self.assertIsNone(evidence("\n".join(lines).encode(), "b" * 64, nonce, True))

    def test_each_payload_check_needs_an_ordered_zero_exit_marker(self) -> None:
        lines = ["AULA:RESCUE:INIT", "AULA:RESCUE:UNAME:fixture", "AULA:RESCUE:BUSYBOX:" + "a" * 64,
                 "AULA:RESCUE:CHECK:0:EXIT:0", "AULA:RESCUE:CHECK:1:EXIT:0", "AULA:RESCUE:READY"]
        self.assertIsNotNone(evidence("\n".join(lines).encode(), "a" * 64, check_count=2))
        self.assertIsNone(evidence("\n".join(lines[:3] + lines[4:]).encode(), "a" * 64, check_count=2))
        self.assertIsNone(evidence("\n".join(lines).encode(), "a" * 64, check_count=3))

    def test_command_echo_cannot_satisfy_receipt_or_report_failure(self) -> None:
        script = verification_script("bin/busybox", "a" * 64, [], nonce="nonce", require_rootfs=True)
        self.assertIsNone(evidence(script, "a" * 64, "nonce", True))
        self.assertIn(b'filesystem" = ubifs', script)
        self.assertIn(b'case ",$options," in *,ro,*)', script)
        with self.assertRaises(RuntimeError):
            evidence(b"AULA:RESCUE:nonce:FAIL\n", "a" * 64, "nonce", True)
        with self.assertRaises(RuntimeError):
            evidence(b"Kernel panic - test\n", "a" * 64)
        result = subprocess.run(["sh", "-n"], input=script, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_unpinned_machine_files_and_network_options_rejected(self) -> None:
        for option in ("control-init-file=/tmp/file", "nand-mtd0-image=/tmp/file", "accel=kvm", "netdev=user"):
            with self.assertRaises(ValueError):
                safe_option(option)
        self.assertEqual(safe_option("early-uart-base=0x12340000"), "early-uart-base=0x12340000")

    def test_composition_missing_rootfs_pin_and_receipt_mismatch_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            base = Path(name)
            kernel = base / "kernel"
            kernel.write_bytes(b"kernel")
            pin = {"path": str(kernel), "sha256": sha256(b"kernel")}
            manifest = base / "composition.json"
            manifest.write_text(json.dumps({
                "kind": "synthetic-ti8168-nand-composition-v1", "schema_version": 1,
                "bank_count": 2, "bank_bytes": 256 * 1024 * 1024,
                "segments": [{**pin, "size": 6}], "files": [{}, {}, {}, {}],
            }))
            with self.assertRaisesRegex(ValueError, "kernel/rootfs"):
                composition(manifest, pin, {"path": str(base / "other-root"), "sha256": "b" * 64})
            with self.assertRaises(ValueError):
                file_receipt(kernel, "b" * 64, 6)
            with self.assertRaises(ValueError):
                file_receipt(kernel, sha256(b"kernel"), 7)
            self.assertEqual(regular(kernel), b"kernel")

    def test_composition_is_bound_to_cross_bank_segment_bytes_and_erased_gaps(self) -> None:
        with tempfile.TemporaryDirectory() as name, patch("run_original.BANK_BYTES", 4096):
            base = Path(name)
            kernel, rootfs = base / "kernel", base / "rootfs"
            kernel.write_bytes(b"kernel")
            rootfs.write_bytes(b"rootfs-crosses-boundary")
            segments = [
                {"path": str(kernel), "sha256": sha256(kernel.read_bytes()), "size": 6, "offset": 64},
                {"path": str(rootfs), "sha256": sha256(rootfs.read_bytes()), "size": len(rootfs.read_bytes()), "offset": 4090},
            ]
            combined = bytearray(b"\xff" * 8192)
            for item in segments:
                combined[item["offset"]:item["offset"] + item["size"]] = Path(item["path"]).read_bytes()
            document = {"kind": "synthetic-ti8168-nand-composition-v1", "schema_version": 1,
                        "bank_count": 2, "bank_bytes": 4096, "segments": segments, "files": []}
            for bank in (0, 1):
                for kind, filename, data in (
                    ("main", f"bank{bank}.bin", bytes(combined[bank * 4096:(bank + 1) * 4096])),
                    ("synthetic-erased-oob", f"bank{bank}.oob.bin", b"\xff" * 128),
                ):
                    (base / filename).write_bytes(data)
                    document["files"].append({"path": filename, "bank": bank, "kind": kind,
                                               "size": len(data), "sha256": sha256(data)})
            manifest = base / "composition.json"
            manifest.write_text(json.dumps(document))
            kernel_pin = {"path": str(kernel), "sha256": sha256(kernel.read_bytes())}
            rootfs_pin = {"path": str(rootfs), "sha256": sha256(rootfs.read_bytes())}
            receipt, options = composition(manifest, kernel_pin, rootfs_pin)
            self.assertEqual(len(options), 4)
            self.assertEqual(len(receipt["verified"]), 6)
            forged = bytearray((base / "bank1.bin").read_bytes())
            forged[0] ^= 1
            (base / "bank1.bin").write_bytes(forged)
            document["files"][2]["sha256"] = sha256(bytes(forged))
            manifest.write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, "backing bytes"):
                composition(manifest, kernel_pin, rootfs_pin)
            segments[1]["offset"] = 65
            with self.assertRaisesRegex(ValueError, "overlap"):
                expected_banks(segments)

    def test_init_commands_shell_quote_arguments(self) -> None:
        script = init_script("bin/busybox", "a" * 64, [["/opt/test", "a; echo unexpected"]])
        self.assertIn(b"'a; echo unexpected'", script)


if __name__ == "__main__":
    unittest.main()
