"""Synthetic revision-1 device activity provider over an inherited Unix socket.

Only status is implemented. Credential, maintenance, and hardware operations
are rejected by closing the connection, as with an invalid companion request.
The caller owns the inherited descriptor; no socket paths are created or removed.
"""

from __future__ import annotations

import errno
import json
import os
import socket
import struct
import time
from pathlib import Path
from typing import Any

from .runtime import SyntheticRuntime

_LIMIT = 4096
_TIMEOUT = 4.0
_ACTIVITIES = {"unknown", "inactive", "active", "paused"}


def _object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key")
        result[key] = value
    return result


def _remaining(deadline: float) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("device request timed out")
    return remaining


def _receive(connection: socket.socket, size: int, deadline: float) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        connection.settimeout(_remaining(deadline))
        data = connection.recv(size - len(chunks))
        if not data:
            raise ValueError("truncated device request")
        chunks.extend(data)
    return bytes(chunks)


def _serve_connection(connection: socket.socket, runtime: SyntheticRuntime) -> None:
    deadline = time.monotonic() + _TIMEOUT
    try:
        size = struct.unpack("!I", _receive(connection, 4, deadline))[0]
        if not 0 < size <= _LIMIT:
            return
        request = json.loads(_receive(connection, size, deadline).decode("utf-8"), object_pairs_hook=_object)
        if (not isinstance(request, dict) or set(request) != {"revision", "operation"}
                or type(request["revision"]) is not int or request["revision"] != 1
                or request["operation"] != "status"):
            return
    except (OSError, ValueError, RecursionError):
        return
    state = runtime.snapshot()
    recording, streaming = state["recording"], state["streaming"]
    if recording not in _ACTIVITIES or streaming not in _ACTIVITIES:
        raise ValueError("invalid synthetic activity state")
    response = json.dumps({"revision": 1, "recording": recording, "streaming": streaming},
                          separators=(",", ":")).encode("utf-8")
    try:
        connection.settimeout(_remaining(deadline))
        connection.sendall(struct.pack("!I", len(response)) + response)
    except OSError:
        return


def serve_device(fd: int, *, runtime: SyntheticRuntime | None = None,
                 state_file: str | Path | None = None) -> None:
    """Serve one connected fd, or accept requests on an inherited listening fd.

    A persistent runtime refreshes its snapshot for each request, so independent
    CLI activity changes appear without restarting this provider. Passing both
    runtime and state_file is an error. The descriptor must be AF_UNIX/SOCK_STREAM.
    """
    if runtime is not None and state_file is not None:
        raise ValueError("choose runtime or state_file")
    if type(fd) is not int or fd < 0:
        raise ValueError("fd must be a nonnegative integer")
    model = runtime if runtime is not None else SyntheticRuntime(state_file)
    duplicate = os.dup(fd)
    try:
        inherited_socket = socket.socket(fileno=duplicate)
    except OSError:
        os.close(duplicate)
        raise
    with inherited_socket as inherited:
        if inherited.family != socket.AF_UNIX or inherited.type != socket.SOCK_STREAM:
            raise ValueError("device provider requires a Unix stream socket")
        try:
            inherited.getpeername()
        except OSError as error:
            if error.errno != errno.ENOTCONN:
                raise
        else:
            _serve_connection(inherited, model)
            return
        while True:
            connection, _ = inherited.accept()
            with connection:
                _serve_connection(connection, model)
