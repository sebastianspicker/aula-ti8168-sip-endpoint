#!/usr/bin/env python3
"""Compare a QEMU worktree to the exact patch stack using an isolated index."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tooling/workspace'))
from protected_output import input_path, output_path


def verify(source: Path, commit: str, patches: list[Path], scratch: Path) -> None:
    if not patches:
        raise ValueError('QEMU patch stack is empty')
    source = input_path(ROOT, str(source))
    scratch = output_path(ROOT, str(scratch))
    scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='source-index-', dir=scratch) as temporary:
        environment = dict(os.environ, GIT_INDEX_FILE=str(Path(temporary) / 'index'))

        def git(*arguments: str) -> bytes:
            return subprocess.check_output(['git', '-c', 'core.filemode=true', '-C', str(source),
                                            *arguments], env=environment)

        if git('rev-parse', 'HEAD').decode().strip() != commit:
            raise ValueError('QEMU source is not at the reviewed base commit')
        git('read-tree', commit)
        for patch in patches:
            git('apply', '--cached', str(patch.resolve(strict=True)))
        git('diff', '--exit-code', '--no-ext-diff', '--ignore-submodules=none')
        if git('ls-files', '--others', '--exclude-standard', '-z'):
            raise ValueError('QEMU prepared source contains unexpected untracked files')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--commit', required=True)
    parser.add_argument('patches', nargs='+', type=Path)
    args = parser.parse_args()
    try:
        verify(args.source, args.commit, args.patches, ROOT / '.work/build/qemu-source-check')
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f'QEMU prepared source verification failed: {error}', file=sys.stderr)
        return 1
    print('QEMU prepared source matches the complete reviewed patch stack')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
