"""Read the independent digest emitted by the trusted payload builder."""

import os
from pathlib import Path
import stat


def read_manifest_digest(payload: Path) -> str:
    parent = payload.parent
    details = parent.lstat()
    if (not stat.S_ISDIR(details.st_mode) or details.st_uid != os.getuid()
            or stat.S_IMODE(details.st_mode) & 0o022):
        raise ValueError("payload receipt parent is unsafe")
    descriptor = os.open(parent / "payload-manifest.sha256",
                         os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    try:
        details = os.fstat(descriptor)
        if (not stat.S_ISREG(details.st_mode) or details.st_uid != os.getuid()
                or details.st_nlink != 1 or stat.S_IMODE(details.st_mode) != 0o444):
            raise ValueError("payload digest receipt is unsafe")
        raw = os.read(descriptor, 66)
    finally:
        os.close(descriptor)
    if (len(raw) != 65 or raw[-1:] != b"\n"
            or any(byte not in b"0123456789abcdef" for byte in raw[:-1])):
        raise ValueError("payload digest receipt must contain one lowercase SHA-256")
    return raw[:-1].decode("ascii")
