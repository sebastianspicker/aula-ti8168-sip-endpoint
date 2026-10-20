#!/usr/bin/env python3
"""Build the project-owned ARM probe with explicitly inventoried GNU binutils."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tool_path(value: str) -> Path:
    found = shutil.which(value)
    if found is None:
        raise ValueError(f"required ARM tool is unavailable: {value}")
    result = Path(found).resolve(strict=True)
    if not result.is_file() or not os.access(result, os.X_OK):
        raise ValueError(f"ARM tool is not executable: {result}")
    return result


def command(arguments: list[str]) -> dict[str, object]:
    result = subprocess.run(arguments, check=True, capture_output=True, text=True, timeout=15)
    return {"argv": arguments, "stdout": result.stdout, "stderr": result.stderr}


def build(output: Path, assembler: str, linker: str) -> Path:
    source = Path(__file__).resolve().parent
    assembler_path, linker_path = tool_path(assembler), tool_path(linker)
    output.mkdir(parents=True, exist_ok=True)
    output = output.resolve(strict=True)
    obj, elf = output / "guest.o", output / "guest.elf"
    records = [
        command([str(assembler_path), "--version"]),
        command([str(linker_path), "--version"]),
        command([str(assembler_path), "-mcpu=cortex-a8", "-o", str(obj), str(source / "timer_irq.S")]),
        command([str(linker_path), "-T", str(source / "guest.ld"), "-Map", str(output / "guest.map"),
                 "-o", str(elf), str(obj)]),
    ]
    raw = elf.read_bytes()
    if (len(raw) < 52 or raw[:7] != b"\x7fELF\x01\x01\x01"
            or struct.unpack_from("<H", raw, 18)[0] != 40
            or struct.unpack_from("<I", raw, 24)[0] != 0x80000000):
        raise ValueError("toolchain did not produce the expected ARM32 little-endian guest ELF")
    manifest = {
        "kind": "aula-generated-arm-guest-build-v1",
        "origin": "project-owned synthetic assembly; no vendor firmware",
        "sources": {name: digest(source / name) for name in ("timer_irq.S", "guest.ld", "build_guest.py")},
        "tools": {str(path): digest(path) for path in (assembler_path, linker_path)},
        "commands": records,
        "elf": {"path": str(elf), "sha256": digest(elf), "entry": "0x80000000"},
    }
    (output / "build.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return elf


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--assembler", default="arm-none-eabi-as")
    parser.add_argument("--linker", default="arm-none-eabi-ld")
    arguments = parser.parse_args()
    elf = build(arguments.output_dir, arguments.assembler, arguments.linker)
    print(elf)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        raise SystemExit(1) from error
