#!/usr/bin/env python3
"""Activate both emulator providers on private, caller-selected Unix sockets.

Run from the checkout with PYTHONPATH=lab/emulator/src. This host launcher
owns the socket paths; the package servers consume inherited descriptors.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import stat
import subprocess
import sys
import time
from pathlib import Path


def private_directory(path: Path) -> None:
    """Create private missing ancestors, retaining transport ownership rules."""
    for directory in reversed((path, *path.parents)):
        try:
            directory.mkdir(mode=0o700)
        except FileExistsError:
            pass
        info = directory.lstat()
        if (
            not stat.S_ISDIR(info.st_mode)
            or info.st_uid not in (0, os.getuid())
            or info.st_mode & 0o022
        ):
            raise ValueError(f"unsafe socket directory: {directory}")
    # Existing parent directories keep their original mode. The socket directory
    # itself must already be private, or be one we just created.
    if path.stat().st_mode & 0o077:
        raise ValueError(f"socket directory must be mode 0700: {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.work_root.absolute()
    sockets: list[socket.socket] = []
    paths: list[tuple[Path, int, int]] = []
    processes: list[subprocess.Popen[bytes]] = []

    def interrupted(_signum: int, _frame: object) -> None:
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    try:
        private_directory(root)
        root = root.resolve()
        state = root / "activity.json"
        for action, name, kind in (
            ("serve", "sipd.sock",
             socket.SOCK_SEQPACKET if sys.platform.startswith("linux") else socket.SOCK_STREAM),
            ("serve-device", "device.sock", socket.SOCK_STREAM),
        ):
            path = root / name
            listener = socket.socket(socket.AF_UNIX, kind)
            sockets.append(listener)
            # bind refuses existing entries; never unlink an unknown/stale path.
            listener.bind(str(path))
            info = path.lstat()
            paths.append((path, info.st_dev, info.st_ino))
            path.chmod(0o600)
            listener.listen(16)
            processes.append(subprocess.Popen(
                [
                    sys.executable, "-m", "aula_emulator", action,
                    "--fd", str(listener.fileno()), "--work-root", str(root),
                    "--state-file", str(state),
                ],
                pass_fds=(listener.fileno(),),
            ))
        print(json.dumps({
            "control_socket_path": str(root / "sipd.sock"),
            "expected_sipd_uid": os.getuid(),
            "device_socket_path": str(root / "device.sock"),
            "expected_device_uid": os.getuid(),
            "development_device_provider": True,
        }, indent=2), flush=True)
        while all(process.poll() is None for process in processes):
            time.sleep(0.1)
        print("an emulator provider exited; stopping both", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError) as error:
        print(f"host activation: {error}", file=sys.stderr)
        return 3
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for listener in sockets:
            listener.close()
        for path, device, inode in paths:
            try:
                current = path.lstat()
                if current.st_dev == device and current.st_ino == inode:
                    path.unlink()
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
