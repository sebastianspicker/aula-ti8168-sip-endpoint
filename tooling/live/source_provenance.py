#!/usr/bin/env python3
"""Canonical maintained-source provenance for live build and gate receipts."""

from __future__ import annotations

import hashlib
import stat
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SKIP_PARTS = frozenset({
    ".work", "BUILD", "__pycache__", "node_modules", ".pytest_cache",
})
SOURCE_ROOTS = tuple(ROOT / name for name in (
    "Makefile", "deployment", "docs", "lab", "product", "tooling", "dependencies",
))
EVIDENCE_SOURCE_ROOTS = tuple(ROOT / "evidence/firmware-analysis" / name for name in (
    "tools", "reverse_engineering/tools", "reverse_engineering/ghidra",
    "reverse_engineering/deep",
))
EVIDENCE_SKIPS = frozenset({
    "archive", "archives", "work", "reports", "extracted", "decompiled_python",
    "decompilations", "decompiled_full", "binary_evidence", "segments", "vendor",
})
EVIDENCE_SUFFIXES = frozenset({
    ".c", ".h", ".inc", ".py", ".sh", ".mjs", ".js", ".java",
})
EVIDENCE_GENERATOR = (
    ROOT / "evidence/firmware-analysis/reverse_engineering/deep/m3/reports/"
    "rebuild_reports.mjs"
)


class SourceProvenanceError(RuntimeError):
    pass


def _file_digest(path: Path) -> bytes:
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(block)
    return value.digest()


def source_candidates(
    source_roots: tuple[Path, ...], evidence_source_roots: tuple[Path, ...],
    evidence_generator: Path,
) -> list[Path]:
    candidates: list[Path] = []
    for root in (*source_roots, *evidence_source_roots):
        mode = root.lstat().st_mode
        if stat.S_ISLNK(mode) or not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)):
            raise SourceProvenanceError(f"maintained source root is unsafe: {root}")
        paths = [root] if stat.S_ISREG(mode) else root.rglob("*")
        if root in evidence_source_roots:
            paths = (
                path for path in paths
                if path.suffix in EVIDENCE_SUFFIXES
                and not any(part in EVIDENCE_SKIPS for part in path.relative_to(root).parts)
            )
        candidates.extend(paths)
    candidates.append(evidence_generator)
    return candidates


def source_digest(
    *, root: Path = ROOT, source_roots: tuple[Path, ...] = SOURCE_ROOTS,
    evidence_source_roots: tuple[Path, ...] = EVIDENCE_SOURCE_ROOTS,
    evidence_generator: Path = EVIDENCE_GENERATOR,
) -> str:
    value = hashlib.sha256()
    candidates = source_candidates(
        source_roots, evidence_source_roots, evidence_generator,
    )
    for path in sorted(set(candidates)):
        try:
            relative_path = path.relative_to(root)
        except ValueError as error:
            raise SourceProvenanceError(
                f"maintained source escaped the repository: {path}"
            ) from error
        if any(part in SKIP_PARTS for part in relative_path.parts):
            continue
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode) or path.is_symlink():
            raise SourceProvenanceError(
                f"maintained source contains a symlink or special file: {relative_path}"
            )
        relative = relative_path.as_posix().encode("utf-8")
        value.update(len(relative).to_bytes(4, "big"))
        value.update(relative)
        value.update(stat.S_IMODE(mode).to_bytes(2, "big"))
        value.update(_file_digest(path))
    return value.hexdigest()
