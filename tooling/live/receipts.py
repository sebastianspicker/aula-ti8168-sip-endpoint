#!/usr/bin/env python3
"""Shared JSON, digest and durable-write primitives for the live-campaign build, gate,
package, and session receipts.

Each caller keeps its own thin wrapper for the pieces that must stay
patchable or raise a caller-specific exception type; see the module
docstrings of `build.py`, `gates.py`, `package.py`, and `session.py`.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
from typing import Any


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def is_sha256(value: Any) -> bool:
    return isinstance(value, str) and len(value) == 64 and all(character in "0123456789abcdef" for character in value)


def exact_json(raw: bytes, *, error: type[BaseException] = ValueError, context: str = "JSON") -> Any:
    """Parse JSON, rejecting duplicate object keys.

    `error` and `context` let each caller keep its own exception type and
    field-name wording (for example "duplicate build-input field: ...").
    """

    def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        value: dict[str, Any] = {}
        for key, child in pairs:
            if key in value:
                raise error(f"duplicate {context} field: {key}")
            value[key] = child
        return value

    return json.loads(raw, object_pairs_hook=reject_duplicates)


def write_all(descriptor: int, data: bytes, *, error: type[BaseException], context: str) -> None:
    """Write all of `data`; a zero or failed write raises `error("<context> write did not complete")`."""
    view = memoryview(data)
    while view:
        written = os.write(descriptor, view)
        if written <= 0:
            raise error(f"{context} write did not complete")
        view = view[written:]


def fsync_directory(directory: Path) -> None:
    descriptor = os.open(directory, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_CLOEXEC", 0))
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_new_receipt(
    path: Path, value: Any, *, indent: int | None, error: type[BaseException], context: str
) -> None:
    """Create `path` (create-new, 0600, no symlink), write `value` as sorted JSON, fsync it and its parent."""
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0),
        0o600,
    )
    try:
        rendered = (json.dumps(value, sort_keys=True, indent=indent) + "\n").encode("utf-8")
        write_all(descriptor, rendered, error=error, context=context)
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    fsync_directory(path.parent)
