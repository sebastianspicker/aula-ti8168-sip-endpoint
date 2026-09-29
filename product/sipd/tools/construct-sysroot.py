#!/usr/bin/env python3
"""Construct a hash-pinned development sysroot outside recovered evidence.

The input manifest names already acquired headers, loader, and runtime
libraries.  This tool neither downloads dependencies nor infers ABI material
from the build host.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
EVIDENCE_ROOT = PROJECT.parents[1] / "evidence"


def die(message: str) -> None:
    raise SystemExit(f"construct-sysroot: {message}")


def inside(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def regular(path: Path) -> bytes:
    if path.is_symlink() or not path.is_file() or path.resolve() != path.absolute():
        die(f"source must be a regular non-symlink file: {path}")
    if inside(path.resolve(), EVIDENCE_ROOT.resolve(strict=False)):
        die("sysroot inputs must not come from the protected evidence tree")
    return path.read_bytes()


def digest(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def relative_destination(value: object) -> Path:
    if not isinstance(value, str):
        die("manifest destination must be a string")
    path = Path(value)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        die(f"unsafe destination: {value}")
    return path


def checked_output(value: Path) -> Path:
    if value.exists() or value.is_symlink() or not value.parent.is_dir():
        die("output must be a new directory below an existing non-symlink parent")
    output = value.parent.resolve() / value.name
    if inside(output, EVIDENCE_ROOT.resolve(strict=False)):
        die("refusing to write inside protected evidence tree")
    return output


def checked_manifest(path: Path) -> tuple[dict[str, object], list[object], str]:
    try:
        raw = regular(path)
        document = json.loads(raw)
    except (OSError, json.JSONDecodeError) as error:
        die(f"invalid manifest: {error}")
    if not isinstance(document, dict) or document.get("format") != "aula-sipd-sysroot-v1":
        die("unsupported manifest format")
    files = document.get("files")
    if not isinstance(files, list) or not files:
        die("manifest must contain a non-empty files list")
    return document, files, digest(raw)


def populate_sysroot(output: Path, files: list[object], manifest_digest: str) -> None:
    output.mkdir(mode=0o755)
    inventory: list[dict[str, object]] = []
    try:
        for item in files:
            if not isinstance(item, dict):
                die("manifest entry must be an object")
            source_value, target_value, expected = item.get("source"), item.get("target"), item.get("sha256")
            if not isinstance(source_value, str) or not isinstance(expected, str) or len(expected) != 64:
                die("manifest entry needs source, target, and sha256")
            source = Path(source_value)
            value = regular(source)
            if digest(value) != expected:
                die(f"source hash does not match manifest: {source}")
            target = output / relative_destination(target_value)
            target.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
            if target.exists() or target.is_symlink():
                die(f"duplicate destination: {target.relative_to(output)}")
            with target.open("xb") as stream:
                stream.write(value)
            os.chmod(target, int(str(item.get("mode", "0644")), 8))
            inventory.append({"target": str(target.relative_to(output)), "sha256": expected, "mode": f"{stat.S_IMODE(target.stat().st_mode):04o}", "size": len(value)})
        metadata = {"format": "aula-sipd-sysroot-lock-v1", "source_manifest_sha256": manifest_digest, "files": sorted(inventory, key=lambda entry: str(entry["target"]))}
        (output / ".aula-sipd-sysroot-lock.json").write_text(json.dumps(metadata, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    except BaseException:
        shutil.rmtree(output)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = checked_output(args.output)
    _, files, manifest_digest = checked_manifest(args.manifest)
    populate_sysroot(output, files, manifest_digest)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
