"""Operate synthetic activity state and inherited Unix-socket Aula providers."""

from __future__ import annotations

import argparse
import json
import math
import os
import socket
import sys
from collections.abc import Sequence
from pathlib import Path

from .persistence import PersistenceError
from .runtime import Activity, SyntheticRuntime


def discover_work_root(start: Path | None = None) -> Path:
    """Find the nearest cwd ancestor with .git (file or dir) or .work."""
    current = (Path.cwd() if start is None else start).resolve()
    for ancestor in (current, *current.parents):
        if (ancestor / ".git").exists() or (ancestor / ".work").is_dir():
            return ancestor / ".work"
    raise ValueError("cannot discover a repository .work directory; pass --work-root")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=(
        "status", "reset", "record-start", "record-stop", "record-pause", "record-unknown",
        "stream-start", "stream-stop", "stream-pause", "stream-unknown",
        "serve", "serve-device"))
    parser.add_argument("--state-file", type=Path, required=True)
    parser.add_argument("--work-root", type=Path,
                        help="user-controlled confinement directory (default: discovered repository .work)")
    parser.add_argument("--fd", type=int, help="inherited connected or listening Unix socket")
    parser.add_argument("--fault", action="append", default=[],
                        choices=("persistence-uncertain", "oversized"),
                        help="synthetic LSZ1 fault (serve only; repeatable)")
    parser.add_argument("--reply-delay", type=float, help="LSZ1 reply delay in seconds")
    parser.add_argument("--step-seconds", type=float, help="synthetic call progression interval")
    args = parser.parse_args(argv)
    serving = args.action in ("serve", "serve-device")
    if serving:
        if args.fd is None or args.fd < 0:
            parser.error("serve and serve-device require --fd with an inherited Unix socket")
        try:
            descriptor = os.dup(args.fd)
            try:
                inherited = socket.socket(fileno=descriptor)
            except OSError:
                os.close(descriptor)
                raise
            with inherited:
                allowed = (socket.SOCK_STREAM, socket.SOCK_SEQPACKET)
                if inherited.family != socket.AF_UNIX or inherited.type not in allowed:
                    parser.error("--fd must refer to a stream or packet Unix socket")
                if args.action == "serve-device" and inherited.type != socket.SOCK_STREAM:
                    parser.error("serve-device requires a stream Unix socket")
        except (OSError, ValueError) as error:
            parser.error(f"invalid inherited socket: {error}")
    elif args.fd is not None:
        parser.error("--fd requires serve or serve-device")
    if args.action != "serve" and (
        args.fault or args.reply_delay is not None or args.step_seconds is not None
    ):
        parser.error("--fault, --reply-delay, and --step-seconds require serve")
    if args.reply_delay is not None and (
        not math.isfinite(args.reply_delay) or args.reply_delay < 0
    ):
        parser.error("--reply-delay must be finite and nonnegative")
    if args.step_seconds is not None and (
        not math.isfinite(args.step_seconds) or args.step_seconds <= 0
    ):
        parser.error("--step-seconds must be finite and positive")
    try:
        work = (args.work_root if args.work_root is not None else discover_work_root()).resolve()
        target = args.state_file.absolute()
        # The work root and its ancestors must be user-controlled: resolved
        # path confinement does not defend against concurrent ancestor swaps.
        resolved = target.resolve()
        if resolved == work or not resolved.is_relative_to(work):
            parser.error("state file must be below the repository .work directory or explicit --work-root")
    except (OSError, RuntimeError, ValueError) as error:
        parser.error(str(error))
    try:
        if args.action == "serve":
            from .lsz1 import FaultOptions, serve

            assert args.fd is not None  # Validated before path or persistence work.
            serve(
                args.fd, state_file=target,
                faults=FaultOptions(
                    persistence_uncertain="persistence-uncertain" in args.fault,
                    oversized_response="oversized" in args.fault,
                    reply_delay=args.reply_delay or 0.0,
                ),
                step_seconds=args.step_seconds if args.step_seconds is not None else 0.1,
            )
            return 0
        if args.action == "serve-device":
            from .device_server import serve_device

            assert args.fd is not None
            serve_device(args.fd, state_file=target)
            return 0
        if args.action == "reset":
            state = SyntheticRuntime.reset_file(target)
        else:
            runtime = SyntheticRuntime(target)
            if args.action == "status":
                state = runtime.snapshot()
            else:
                prefix, verb = args.action.split("-", 1)
                values: dict[str, Activity] = {
                    "start": "active", "stop": "inactive", "pause": "paused", "unknown": "unknown"}
                state = runtime.set_activity("recording" if prefix == "record" else "streaming", values[verb])
        print(json.dumps(state, sort_keys=True))
        return 0
    except PersistenceError as error:
        print(f"aula-emulator: {error}", file=sys.stderr)
        return 3
    except OSError as error:
        print(f"aula-emulator: server I/O failed: {error}", file=sys.stderr)
        return 3
    except ValueError as error:
        parser.error(str(error))
    except KeyboardInterrupt:
        return 0
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
