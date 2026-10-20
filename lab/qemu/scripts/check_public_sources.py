#!/usr/bin/env python3
"""Reject accidental private/generated artifacts in reviewed public lab sources.

This is an exact path/type/provenance inventory guard, not a secret scanner or
proof about the contents of authored source. No inventory entries are auto-added.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import stat
import subprocess
import sys
from collections.abc import Callable, Mapping
from pathlib import Path, PurePosixPath
from typing import Any

SCOPES = ("lab/qemu", "lab/emulator", "lab/arm-userspace")
CLASSES = {
    "authored-source": {".py", ".sh", ".c", ".h", ".inc", ".S", ".ld", ".toml", ".gitignore"},
    "doc": {".md"},
    "public-dependency-lock": {".json"},
    "patch": {".patch"},
}
RETIRED = "lab/emulator/uv.lock"
MAX_FILE = 16 * 1024 * 1024


def private_components(path: str) -> bool:
    # Any casing/location of these reserved corpus/output directory names is
    # private. This applies to the complete Git index, beyond inventoried roots.
    return any(part.casefold() in (".work", "evidence") for part in path.split("/"))


def relative(value: object) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_./-]+", value):
        raise ValueError(f"unsafe relative path: {value!r}")
    path = PurePosixPath(value)
    if path.is_absolute() or str(path) != value or any(p in (".", "..") for p in value.split("/")) or private_components(value):
        raise ValueError(f"unsafe/private relative path: {value!r}")
    return value


def scoped(path: str) -> bool:
    return any(path.startswith(scope + "/") for scope in SCOPES)


def regular(root: Path, name: str) -> bytes:
    name = relative(name)
    target = root
    for component in name.split("/"):
        target = target / component
        info = target.lstat()
        if stat.S_ISLNK(info.st_mode):
            raise ValueError(f"symlink source/ancestor is forbidden: {name}")
    fd = os.open(target, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_FILE:
            raise ValueError(f"expected bounded regular source: {name}")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            data = stream.read(MAX_FILE + 1)
        if len(data) > MAX_FILE:
            raise ValueError(f"source exceeds size bound: {name}")
        return data
    finally:
        os.close(fd)


def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate inventory JSON key: {key}")
        result[key] = value
    return result


def provenance(item: dict[str, Any]) -> None:
    if not isinstance(item.get("provenance"), str) or not item["provenance"].strip():
        raise ValueError(f"missing provenance: {item.get('path')}")


def inventory(data: bytes) -> tuple[dict[str, str], set[str], dict[str, str]]:
    value = json.loads(data, object_pairs_hook=unique_object)
    required = {"schema", "scopes", "boundary", "files", "qemu_patch_targets", "retired_index_paths"}
    if not isinstance(value, dict) or set(value) != required or type(value["schema"]) is not int or value["schema"] != 1 or value["scopes"] != list(SCOPES):
        raise ValueError("invalid public source inventory schema/scopes")
    if not isinstance(value["boundary"], str) or not value["boundary"].strip():
        raise ValueError("inventory boundary statement is required")
    if not isinstance(value["files"], list) or not value["files"]:
        raise ValueError("inventory files must be a nonempty list")
    entries: dict[str, str] = {}
    for item in value["files"]:
        if not isinstance(item, dict) or set(item) != {"path", "class", "provenance"}:
            raise ValueError("each source needs path, class and provenance")
        name = relative(item["path"])
        kind = item["class"]
        if not scoped(name) or name in entries:
            raise ValueError(f"duplicate or out-of-scope source: {name}")
        if not isinstance(kind, str) or kind not in CLASSES or (PurePosixPath(name).suffix or PurePosixPath(name).name) not in CLASSES[kind]:
            raise ValueError(f"source class/extension is not allowed: {name}")
        provenance(item)
        entries[name] = kind
    raw_targets = value["qemu_patch_targets"]
    if not isinstance(raw_targets, list) or not raw_targets:
        raise ValueError("exact QEMU patch target allowlist is required")
    targets = {relative(target) for target in raw_targets}
    if len(targets) != len(raw_targets) or any(not target.startswith(("hw/", "include/hw/")) for target in targets):
        raise ValueError("duplicate/invalid QEMU patch target")
    retired: dict[str, str] = {}
    if not isinstance(value["retired_index_paths"], list):
        raise TypeError("retired_index_paths must be a list")
    for item in value["retired_index_paths"]:
        if not isinstance(item, dict) or set(item) != {"path", "git_blob", "provenance"}:
            raise ValueError("invalid retired public index entry")
        if item["path"] != RETIRED or RETIRED in retired or RETIRED in entries:
            raise ValueError("only the explicitly retired emulator lock may be excepted")
        blob = item["git_blob"]
        if not isinstance(blob, str) or re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", blob) is None:
            raise ValueError("retired index entry requires pinned public Git blob")
        provenance(item)
        retired[RETIRED] = blob
    return entries, targets, retired


def text_source(name: str, data: bytes, kind: str, targets: set[str]) -> set[str]:
    if len(data) > MAX_FILE:
        raise ValueError(f"source exceeds size bound: {name}")
    if b"\0" in data:
        raise ValueError(f"NUL/binary content in public source: {name}")
    text = data.decode("utf-8", "strict")
    found: set[str] = set()
    if kind != "patch":
        return found
    active: str | None = None
    for line in text.splitlines():
        if line == "GIT binary patch" or line.startswith("Binary files ") or re.fullmatch(r"(?:literal|delta) [0-9]+", line):
            raise ValueError(f"binary patch payload is forbidden: {name}")
        if (line.startswith(("new file mode ", "deleted file mode ", "old mode ", "new mode "))
                and re.fullmatch(r"(?:new file mode|deleted file mode|old mode|new mode) (?:100644|100755)", line) is None):
            raise ValueError(f"nonregular/unsupported patch mode: {name}")
        if line.startswith("index "):
            match = re.fullmatch(r"index [0-9a-f]+\.\.[0-9a-f]+(?: (100644|100755))?", line)
            if match is None:
                raise ValueError(f"nonregular/unsupported patch index mode: {name}")
        if line.startswith("diff --git "):
            parts = line.split(" ")
            if len(parts) != 4 or not parts[2].startswith("a/") or not parts[3].startswith("b/") or parts[2][2:] != parts[3][2:]:
                raise ValueError(f"unsupported/unsafe patch header: {name}")
            active = relative(parts[2][2:])
            if active not in targets or active in found:
                raise ValueError(f"unexpected/duplicate QEMU overlay target: {active}")
            found.add(active)
        elif line.startswith(("--- ", "+++ ")):
            expected = ("a/" if line.startswith("--- ") else "b/") + str(active)
            if active is None or line[4:] not in (expected, "/dev/null"):
                raise ValueError(f"patch file header disagrees with target: {name}")
        elif line.startswith(("rename from ", "rename to ", "copy from ", "copy to ")):
            raise ValueError(f"patch rename/copy metadata is forbidden: {name}")
    if not found:
        raise ValueError(f"patch has no reviewed targets: {name}")
    return found


def git(root: Path, *arguments: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(root), *arguments], stderr=subprocess.PIPE)


def git_source(root: Path, blob: str) -> bytes:
    size = int(git(root, "cat-file", "-s", blob))
    if not 0 <= size <= MAX_FILE:
        raise ValueError(f"indexed source exceeds size bound: {blob}")
    return git(root, "cat-file", "blob", blob)


def git_index(root: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for record in git(root, "ls-files", "--stage", "-z").split(b"\0"):
        if not record:
            continue
        meta, raw_name = record.split(b"\t", 1)
        mode, blob, stage = meta.decode("ascii").split(" ")
        name = raw_name.decode("utf-8", "strict")
        if stage != "0":
            raise ValueError(f"unmerged index entry: {name}")
        if scoped(name) and mode not in ("100644", "100755"):
            raise ValueError(f"nonregular indexed source: {name}")
        entries[name] = blob
    return entries


def check(root: Path, manifest: bytes, *, index_paths: Mapping[str, str] | None = None,
          untracked_paths: list[str] | None = None,
          index_content: Callable[[str], bytes] | None = None) -> int:
    entries, targets, retired = inventory(manifest)
    if index_paths is None:
        index_paths = git_index(root)
        index_content = lambda blob: git_source(root, blob)
    if untracked_paths is None:
        untracked_paths = [path.decode("utf-8", "strict") for path in
                           git(root, "ls-files", "--others", "--exclude-standard", "-z", "--", *SCOPES).split(b"\0") if path]
    for name in retired:
        if os.path.lexists(root / name):
            raise ValueError(f"retired public source restored: {name}")
    for name in untracked_paths:
        relative(name)
        if scoped(name) and name not in entries:
            raise ValueError(f"unlisted nonignored working source/artifact: {name}")
    for name, blob in index_paths.items():
        if private_components(name):
            raise ValueError(f"private/generated artifact in Git index: {name}")
        if not scoped(name):
            continue
        relative(name)
        if name in retired:
            if blob != retired[name] or os.path.lexists(root / name):
                raise ValueError(f"retired public index blob changed/restored: {name}")
        elif name not in entries:
            raise ValueError(f"unlisted indexed source/artifact: {name}")
        elif index_content is not None:
            text_source(name, index_content(blob), entries[name], targets)
    actual_targets: set[str] = set()
    for name, kind in entries.items():
        actual_targets |= text_source(name, regular(root, name), kind, targets)
    if actual_targets != targets:
        raise ValueError(f"QEMU overlay allowlist/actual targets differ: {sorted(targets ^ actual_targets)}")
    return len(entries)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    try:
        count = check(root, regular(root, "lab/PUBLIC_SOURCES.json"))
    except (OSError, ValueError, TypeError, UnicodeError, subprocess.CalledProcessError) as error:
        print(f"public source inventory failed: {error}", file=sys.stderr)
        return 1
    print(f"Public source inventory passed: {count} exact paths; artifact guard, not a secret-content proof")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
