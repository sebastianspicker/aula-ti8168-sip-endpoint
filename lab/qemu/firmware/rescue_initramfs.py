#!/usr/bin/env python3
"""Pack explicitly pinned caller inputs into a minimal generated rescue newc."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import stat
import struct
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any

from private_output import prepare_output

LIMIT = 64 * 1024 * 1024


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def checked(data: bytes, expected: object, label: str) -> bytes:
    if not isinstance(expected, str) or len(expected) != 64 or sha256(data) != expected:
        raise ValueError(f"SHA-256 mismatch or invalid pin: {label}")
    return data


def relative(value: object) -> str:
    if (not isinstance(value, str) or not value or "\0" in value or "\\" in value
            or value.startswith("/") or any(part in ("", ".", "..") for part in value.split("/"))):
        raise ValueError(f"unsafe archive path: {value!r}")
    return value


def regular(path: Path) -> bytes:
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > LIMIT:
            raise ValueError(f"expected bounded regular file: {path}")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            data = stream.read(LIMIT + 1)
        if len(data) > LIMIT:
            raise ValueError(f"file exceeds size limit: {path}")
        return data
    finally:
        os.close(fd)


def selected(root: Path, name: str, kind: str) -> bytes:
    """Walk source ancestors without following extraction symlinks."""
    parts = relative(name).split("/")
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0))
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_NOFOLLOW", 0), dir_fd=fd)
            os.close(fd)
            fd = child
        if kind == "symlink":
            return os.fsencode(os.readlink(parts[-1], dir_fd=fd))
        source = os.open(parts[-1], os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0), dir_fd=fd)
        try:
            info = os.fstat(source)
            if not stat.S_ISREG(info.st_mode) or info.st_size > LIMIT:
                raise ValueError(f"expected bounded selected regular file: {name}")
            with os.fdopen(source, "rb", closefd=False) as stream:
                data = stream.read(LIMIT + 1)
            if len(data) > LIMIT:
                raise ValueError(f"selected file exceeds size limit: {name}")
            return data
        finally:
            os.close(source)
    finally:
        os.close(fd)


@dataclass(frozen=True)
class Entry:
    path: str
    mode: int
    data: bytes = b""
    major: int = 0
    minor: int = 0


def link_destination(name: str, payload: bytes) -> str:
    target = payload.decode("utf-8", "strict")
    if not target or "\0" in target or "\n" in target or "\\" in target:
        raise ValueError(f"invalid extracted symlink: {name}")
    parts: list[str] = [] if target.startswith("/") else list(PurePosixPath(name).parent.parts)
    for part in target.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if not parts:
                raise ValueError(f"extracted symlink escapes guest root: {name}")
            parts.pop()
        else:
            parts.append(part)
    return relative("/".join(parts))


def resolve_entry(name: str, entries: dict[str, Entry]) -> Entry:
    visited: set[str] = set()
    while True:
        if name in visited or len(visited) >= 40 or name not in entries:
            raise ValueError(f"missing or cyclic selected link target: {name}")
        visited.add(name)
        entry = entries[name]
        if not stat.S_ISLNK(entry.mode):
            return entry
        name = link_destination(name, entry.data)


def arm_elf(data: bytes) -> bool:
    return (len(data) >= 52 and data[:7] == b"\x7fELF\x01\x01\x01"
            and struct.unpack_from("<H", data, 18)[0] == 40)


def verification_script(busybox: str, digest: str, checks: list[list[str]],
                        nonce: str = "", require_rootfs: bool = False) -> bytes:
    bb = shlex.quote("/" + busybox)
    prefix = "AULA:RESCUE:" + (nonce + ":" if nonce else "")
    root_check = ""
    if require_rootfs:
        root_check = f'''[ -r /proc/mounts ] || "$BB" mount -t proc proc /proc || return 1
mounted=0
while read device point filesystem options rest; do
 if [ "$point" = / ] && [ "$filesystem" = ubifs ]; then
  case ",$options," in *,ro,*) mounted=1;; esac
 fi
done </proc/mounts
[ "$mounted" = 1 ] || return 1
"$BB" echo {prefix}ROOT:ubifs:ro
'''
    commands = "\n".join(" ".join(shlex.quote(part) for part in command) +
                         f' || return 1\n"$BB" echo {prefix}CHECK:{index}:EXIT:0'
                         for index, command in enumerate(checks))
    return f'''BB={bb}
aula_verify() {{
"$BB" echo {prefix}INIT
actual=$("$BB" sha256sum "$BB") || return 1
[ "${{actual%% *}}" = {shlex.quote(digest)} ] || return 1
release=$("$BB" uname -r) || return 1
[ -n "$release" ] || return 1
"$BB" echo "{prefix}UNAME:$release"
"$BB" echo {prefix}BUSYBOX:{digest}
{root_check}{commands}
"$BB" echo {prefix}READY
}}
aula_verify || "$BB" echo {prefix}FAIL
'''.encode()


def init_script(busybox: str, digest: str, checks: list[list[str]]) -> bytes:
    bb = shlex.quote("/" + busybox)
    setup = f'''#!/bin/sh
BB={bb}
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
fail() {{ "$BB" echo AULA:RESCUE:FAIL; while :; do "$BB" sleep 1; done; }}
"$BB" mount -t proc proc /proc || fail
"$BB" mount -t sysfs sysfs /sys || fail
"$BB" mount -t devtmpfs devtmpfs /dev || fail
exec </dev/console >/dev/console 2>&1
'''.encode()
    return setup + verification_script(busybox, digest, checks) + b'while :; do "$BB" sleep 1; done\n'


def newc(entries: list[Entry]) -> bytes:
    output = bytearray()
    for inode, entry in enumerate([*entries, Entry("TRAILER!!!", 0)], 1):
        name = entry.path.encode("utf-8") + b"\0"
        fields = (inode, entry.mode, 0, 0, 2 if stat.S_ISDIR(entry.mode) else 1, 0,
                  len(entry.data), 0, 0, entry.major, entry.minor, len(name), 0)
        output.extend(b"070701" + b"".join(f"{value:08x}".encode("ascii") for value in fields))
        output.extend(name)
        output.extend(b"\0" * (-len(output) % 4))
        output.extend(entry.data)
        output.extend(b"\0" * (-len(output) % 4))
    output.extend(b"\0" * (-len(output) % 512))
    return bytes(output)


def strict_json(raw: bytes) -> dict[str, Any]:
    def strict(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate manifest field: {key}")
            result[key] = value
        return result
    try:
        value = json.loads(raw, object_pairs_hook=strict)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ValueError("manifest must be bounded UTF-8 JSON with unique fields") from error
    if not isinstance(value, dict):
        raise TypeError("manifest must be an object")
    return value


def load_inputs(path: Path) -> dict[str, Any]:
    raw = regular(path)
    if len(raw) > 1024 * 1024:
        raise ValueError("input manifest exceeds 1 MiB")
    value = strict_json(raw)
    if value.get("kind") != "aula-rescue-inputs-v1":
        raise ValueError("unsupported rescue input manifest")
    if set(value) - {"kind", "kernel", "runtime_root", "busybox", "files", "supplemental", "checks", "rootfs"}:
        raise ValueError("unknown rescue input manifest field")
    return value


def output_allowed(output: Path, root: Path, kernel: Path) -> None:
    destination = output.resolve()
    if destination.is_relative_to(root.resolve()) or destination.is_relative_to(kernel.parent.resolve()):
        raise ValueError("output must be outside the original kernel/runtime trees")


def build(inputs_path: Path, output: Path) -> dict[str, Any]:
    inputs_path = inputs_path.resolve(strict=True)
    inputs = load_inputs(inputs_path)
    kernel = Path(inputs["kernel"]["path"])
    root = Path(inputs["runtime_root"])
    if not kernel.is_absolute() or not root.is_absolute() or root.is_symlink():
        raise ValueError("kernel and runtime_root must be explicit absolute paths; root cannot be a symlink")
    checked(regular(kernel), inputs["kernel"]["sha256"], "kernel")
    destination = output.resolve()
    output_allowed(output, root, kernel)
    files = inputs["files"]
    if not isinstance(files, list) or not 1 <= len(files) <= 128:
        raise ValueError("files must explicitly select 1..128 runtime entries")
    entries: dict[str, Entry] = {}
    provenance: list[dict[str, Any]] = []
    selected_bytes = 0
    for item in files:
        if not isinstance(item, dict) or set(item) != {"path", "kind", "sha256"}:
            raise ValueError("each runtime entry requires exactly path, kind, sha256")
        name, kind = relative(item["path"]), item["kind"]
        if name in entries or kind not in ("file", "symlink", "rclonelink"):
            raise ValueError(f"duplicate path or unsupported entry kind: {name}")
        source = name + (".rclonelink" if kind == "rclonelink" else "")
        data = checked(selected(root, source, kind), item["sha256"], source)
        selected_bytes += len(data)
        if selected_bytes > LIMIT:
            raise ValueError("selected rescue content exceeds 64 MiB")
        mode = stat.S_IFREG | 0o755 if kind == "file" else stat.S_IFLNK | 0o777
        if stat.S_ISLNK(mode):
            link_destination(name, data)
        entries[name] = Entry(name, mode, data)
        provenance.append({"source": str(root / source), **item, "bytes": len(data)})
    for item in inputs.get("supplemental", []):
        if (not isinstance(item, dict) or not {"source", "path", "sha256"} <= item.keys() or
                set(item) - {"source", "path", "sha256", "kind"} or item.get("kind", "executable") not in ("executable", "data")):
            raise ValueError("supplemental entries require source, path, sha256 and optional executable/data kind")
        source = Path(item["source"])
        name = relative(item["path"])
        if not source.is_absolute() or name in entries:
            raise ValueError("supplemental source must be absolute and destination unique")
        data = checked(regular(source), item["sha256"], str(source))
        selected_bytes += len(data)
        if selected_bytes > LIMIT:
            raise ValueError("selected rescue content exceeds 64 MiB")
        executable = item.get("kind", "executable") == "executable"
        if executable and not arm_elf(data):
            raise ValueError("supplemental executable must be ARM32 little-endian ELF")
        entries[name] = Entry(name, stat.S_IFREG | (0o755 if executable else 0o600), data)
        provenance.append({**item, "kind": "owned-supplemental-" + ("executable" if executable else "data"), "bytes": len(data)})
    for name in entries:
        if not stat.S_ISREG(resolve_entry(name, entries).mode):
            raise ValueError(f"selected link must resolve to a selected regular file: {name}")
    busybox = relative(inputs["busybox"])
    executable = resolve_entry(busybox, entries)
    if not arm_elf(executable.data):
        raise ValueError("selected BusyBox must be ARM32 little-endian ELF")
    checks = inputs.get("checks", [])
    if (not isinstance(checks, list) or any(not isinstance(command, list) or not command or
            any(not isinstance(arg, str) or not arg or "\0" in arg for arg in command) for command in checks)):
        raise ValueError("checks must be arrays of nonempty command argument strings")
    for command in checks:
        if (not command[0].startswith("/") or command[0][1:] not in entries or
                not (resolve_entry(command[0][1:], entries).mode & stat.S_IXUSR)):
            raise ValueError("check executable must name an explicitly selected absolute guest path")
    generated = [Entry("init", stat.S_IFREG | 0o755, init_script(busybox, sha256(executable.data), checks)),
                 Entry("bin/sh", stat.S_IFLNK | 0o777, ("/" + busybox).encode()),
                 Entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1),
                 Entry("dev/null", stat.S_IFCHR | 0o666, major=1, minor=3)]
    for entry in generated:
        if entry.path in entries:
            raise ValueError(f"selected entry conflicts with generated rescue path: {entry.path}")
        entries[entry.path] = entry
    directories = {"bin", "dev", "proc", "sys", "tmp", "run", "sbin", "usr", "usr/bin"}
    for name in entries:
        directories.update(str(parent) for parent in PurePosixPath(name).parents if str(parent) != ".")
    if directories & entries.keys():
        raise ValueError("archive file/link cannot be an ancestor directory")
    all_entries = [Entry(name, stat.S_IFDIR | (0o1777 if name == "tmp" else 0o755)) for name in directories]
    all_entries.extend(entries.values())
    all_entries.sort(key=lambda entry: entry.path)
    if sum(len(entry.data) for entry in all_entries) > LIMIT:
        raise ValueError("selected rescue content exceeds 64 MiB")
    archive = newc(all_entries)
    destination.mkdir(parents=True, exist_ok=False, mode=0o700)
    archive_path = destination / "rescue.cpio"
    archive_path.write_bytes(archive)
    archive_path.chmod(0o600)
    manifest: dict[str, Any] = {
        "kind": "aula-rescue-build-v1", "inputs_path": str(inputs_path),
        "inputs_sha256": sha256(regular(inputs_path)), "kernel": inputs["kernel"],
        "runtime_root": str(root), "selected": provenance, "busybox_sha256": sha256(executable.data),
        "checks": checks, "busybox_path": busybox, "init_sha256": sha256(entries["init"].data),
        "archive": {"path": str(archive_path), "sha256": sha256(archive), "bytes": len(archive)},
        "claim": "new minimal rescue userspace from pinned inputs; original vendor init is never used",
    }
    (destination / "build.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir = prepare_output(args.output_dir)
    print(json.dumps(build(args.inputs, args.output_dir), indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
