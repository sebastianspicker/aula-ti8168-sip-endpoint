#!/usr/bin/env python3
"""Compose synthetic two-bank TI8168 NAND backings from explicit segments.

Usage: compose_nand.py MANIFEST.json OUTPUT_DIRECTORY

The output directory must not exist. A manifest contains schema_version=1 and
segments with exactly path, offset, size, sha256. Paths are relative to the
manifest or absolute. Offsets/sizes are integer byte counts or 0x-prefixed hex
strings; every segment contains its complete source file. Example:
{"schema_version":1,"segments":[{"path":"example.bin","offset":"0x100000",
"size":65536,"sha256":"<64 hexadecimal characters>"}]}

Main bytes outside explicit segments and all generated OOB bytes are 0xff.
These are synthetic backings, not captured MTD/OOB. No ECC, bad-block markers,
partition boundaries, boot support or hardware identity are reconstructed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
from dataclasses import dataclass
from itertools import pairwise
from pathlib import Path
from typing import BinaryIO

from private_output import prepare_output

BANK_BYTES = 256 * 1024 * 1024
BANK_COUNT = 2
PAGE_BYTES = 2048
OOB_BYTES_PER_PAGE = 64
CHUNK_BYTES = 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024


class CompositionError(ValueError):
    """An explicit manifest or backing composition failed validation."""


@dataclass(frozen=True)
class Segment:
    path: Path
    offset: int
    size: int
    sha256: str
    identity: tuple[int, int, int, int]


def byte_count(value: object, field: str) -> int:
    if type(value) is int and value >= 0:
        return value
    if isinstance(value, str) and re.fullmatch(r"0x[0-9a-fA-F]+", value):
        return int(value, 16)
    raise CompositionError(
        f"{field} must be a nonnegative integer or 0x-prefixed hex string"
    )


def _pairs(items: list[tuple[str, object]]) -> dict:
    result = dict(items)
    if len(result) != len(items):
        raise CompositionError("duplicate manifest key")
    return result


def _identity(details: os.stat_result) -> tuple[int, int, int, int]:
    return details.st_dev, details.st_ino, details.st_size, details.st_mtime_ns


def _open_source(path: Path) -> BinaryIO:
    descriptor = os.open(
        path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0)
    )
    try:
        if not stat.S_ISREG(os.fstat(descriptor).st_mode):
            raise CompositionError("segment source must be a regular file")
        return os.fdopen(descriptor, "rb")
    except BaseException:
        os.close(descriptor)
        raise


def _sha256(source: BinaryIO) -> str:
    digest = hashlib.sha256()
    while chunk := source.read(CHUNK_BYTES):
        digest.update(chunk)
    return digest.hexdigest()


def read_manifest(path: Path, *, bank_bytes: int = BANK_BYTES) -> list[Segment]:
    """Validate all offsets, sizes, overlap and SHA256 before creating output."""
    with _open_source(path.resolve(strict=True)) as source:
        raw = source.read(MAX_MANIFEST_BYTES + 1)
    if len(raw) > MAX_MANIFEST_BYTES:
        raise CompositionError("manifest exceeds 1 MiB")
    try:
        document = json.loads(raw, object_pairs_hook=_pairs)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise CompositionError(
            "manifest must be bounded UTF-8 JSON with unique keys"
        ) from error
    if (
        type(document) is not dict
        or set(document) != {"schema_version", "segments"}
        or type(document["schema_version"]) is not int
        or document["schema_version"] != 1
        or type(document["segments"]) is not list
        or len(document["segments"]) > 1024
    ):
        raise CompositionError(
            "manifest requires schema_version 1 and up to 1024 segments"
        )
    segments = []
    for item in document["segments"]:
        if (
            type(item) is not dict
            or set(item) != {"path", "offset", "size", "sha256"}
            or not isinstance(item["path"], str)
            or not item["path"]
            or "\x00" in item["path"]
            or not isinstance(item["sha256"], str)
            or not re.fullmatch(r"[a-fA-F0-9]{64}", item["sha256"])
        ):
            raise CompositionError(
                "segments require exactly path, offset, size and SHA256"
            )
        offset, size = (
            byte_count(item["offset"], "offset"),
            byte_count(item["size"], "size"),
        )
        if (
            size == 0
            or offset >= bank_bytes * BANK_COUNT
            or size > bank_bytes * BANK_COUNT - offset
        ):
            raise CompositionError(
                "segment is empty or outside the two-bank address space"
            )
        source_path = Path(item["path"])
        if not source_path.is_absolute():
            source_path = path.resolve().parent / source_path
        try:
            source_path = source_path.resolve(strict=True)
            os.fsencode(source_path)
        except (ValueError, UnicodeError) as error:
            raise CompositionError("segment path is not a valid local path") from error
        expected = item["sha256"].lower()
        with _open_source(source_path) as source:
            before = os.fstat(source.fileno())
            if before.st_size != size:
                raise CompositionError(
                    "segment size does not match its explicit manifest size"
                )
            actual = _sha256(source)
            after = os.fstat(source.fileno())
        if actual != expected or _identity(before) != _identity(after):
            raise CompositionError(
                "segment SHA256 mismatch or source changed during validation"
            )
        segments.append(Segment(source_path, offset, size, expected, _identity(after)))
    segments.sort(key=lambda segment: segment.offset)
    for previous, current in pairwise(segments):
        if previous.offset + previous.size > current.offset:
            raise CompositionError("manifest segments overlap")
    return segments


def _fill(source: BinaryIO, length: int) -> None:
    chunk = b"\xff" * min(CHUNK_BYTES, length)
    remaining = length
    while remaining:
        count = min(len(chunk), remaining)
        source.write(chunk[:count])
        remaining -= count


def _create(path: Path) -> BinaryIO:
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
    return os.fdopen(descriptor, "w+b")


def _copy_segment(segment: Segment, banks: list[BinaryIO], bank_bytes: int) -> None:
    digest = hashlib.sha256()
    position = segment.offset
    copied = 0
    with _open_source(segment.path) as source:
        if _identity(os.fstat(source.fileno())) != segment.identity:
            raise CompositionError("segment source changed after validation")
        while chunk := source.read(CHUNK_BYTES):
            digest.update(chunk)
            start = 0
            while start < len(chunk):
                bank, offset = divmod(position, bank_bytes)
                count = min(len(chunk) - start, bank_bytes - offset)
                if bank >= BANK_COUNT:
                    raise CompositionError(
                        "segment source grew beyond its manifest extent"
                    )
                banks[bank].seek(offset)
                banks[bank].write(chunk[start : start + count])
                start += count
                copied += count
                position += count
        if (
            copied != segment.size
            or digest.hexdigest() != segment.sha256
            or _identity(os.fstat(source.fileno())) != segment.identity
        ):
            raise CompositionError("segment source changed during composition")


def compose(manifest: Path, output: Path, *, bank_bytes: int = BANK_BYTES) -> dict:
    """Write complete FF-backed banks; bank_bytes exists for small independent tests."""
    if type(bank_bytes) is not int or bank_bytes <= 0 or bank_bytes % PAGE_BYTES:
        raise CompositionError("bank size must be a positive whole number of pages")
    segments = read_manifest(manifest, bank_bytes=bank_bytes)
    oob_bytes = bank_bytes // PAGE_BYTES * OOB_BYTES_PER_PAGE
    output.mkdir(mode=0o700)  # Exclusive directory creation rejects all overwrite.
    created: list[Path] = []
    banks: list[BinaryIO] = []
    try:
        files = []
        for bank in range(BANK_COUNT):
            main_path = output / f"bank{bank}.bin"
            main = _create(main_path)
            created.append(main_path)
            banks.append(main)
            _fill(main, bank_bytes)
        for segment in segments:
            _copy_segment(segment, banks, bank_bytes)
        for bank, main in enumerate(banks):
            main.flush()
            os.fsync(main.fileno())
            main.seek(0)
            files.append(
                {
                    "path": f"bank{bank}.bin",
                    "kind": "main",
                    "bank": bank,
                    "offset": bank * bank_bytes,
                    "size": bank_bytes,
                    "sha256": _sha256(main),
                }
            )
            oob_path = output / f"bank{bank}.oob.bin"
            with _create(oob_path) as oob:
                created.append(oob_path)
                _fill(oob, oob_bytes)
                oob.flush()
                os.fsync(oob.fileno())
                oob.seek(0)
                files.append(
                    {
                        "path": oob_path.name,
                        "kind": "synthetic-erased-oob",
                        "bank": bank,
                        "size": oob_bytes,
                        "sha256": _sha256(oob),
                    }
                )
        report = {
            "kind": "synthetic-ti8168-nand-composition-v1",
            "schema_version": 1,
            "synthetic": True,
            "fill_byte": 255,
            "bank_count": BANK_COUNT,
            "bank_bytes": bank_bytes,
            "page_bytes": PAGE_BYTES,
            "oob_bytes_per_page": OOB_BYTES_PER_PAGE,
            "oob_provenance": "generated all-FF; no captured OOB, ECC or bad-block reconstruction",
            "segments": [
                {
                    "path": str(segment.path),
                    "offset": segment.offset,
                    "size": segment.size,
                    "sha256": segment.sha256,
                }
                for segment in segments
            ],
            "files": files,
        }
        report_path = output / "composition.json"
        with _create(report_path) as destination:
            created.append(report_path)
            destination.write((json.dumps(report, indent=2) + "\n").encode())
            destination.flush()
            os.fsync(destination.fileno())
        for path in created:
            path.chmod(0o400)
        return report
    except BaseException:
        for handle in banks:
            handle.close()
        for path in reversed(created):
            path.unlink(missing_ok=True)
        try:
            output.rmdir()
        except OSError:
            pass
        raise
    finally:
        for handle in banks:
            handle.close()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    try:
        arguments.output = prepare_output(arguments.output)
        report = compose(arguments.manifest, arguments.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"error: {error}\n")
    print(
        f"Composed {report['bank_count']} synthetic FF-padded NAND banks; report: {arguments.output / 'composition.json'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
