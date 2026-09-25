#!/usr/bin/env python3
"""Shared JSON and digest primitives for the live-campaign build, gate,
package, and session receipts.

Each caller keeps its own thin wrapper for the pieces that must stay
patchable or raise a caller-specific exception type; see the module
docstrings of `build.py`, `gates.py`, `package.py`, and `session.py`.
"""

from __future__ import annotations

import hashlib
import json
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
