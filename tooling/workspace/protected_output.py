#!/usr/bin/env python3
"""Canonicalize reviewed inputs and keep generated output below ``.work``."""
from pathlib import Path
import sys


def _inside(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def input_path(repository: Path, requested: str) -> Path:
    """Resolve an existing input while rejecting every repository evidence tree."""
    repository = repository.resolve(strict=True)
    evidence = repository / "evidence"
    lexical = Path(requested).absolute()
    resolved = lexical.resolve(strict=True)
    if _inside(lexical, evidence) or _inside(resolved, evidence.resolve(strict=False)):
        raise ValueError("repository evidence is not a build input")
    return resolved


def output_path(repository: Path, requested: str) -> Path:
    """Require both the requested and resolved output to remain below ``.work``."""
    repository = repository.resolve(strict=True)
    work = repository / ".work"
    lexical = Path(requested).absolute()
    resolved = lexical.resolve(strict=False)
    resolved_work = work.resolve(strict=False)
    if resolved_work != work.absolute():
        raise ValueError("repository .work must not be a symlink alias")
    if lexical == work or resolved == resolved_work:
        raise ValueError("build output must be strictly below repository .work")
    if not _inside(lexical, work) or not _inside(resolved, resolved_work):
        raise ValueError("build output must remain below repository .work")
    return resolved


def main() -> int:
    try:
        if len(sys.argv) == 4 and sys.argv[1] == "--input":
            print(input_path(Path(sys.argv[2]), sys.argv[3]))
        elif len(sys.argv) == 3:
            print(output_path(Path(sys.argv[1]), sys.argv[2]))
        else:
            raise ValueError("usage: protected_output.py [--input] REPOSITORY PATH")
    except (OSError, RuntimeError, ValueError) as error:
        print(f"output safety: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
