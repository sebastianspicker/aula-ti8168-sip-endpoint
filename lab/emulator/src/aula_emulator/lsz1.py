"""Bounded synthetic LSZ1 daemon, served exclusively over inherited Unix FDs.

This is a console/backend contract fixture. It does not implement SIP, RTP,
rendering, registration, credentials, or hardware media. See LSZ1_INVENTORY.md.
"""
from __future__ import annotations

from collections import OrderedDict
from dataclasses import dataclass, field
from enum import IntEnum
import hashlib
import errno
import json
import math
import os
from pathlib import Path
import re
import secrets
import selectors
import socket
import struct
import time
from typing import Callable

from .persistence import JsonStateStore, PersistenceError, PersistenceLoadError

HEADER = struct.Struct("!4sBBHII")
MAX_PAYLOAD = 65_536
MAX_REQUEST = 2_048
RESPONSE, ERROR, EVENT = 1, 2, 4
PROFILES = {"zoom_direct", "zoom_proxy", "private_lab"}
TLS = {"verified", "required", "not_required"}


class Opcode(IntEnum):
    STATUS = 1
    ORIGINATE = 2
    HANGUP = 3
    DTMF = 4
    MEDIA = 5
    REPLACE_CREDENTIAL = 6
    DIAGNOSTICS = 7
    SUBSCRIBE = 8
    SETTINGS = 9
    METRICS = 10


@dataclass(frozen=True)
class Frame:
    opcode: int
    request_id: int = 0
    payload: bytes = b"{}"
    flags: int = 0

    def encode(self) -> bytes:
        if (type(self.opcode) not in (int, Opcode) or not 1 <= self.opcode <= 10
                or type(self.request_id) is not int or not 0 <= self.request_id <= 0xFFFFFFFF
                or type(self.flags) is not int or self.flags < 0 or self.flags & ~7
                or not isinstance(self.payload, bytes) or len(self.payload) > MAX_PAYLOAD):
            raise ValueError("invalid LSZ1 frame")
        return HEADER.pack(b"LSZ1", 1, self.opcode, self.flags,
                           self.request_id, len(self.payload)) + self.payload

    @classmethod
    def decode(cls, data: bytes) -> Frame:
        if len(data) < HEADER.size:
            raise ValueError("truncated LSZ1 frame")
        magic, version, opcode, flags, request_id, length = HEADER.unpack_from(data)
        if (magic != b"LSZ1" or version != 1 or not 1 <= opcode <= 10
                or flags & ~7 or length > MAX_PAYLOAD or len(data) != HEADER.size + length):
            raise ValueError("invalid LSZ1 frame")
        return cls(opcode, request_id, data[HEADER.size:], flags)


def _json(value: object) -> bytes:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True).encode("ascii")


def _error(request: Frame, code: str) -> Frame:
    return Frame(request.opcode, request.request_id,
                 _json({"ok": False, "error": {"code": code}}), RESPONSE | ERROR)


def _object(payload: bytes) -> dict[str, str | int]:
    """Match the deliberately restricted C parser, including lexical limits."""
    def pairs(items: list[tuple[str, object]]) -> dict[str, object]:
        value = dict(items)
        if len(items) != len(value):
            raise ValueError("duplicate key")
        return value

    # C rejects all escapes and non-ASCII, even otherwise valid JSON escapes.
    if b"\\" in payload or any(byte > 126 or (byte < 32 and byte not in b"\t\n\r")
                               for byte in payload):
        raise ValueError("unsupported JSON text")
    try:
        value = json.loads(payload, object_pairs_hook=pairs)
    except (ValueError, UnicodeError, RecursionError) as exc:
        raise ValueError("invalid command object") from exc
    if (type(value) is not dict or len(value) > 6
            or any(len(key) > 31 or any(ord(c) < 32 for c in key)
                   or type(item) not in (str, int)
                   or (type(item) is int and item < 0)
                   or len(str(item)) > 256
                   or (isinstance(item, str) and any(ord(c) < 32 for c in item))
                   for key, item in value.items())):
        raise ValueError("invalid command members")
    # JSON accepts -0; the C parser permits only unsigned digit tokens.
    if re.search(rb':\s*-', payload):
        raise ValueError("signed number")
    return value


def _empty(payload: bytes) -> bool:
    if not payload:
        return True
    try:
        return _object(payload) == {}
    except ValueError:
        return False


@dataclass(frozen=True)
class FaultOptions:
    persistence_uncertain: bool = False
    reply_delay: float = 0.0
    oversized_response: bool = False

    def __post_init__(self) -> None:
        if (type(self.persistence_uncertain) is not bool
                or type(self.oversized_response) is not bool
                or isinstance(self.reply_delay, bool)
                or not isinstance(self.reply_delay, (int, float))
                or not math.isfinite(self.reply_delay) or not 0 <= self.reply_delay <= 30):
            raise ValueError("fault delay must be finite and between 0 and 30 seconds")


class SyntheticDaemon:
    """Single-owner timed call model with volatile replay cache and safe settings."""

    def __init__(self, state_file: str | Path | None = None, *,
                 clock: Callable[[], float] = time.monotonic,
                 step_seconds: float = 0.1,
                 faults: FaultOptions | None = None) -> None:
        if (isinstance(step_seconds, bool) or not isinstance(step_seconds, (int, float))
                or not math.isfinite(step_seconds) or not 0 < step_seconds <= 60):
            raise ValueError("step_seconds must be finite and between 0 and 60")
        self.clock, self.step_seconds = clock, step_seconds
        self.faults = faults or FaultOptions()
        self._store = JsonStateStore(None if state_file is None else str(state_file) + ".sip-settings",
                                     schema_version=1, max_file_bytes=2048)
        saved = self._store.load()
        self.settings = {"revision": 1, "profile": "private_lab", "media": "managed", "tls": "not_required"}
        if saved is not None:
            if not self._valid_saved_settings(saved):
                raise PersistenceLoadError("invalid synthetic LSZ1 settings")
            self.settings = saved
        self.call_state = "idle"
        self.event_revision = 1
        self.video_transmit_enabled = True
        self.audio_muted = False
        self._pending: list[tuple[float, str]] = []
        self._key = secrets.token_bytes(32)
        self._completed: OrderedDict[int, tuple[int, int, bytes, float, Frame]] = OrderedDict()

    @staticmethod
    def _valid_saved_settings(value: dict) -> bool:
        return (set(value) == {"revision", "profile", "media", "tls"}
                and type(value["revision"]) is int and 1 <= value["revision"] <= 0xFFFFFFFF
                and type(value["profile"]) is str and value["profile"] in PROFILES
                and type(value["media"]) is str and value["media"] in {"managed", "disabled"}
                and type(value["tls"]) is str and value["tls"] in {"required", "not_required"}
                and (value["profile"] == "private_lab" or value["tls"] == "required"))

    def tick(self) -> None:
        now = self.clock()
        while self._pending and self._pending[0][0] <= now:
            _, state = self._pending.pop(0)
            self._transition(state)

    def _transition(self, state: str) -> None:
        if self.call_state != state:
            self.call_state = state
            self.event_revision = min(self.event_revision + 1, 0x7FFFFFFF)

    def _schedule(self, states: tuple[str, ...]) -> None:
        now = self.clock()
        self._pending = [(now + self.step_seconds * index, state)
                         for index, state in enumerate(states, 1)]

    def _status(self) -> dict:
        established = self.call_state == "established"
        media = established or self.call_state == "establishing_media"
        stream = {"rendering": False, "fresh": False, "last_success_ns": 0}
        common = {"direction": "sendrecv" if established else "inactive", "security": "rtp",
                  "frames": 0, "packets": 0, "lost": 0, "jitter_ns": 0}
        return {
            "call_state": self.call_state,
            "media_state": "committed" if established else "preparing" if media else "none",
            "revision": 1, "rx_rendering": False,
            "video_transmit_enabled": self.video_transmit_enabled, "audio_muted": self.audio_muted,
            "reconnect_attempts": 0,
            "renderer": {"healthy": False, "audio": dict(stream), "video": dict(stream)},
            "aec": {"delay_state": "inactive", "available": False, "active": False,
                    "processed_frames": 0, "reference_underflows": 0, "resets": 0},
            "sip": {"profile": self.settings["profile"],
                    "transport": "tls" if self.settings["tls"] == "required" else "udp",
                    "registration": "disabled", "status_code": 0, "expires_seconds": 0},
            "video": {**common, "payload_type": 96, "h264_profile_level_id": "none"},
            "audio": {**common, "payload_type": 0}, "last_error": 0,
        }

    def handle(self, request: Frame) -> Frame:
        # Validate direct API input by the same codec used on the socket.
        request.encode()
        if len(request.payload) > MAX_REQUEST:
            raise ValueError("request exceeds daemon payload bound")
        self.tick()
        if request.flags:
            return _error(request, "INVALID_FLAGS")
        cacheable = request.opcode in range(2, 7) or (request.opcode == Opcode.SETTINGS and not _empty(request.payload))
        fingerprint = hashlib.blake2b(request.payload, key=self._key, digest_size=16).digest()
        if cacheable:
            previous = self._completed.get(request.request_id)
            if previous and self.clock() > previous[3]:
                del self._completed[request.request_id]
                previous = None
            if previous:
                if previous[0] == request.opcode and previous[1] == len(request.payload) and previous[2] == fingerprint:
                    return previous[4]
                return _error(request, "REQUEST_ID_CONFLICT")
        try:
            # Reload, policy checks, revision CAS and replacement share one
            # persistent lock across daemon instances. Replayed mutations above
            # keep their original reply regardless of newer on-disk settings.
            with self._store.transaction():
                self._refresh_settings()
                value = self._dispatch(request)
            response = Frame(request.opcode, request.request_id, _json(value), RESPONSE)
        except _CommandError as exc:
            response = _error(request, exc.code)
        except (OSError, PersistenceError) as exc:
            response = _error(request, "PERSISTENCE_UNCERTAIN" if getattr(exc, "committed", False)
                              else "COMMAND_FAILED")
        if self.faults.oversized_response and not response.flags & ERROR:
            response = Frame(request.opcode, request.request_id, b" " * (MAX_REQUEST + 1), RESPONSE)
        committed = not response.flags & ERROR or b'"PERSISTENCE_UNCERTAIN"' in response.payload
        if cacheable and committed:
            cached = response if len(response.payload) <= MAX_REQUEST else _error(request, "REPLAY_UNAVAILABLE")
            self._completed[request.request_id] = (request.opcode, len(request.payload), fingerprint, self.clock() + 120, cached)
            if len(self._completed) > 16:
                self._completed.popitem(last=False)
        return response

    def _refresh_settings(self) -> None:
        if not self._store.enabled:
            return
        saved = self._store.load()
        if saved is None:
            self.settings = {"revision": 1, "profile": "private_lab", "media": "managed", "tls": "not_required"}
        else:
            if not self._valid_saved_settings(saved):
                raise PersistenceLoadError("invalid synthetic LSZ1 settings")
            self.settings = saved

    def _dispatch(self, request: Frame) -> dict:
        op, payload = request.opcode, request.payload
        if op == Opcode.STATUS:
            if payload not in (b"", b"{}"):
                raise _CommandError("INVALID_REQUEST")
            return self._status()
        if op in (Opcode.DIAGNOSTICS, Opcode.SUBSCRIBE):
            if payload != b"{}":
                raise _CommandError()
            if op == Opcode.SUBSCRIBE:
                return {"revision": self.event_revision, "events": [
                    {"id": f"call-state-{self.event_revision}", "type": "call_state", "message": self.call_state}]}
            return {"state": "not_ready", "checks": {
                name: {"state": state} for name, state in (
                    ("configuration", "ready"), ("sip_control", "not_ready"),
                    ("media_readiness", "unavailable"), ("tls_profile_policy", "ready"),
                    ("renderer_truth", "unavailable"))}}
        if op == Opcode.METRICS:
            if not _empty(payload):
                raise _CommandError()
            counters = {key: 0 for key in ("queue_depth", "packet_drops", "access_unit_drops", "backend_drops", "backend_restarts")}
            return {"revision": 1, "reset": {"streams": "session", "poll": "process"},
                    "streams": [{"kind": kind, **counters} for kind in ("audio", "video")],
                    "poll": {"late_count": 0, "max_lateness_ms": 0}}
        if op == Opcode.REPLACE_CREDENTIAL:
            raise _CommandError("UNSUPPORTED")
        if op == Opcode.HANGUP:
            if not _empty(payload):
                raise _CommandError("INVALID_REQUEST")
            if self.call_state in {"terminated", "stopped", "terminating"}:
                return {"ok": True}
            if self.call_state not in {"inviting", "early", "establishing_media", "established"}:
                raise _CommandError()
            self._transition("terminating")
            self._schedule(("terminated",))
            return {"ok": True}
        if op == Opcode.SETTINGS and _empty(payload):
            return dict(self.settings)
        try:
            value = _object(payload)
        except ValueError as exc:
            raise _CommandError() from exc
        if op == Opcode.SETTINGS:
            return self._settings(value)
        if op == Opcode.ORIGINATE:
            self._originate(value)
        elif op == Opcode.DTMF:
            if (set(value) != {"tone"} or type(value["tone"]) is not str
                    or len(value["tone"]) != 1 or value["tone"] not in "0123456789*#ABCD"
                    or self.call_state != "established"):
                raise _CommandError()
        elif op == Opcode.MEDIA:
            self._media(value, len(payload))
        else:
            raise _CommandError("UNSUPPORTED")
        return {"ok": True}

    def _settings(self, value: dict) -> dict:
        if (set(value) not in ({"revision", "profile", "media"}, {"revision", "profile", "media", "tls"})
                or type(value["revision"]) is not int or not 1 <= value["revision"] <= 0xFFFFFFFF
                or value["profile"] not in PROFILES or value["media"] not in {"managed", "disabled"}
                or ("tls" in value and value["tls"] not in TLS)):
            raise _CommandError()
        if value["revision"] != self.settings["revision"] or self.settings["revision"] == 0xFFFFFFFF:
            raise _CommandError("REVISION_CONFLICT")
        if self.call_state not in {"idle", "terminated"}:
            raise _CommandError()
        candidate = {"revision": self.settings["revision"] + 1,
                     "profile": value["profile"], "media": value["media"],
                     "tls": self.settings["tls"] if value["profile"] == "private_lab" else "required"}
        try:
            self._store.save(candidate)
        except (OSError, PersistenceError) as exc:
            if getattr(exc, "committed", False):
                self.settings = candidate
                raise _CommandError("PERSISTENCE_UNCERTAIN") from exc
            raise _CommandError() from exc
        self.settings = candidate
        if self.faults.persistence_uncertain:
            raise _CommandError("PERSISTENCE_UNCERTAIN")
        return dict(candidate)

    def _originate(self, value: dict) -> None:
        if (set(value) != {"profile", "layout", "meeting_id", "passcode", "host_key", "dial_code"}
                or not all(type(item) is str for item in value.values())
                or value["profile"] not in PROFILES
                or value["layout"] not in {"gallery", "full_screen", "dual_video"}
                or not re.fullmatch(r"[0-9]{1,16}", value["meeting_id"])
                or not re.fullmatch(r"[0-9]{0,32}", value["passcode"])
                or not re.fullmatch(r"[0-9]{0,16}", value["host_key"])
                or not re.fullmatch(r"[A-Za-z0-9]{0,64}", value["dial_code"])):
            raise _CommandError()
        if self.settings["media"] != "managed":
            raise _CommandError("PERMISSION_DENIED")
        if self.call_state not in {"idle", "terminated"} or value["profile"] != self.settings["profile"]:
            raise _CommandError()
        self.video_transmit_enabled, self.audio_muted = True, False
        self._transition("resolving")
        self._schedule(("inviting", "early", "establishing_media", "established"))

    def _media(self, value: dict, length: int) -> None:
        if (length > 48 or not all(type(item) is str for item in value.values())
                or self.call_state != "established" or self.settings["media"] != "managed"):
            raise _CommandError()
        if set(value) == {"mode"} and length <= 32 and value["mode"] in {"enabled", "disabled"}:
            self.video_transmit_enabled = value["mode"] == "enabled"
        elif set(value) == {"action", "value"}:
            if value["action"] == "audio_mute" and value["value"] in {"enabled", "disabled"}:
                self.audio_muted = value["value"] == "enabled"
            elif value["action"] not in {"keyframe", "layout_next"} or value["value"] != "request":
                raise _CommandError()
        else:
            raise _CommandError()


class _CommandError(Exception):
    def __init__(self, code: str = "COMMAND_FAILED") -> None:
        self.code = code


@dataclass
class _Client:
    socket: socket.socket
    deadline: float
    request: bytearray = field(default_factory=bytearray)
    reply: bytes = b""
    offset: int = 0
    ready_at: float = 0


def serve(fd: int, *, state_file: str | Path | None = None,
          daemon: SyntheticDaemon | None = None, faults: FaultOptions | None = None,
          step_seconds: float = 0.1, stop_event=None,
          maximum_requests: int | None = None) -> int:
    """Serve inherited AF_UNIX connection/listener; return completed reply count.

    Caller keeps ownership of fd. A connected FD serves one exchange; a listener
    accepts bounded clients until stop_event or maximum_requests is reached.
    No socket path is created or removed. Delay faults schedule replies without
    blocking other clients. Transport/parser failures close only that client.
    """
    if type(fd) is not int or fd < 0:
        raise ValueError("fd must be a nonnegative integer")
    if maximum_requests is not None and (type(maximum_requests) is not int or maximum_requests < 1):
        raise ValueError("maximum_requests must be positive")
    model = daemon if daemon is not None else SyntheticDaemon(state_file, faults=faults, step_seconds=step_seconds)
    if daemon is not None and faults is not None:
        raise ValueError("faults belong to supplied daemon")
    original_blocking = os.get_blocking(fd)
    duplicate = os.dup(fd)
    try:
        inherited = socket.socket(fileno=duplicate)
    except OSError:
        os.close(duplicate)
        raise ValueError("fd must be a Unix socket") from None
    clients: dict[int, _Client] = {}
    completed = 0
    try:
        kind = inherited.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE)
        if inherited.family != socket.AF_UNIX or kind not in (socket.SOCK_STREAM, getattr(socket, "SOCK_SEQPACKET", -1)):
            raise ValueError("fd must be an AF_UNIX STREAM or SEQPACKET socket")
        packet = kind == getattr(socket, "SOCK_SEQPACKET", -1)
        try:
            inherited.getpeername()
            listening = False
        except OSError as exc:
            if exc.errno != errno.ENOTCONN:
                raise
            # macOS does not implement SO_ACCEPTCONN on socketpairs. An
            # unconnected socket is checked by accept() when readiness arrives.
            if not inherited.getsockname():
                raise ValueError("fd must be connected or listening") from None
            try:
                accepting = inherited.getsockopt(socket.SOL_SOCKET, socket.SO_ACCEPTCONN)
            except OSError as option_error:
                if option_error.errno != errno.ENOPROTOOPT:
                    raise
            else:
                if not accepting:
                    raise ValueError("fd must be connected or listening")
            listening = True
        inherited.setblocking(False)
        with selectors.DefaultSelector() as selector:
            def add(connection: socket.socket) -> None:
                connection.setblocking(False)
                client = _Client(connection, time.monotonic() + 1)
                clients[connection.fileno()] = client
                selector.register(connection, selectors.EVENT_READ, client)

            def close(client: _Client) -> None:
                clients.pop(client.socket.fileno(), None)
                try:
                    selector.unregister(client.socket)
                except KeyError:
                    pass
                try:
                    client.socket.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                client.socket.close()

            if listening:
                selector.register(inherited, selectors.EVENT_READ, None)
            else:
                add(inherited)
            while (stop_event is None or not stop_event.is_set()):
                now = time.monotonic()
                model.tick()
                for client in list(clients.values()):
                    if not client.reply and now >= client.deadline:
                        close(client)
                if not listening and not clients:
                    break
                if maximum_requests is not None and completed >= maximum_requests:
                    break
                for key, events in selector.select(0.02):
                    if key.data is None:
                        try:
                            connection, _ = inherited.accept()
                        except (BlockingIOError, InterruptedError):
                            continue
                        if len(clients) >= 8:
                            connection.close()
                        else:
                            add(connection)
                        continue
                    client = key.data
                    try:
                        if events & selectors.EVENT_READ:
                            data, _, flags, _ = client.socket.recvmsg(HEADER.size + MAX_REQUEST + 1)
                            if not data or flags & getattr(socket, "MSG_TRUNC", 0):
                                close(client)
                                continue
                            client.request.extend(data)
                            if len(client.request) > HEADER.size + MAX_REQUEST:
                                raise ValueError("request exceeds bound")
                            if len(client.request) < HEADER.size:
                                if packet:
                                    raise ValueError("truncated packet")
                                continue
                            length = HEADER.unpack_from(client.request)[-1]
                            if length > MAX_REQUEST:
                                raise ValueError("payload exceeds bound")
                            expected = HEADER.size + length
                            if len(client.request) < expected and not packet:
                                continue
                            request = Frame.decode(bytes(client.request))
                            client.reply = model.handle(request).encode()
                            client.request.clear()
                            client.ready_at = time.monotonic() + model.faults.reply_delay
                            client.deadline = client.ready_at + 1
                            # Unregister during delay: an EOF must not spin on READ.
                            selector.modify(client.socket, selectors.EVENT_WRITE, client)
                            if model.faults.reply_delay:
                                selector.unregister(client.socket)
                                # Register again below once the delay is due.
                        elif events & selectors.EVENT_WRITE:
                            sent = client.socket.send(client.reply[client.offset:])
                            if sent <= 0 or (packet and sent != len(client.reply)):
                                raise OSError("short response")
                            client.offset += sent
                            if client.offset == len(client.reply):
                                completed += 1
                                close(client)
                                if maximum_requests is not None and completed >= maximum_requests:
                                    break
                    except (BlockingIOError, InterruptedError):
                        continue
                    except (OSError, ValueError):
                        close(client)
                # Re-register delayed responses only when due; each reply has a
                # bounded post-delay write deadline even if the peer stops reading.
                now = time.monotonic()
                for client in list(clients.values()):
                    if now >= client.deadline:
                        close(client)
                    elif client.reply and now >= client.ready_at:
                        try:
                            selector.get_key(client.socket)
                        except KeyError:
                            selector.register(client.socket, selectors.EVENT_WRITE, client)
            for client in list(clients.values()):
                client.socket.close()
    finally:
        for client in clients.values():
            client.socket.close()
        inherited.close()
        os.set_blocking(fd, original_blocking)
    return completed
