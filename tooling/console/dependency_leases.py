"""Kernel-owned dependency leases survive wrappers and supervised children."""
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import stat
import uuid


class BusyError(RuntimeError):
    """An installation cannot change while a dependency user is alive."""


class Leases:
    def __init__(self, directory: Path):
        self.directory = directory
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        info = directory.lstat()
        if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid()
                or info.st_mode & 0o022):
            raise RuntimeError("console lease directory must be an owned non-writable real directory")

    @contextmanager
    def metadata(self):
        with self.open_lock("metadata.lock") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield

    def acquire(self, exclusive: bool):
        lock = self.open_lock("users.lock")
        mode = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
        try:
            fcntl.flock(lock, mode | fcntl.LOCK_NB)
        except BlockingIOError as error:
            lock.close()
            raise BusyError("console dependencies are in use; installation/cleanup requires exclusive access") from error
        return lock

    def open_lock(self, name):
        descriptor = os.open(self.directory / name,
                             os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
        info = os.fstat(descriptor)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                or info.st_nlink != 1 or info.st_mode & 0o022):
            os.close(descriptor)
            raise RuntimeError("console lock must be an owned single-link regular file")
        return os.fdopen(descriptor, "a+")

    def register(self):
        path = self.directory / f"lease-{uuid.uuid4().hex}.json"
        descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        lease = os.fdopen(descriptor, "r+")
        fcntl.flock(lease, fcntl.LOCK_EX)
        json.dump({"owner": os.getpid()}, lease)
        lease.flush()
        return lease

    def prune(self):
        # Each wrapper passes this locked descriptor to its supervised command.
        # The kernel releases it only when every inheriting process has exited;
        # PID reuse and a dead wrapper cannot incorrectly reclaim a live lease.
        for path in self.directory.glob("lease-*.json"):
            descriptor = os.open(path, os.O_RDWR | os.O_NOFOLLOW | os.O_NONBLOCK)
            info = os.fstat(descriptor)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
                    or info.st_nlink != 1 or info.st_mode & 0o022):
                os.close(descriptor)
                raise RuntimeError("console lease must be an owned single-link regular file")
            with os.fdopen(descriptor, "r+") as lease:
                try:
                    fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    continue
                path.unlink()
