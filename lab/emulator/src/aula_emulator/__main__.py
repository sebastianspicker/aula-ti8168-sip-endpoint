"""Operate a local synthetic state file without device or network access."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Sequence

from .runtime import SyntheticRuntime


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("status", "record-start", "record-stop", "stream-start", "stream-stop"))
    parser.add_argument("--state-file", type=Path, required=True)
    args = parser.parse_args(argv)
    work = Path(__file__).resolve().parents[4] / ".work"
    target = args.state_file.resolve()
    if not target.is_relative_to(work.resolve()):
        parser.error("state file must be below the repository .work directory")
    runtime = SyntheticRuntime(target)
    if args.action.startswith("record-"):
        runtime.set_recording(args.action == "record-start")
    elif args.action.startswith("stream-"):
        runtime.set_stream_active(args.action == "stream-start")
    print(json.dumps(runtime.snapshot(), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
