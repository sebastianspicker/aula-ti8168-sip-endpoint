#!/usr/bin/env python3
"""Copy a minimal private glibc 2.12.2 execution oracle without changing evidence.

The output is private runtime evidence, never a product build dependency.
No program is executed and no firmware services, configuration or secrets are
copied. Only the explicitly named loader and system-library files are read.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
from pathlib import Path

from run import create_output, inventory, sha256

LIBRARIES = {
    "ld-linux.so.3": "ld-2.12.2.so", "libc.so.6": "libc-2.12.2.so",
    "libm.so.6": "libm-2.12.2.so", "libpthread.so.0": "libpthread-2.12.2.so",
    "libresolv.so.2": "libresolv-2.12.2.so", "librt.so.1": "librt-2.12.2.so",
    "libdl.so.2": "libdl-2.12.2.so", "libnsl.so.1": "libnsl-2.12.2.so",
    "libnss_dns.so.2": "libnss_dns-2.12.2.so", "libnss_files.so.2": "libnss_files-2.12.2.so",
}


def prepare(rootfs: Path, output: Path) -> dict:
    source = rootfs.resolve(strict=True) / "lib"
    libc = (source / "libc-2.12.2.so").read_bytes()
    if not libc.startswith(b"\x7fELF\x01\x01") or b"stable release version 2.12.2" not in libc:
        raise ValueError("private oracle lacks the expected ELF32 little-endian glibc 2.12.2 metadata")
    runtime = output / "runtime"
    target = runtime / "lib"
    target.mkdir(parents=True)
    for soname, name in LIBRARIES.items():
        path = source / name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f"required private runtime library is not a regular file: {name}")
        link = source / soname
        archived = source / (soname + ".rclonelink")
        if link.is_symlink():
            link_target = os.readlink(link)
        elif archived.is_file() and not archived.is_symlink():
            link_target = archived.read_text(encoding="ascii").strip()
        else:
            raise ValueError(f"missing original or archived runtime symlink: {soname}")
        if re.fullmatch(r"[A-Za-z0-9_.+-]+", link_target) is None or link_target != name:
            raise ValueError(f"unexpected private runtime link target for {soname}")
        shutil.copyfile(path, target / name)
        (target / name).chmod(0o444)
        (target / soname).symlink_to(name)
    support = source / "libgcc_s.so.1"
    if support.is_symlink() or not support.is_file():
        raise ValueError("private runtime requires a regular libgcc_s.so.1")
    shutil.copyfile(support, target / support.name)
    (target / support.name).chmod(0o444)
    manifest = inventory(runtime)
    (output / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return {
        "schema": "aula-private-legacy-runtime-v1", "status": "prepared", "runtime": str(runtime),
        "glibc": "2.12.2", "manifest_sha256": sha256(output / "runtime-manifest.json"),
        "preparer_sha256": sha256(Path(__file__)),
        "input_root": str(source.parent), "limits": [
            "Private recovered system-runtime oracle; do not redistribute or commit its contents",
            "Read-only data preparation only; no execution or build use",
            "Mount runtime read-only in a networkless disposable execution environment",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rootfs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    output = create_output(args.output)
    try:
        report = prepare(args.rootfs, output)
    except (OSError, ValueError) as error:
        report = {"schema": "aula-private-legacy-runtime-v1", "status": "failed", "error": str(error)}
    (output / "preparation.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"status": report["status"], "report": str(output / "preparation.json")}))
    return 0 if report["status"] == "prepared" else 1


if __name__ == "__main__":
    raise SystemExit(main())
