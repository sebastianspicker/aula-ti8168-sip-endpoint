#!/usr/bin/env python3
"""Strict offline parser for an owned TI8168 media-board session file."""

from __future__ import annotations

import ipaddress
import json
import os
import re
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import receipts


SCHEMA = "aula-ti8168-sip-endpoint-live-session-v1"
TARGET_INTERFACE = "en5"
TARGET_ROLE = "owned-physical-aula-private-lab"
TARGET_IDENTITY = "private-identity-provider-unavailable"
ALLOWED_ACTIONS = frozenset(
    {"preflight", "install", "smoke", "soak-15", "soak-30", "soak-60", "reboot", "remove"}
)
FORBIDDEN_FIELD_NAMES = frozenset(
    {
        "password",
        "passwd",
        "private_key",
        "private_key_path",
        "identity_file",
        "ssh_key",
        "credential",
        "credentials",
        "bootstrap_token",
        "authorization_header",
        "cookie",
    }
)
SESSION_ID = re.compile(r"\A[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
USERNAME = re.compile(r"\A[A-Za-z_][A-Za-z0-9_.-]{0,31}\Z")
MAC = re.compile(r"\A(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\Z")
FINGERPRINT = re.compile(r"\ASHA256:[A-Za-z0-9+/]{20,64}={0,2}\Z")
VERSION = re.compile(r"\A[A-Za-z0-9][A-Za-z0-9._-]{0,63}\Z")
REPOSITORY = Path(__file__).resolve().parents[2]
PRIVATE_ROOTS = (REPOSITORY / ".work",)


class SessionError(ValueError):
    """The live session is absent, ambiguous, or outside the fixed contract."""


def _secure_private_parent(path: Path, root: Path) -> None:
    """Reject links and writable/private-permission drift before opening a session."""
    try:
        root_info = root.lstat()
    except OSError as error:
        raise SessionError("LIVE_SESSION private root is unavailable") from error
    if (
        not stat.S_ISDIR(root_info.st_mode)
        or stat.S_ISLNK(root_info.st_mode)
        or root_info.st_uid != os.getuid()
        or stat.S_IMODE(root_info.st_mode) & 0o022
    ):
        raise SessionError("LIVE_SESSION private root is unsafe")
    try:
        relative = path.parent.relative_to(root)
    except ValueError as error:
        raise SessionError("LIVE_SESSION escaped its private root") from error
    current = root
    for component in relative.parts:
        current /= component
        try:
            info = current.lstat()
        except OSError as error:
            raise SessionError("LIVE_SESSION private parent is unavailable") from error
        if (
            not stat.S_ISDIR(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.getuid()
            or stat.S_IMODE(info.st_mode) != 0o700
        ):
            raise SessionError("LIVE_SESSION private parent must be operator-owned 0700 without symlinks")


def _object(value: Any, label: str, keys: set[str]) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != keys:
        raise SessionError(f"{label} must contain exactly: {', '.join(sorted(keys))}")
    return value


def _integer(value: Any, label: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
        raise SessionError(f"{label} is outside the approved range")
    return value


def _reject_secret_fields(value: Any, path: str = "session") -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if not isinstance(key, str):
                raise SessionError(f"{path} contains a non-string field name")
            if key.lower() in FORBIDDEN_FIELD_NAMES:
                raise SessionError(f"forbidden credential or private-key field: {path}.{key}")
            _reject_secret_fields(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            _reject_secret_fields(child, f"{path}[{index}]")


@dataclass(frozen=True)
class LiveSession:
    path: Path
    session_id: str
    operator_name: str
    recovery_authorized: bool
    target_ipv4: str
    interface: str
    host_ipv4: str
    expected_mac: str
    ssh_user: str
    ssh_port: int
    ssh_fingerprint: str
    peer_sip_port: int
    peer_media_min: int
    peer_media_max: int
    target_media_min: int
    target_media_max: int
    version_a: str
    version_b: str
    approved_actions: frozenset[str]
    limits: dict[str, int]

    def require_action(self, action: str) -> None:
        if action not in self.approved_actions:
            raise SessionError(f"action is not approved by this session: {action}")


def _session_path(path_text: str | None) -> tuple[Path, Path]:
    if not path_text:
        raise SessionError("LIVE_SESSION must name an absolute session file")
    path = Path(path_text)
    if not path.is_absolute():
        raise SessionError("LIVE_SESSION must be absolute")
    try:
        resolved_parent = path.parent.resolve(strict=True)
    except OSError as error:
        raise SessionError("LIVE_SESSION parent is unavailable") from error
    matching_root = next((root for root in PRIVATE_ROOTS if resolved_parent == root.resolve() or resolved_parent.is_relative_to(root.resolve())), None)
    if matching_root is None:
        raise SessionError("LIVE_SESSION must remain below .work")
    _secure_private_parent(path, matching_root)
    return path, matching_root


def _read_session_file(path: Path) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor: int | None = None
    try:
        descriptor = os.open(path, flags)
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode):
            raise SessionError("LIVE_SESSION must be a regular non-symlink file")
        if stat.S_IMODE(info.st_mode) != 0o600:
            raise SessionError("LIVE_SESSION must have mode 0600")
        if info.st_uid != os.getuid() or info.st_nlink != 1:
            raise SessionError("LIVE_SESSION must be singly linked and owned by the current operator")
        handle = os.fdopen(descriptor, "rb")
        descriptor = None
        with handle:
            raw = handle.read(32 * 1024 + 1)
        if len(raw) > 32 * 1024:
            raise SessionError("LIVE_SESSION exceeds the bounded schema size")
    except OSError as error:
        raise SessionError("LIVE_SESSION is not valid UTF-8 JSON") from error
    finally:
        if descriptor is not None:
            os.close(descriptor)
    return raw


def _decode_session(raw: bytes) -> Any:
    try:
        return receipts.exact_json(raw, error=SessionError, context="JSON")
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise SessionError("LIVE_SESSION is not valid UTF-8 JSON") from error


def _read_session(path_text: str | None) -> tuple[Path, Any]:
    path, _matching_root = _session_path(path_text)
    raw = _read_session_file(path)
    document = _decode_session(raw)
    return path, document


def _validate_private_unicast_ipv4(value: Any, label: str) -> ipaddress.IPv4Address:
    if not isinstance(value, str):
        raise SessionError(f"{label} must be an IPv4 literal")
    try:
        address = ipaddress.ip_address(value)
    except (TypeError, ValueError) as error:
        raise SessionError(f"{label} must be an IPv4 literal") from error
    if not isinstance(address, ipaddress.IPv4Address):
        raise SessionError(f"{label} must be an IPv4 literal")
    first, second, _third, fourth = address.packed
    is_private_unicast = (
        first == 10
        or (first == 172 and 16 <= second <= 31)
        or (first == 192 and second == 168)
    )
    if not is_private_unicast or fourth in {0, 255}:
        raise SessionError(f"{label} must be a non-special private unicast IPv4 address")
    return address


def _validate_host_address(value: Any, target_ip: ipaddress.IPv4Address) -> str:
    host_ip = _validate_private_unicast_ipv4(value, "target.host_ipv4")
    if host_ip == target_ip:
        raise SessionError("target.host_ipv4 must differ from target.ipv4")
    if host_ip.packed[:3] != target_ip.packed[:3]:
        raise SessionError("target.host_ipv4 must share the isolated target /24")
    return str(host_ip)


def _validate_ssh(value: Any) -> tuple[int, str]:
    ssh = _object(value, "target.ssh", {"user", "port", "ecdsa_sha256"})
    if not isinstance(ssh["user"], str) or not USERNAME.fullmatch(ssh["user"]):
        raise SessionError("target.ssh.user is unsafe")
    ssh_port = _integer(ssh["port"], "target.ssh.port", 1, 65535)
    if ssh_port != 5100:
        raise SessionError("target.ssh.port must remain fixed to 5100")
    if not isinstance(ssh["ecdsa_sha256"], str) or not FINGERPRINT.fullmatch(ssh["ecdsa_sha256"]):
        raise SessionError("target.ssh.ecdsa_sha256 is not a pinned SHA-256 fingerprint")
    return ssh_port, ssh["user"]


def _validate_target(value: Any) -> tuple[str, str, str, int, str]:
    target = _object(
        value, "target",
        {"role_token", "ipv4", "interface", "host_ipv4", "expected_mac", "identity", "ssh"},
    )
    if target["role_token"] != TARGET_ROLE:
        raise SessionError("target role token does not authorize the physical private lab")
    target_ip = _validate_private_unicast_ipv4(target["ipv4"], "target.ipv4")
    if target["interface"] != TARGET_INTERFACE:
        raise SessionError("target must remain bound to the approved private-lab interface")
    if target["identity"] != TARGET_IDENTITY:
        raise SessionError("physical identity provider unavailable in maintained source")
    host_ip = _validate_host_address(target["host_ipv4"], target_ip)
    if not isinstance(target["expected_mac"], str) or not MAC.fullmatch(target["expected_mac"]):
        raise SessionError("target.expected_mac must be an exact six-octet MAC")
    ssh_port, ssh_user = _validate_ssh(target["ssh"])
    return str(target_ip), host_ip, target["expected_mac"].lower(), ssh_port, ssh_user


def _validate_peer(value: Any) -> None:
    peer = _object(value, "peer", {"sip_port", "peer_media", "target_media"})
    peer_media = _object(peer["peer_media"], "peer.peer_media", {"min", "max"})
    target_media = _object(peer["target_media"], "peer.target_media", {"min", "max"})
    if (
        peer["sip_port"] != 15060
        or peer_media != {"min": 15200, "max": 15298}
        or target_media != {"min": 40000, "max": 40100}
    ):
        raise SessionError("peer ports must match the fixed physical-lab allocation")


def _validate_payload(value: Any) -> tuple[str, str]:
    payload = _object(value, "payload", {"version_a", "version_b"})
    for label in ("version_a", "version_b"):
        if not isinstance(payload[label], str) or not VERSION.fullmatch(payload[label]):
            raise SessionError(f"payload.{label} is unsafe")
    if payload["version_a"] == payload["version_b"]:
        raise SessionError("the two releases require distinct version IDs")
    return payload["version_a"], payload["version_b"]


def _validate_actions(value: Any) -> frozenset[str]:
    if (
        not isinstance(value, list)
        or not value
        or any(not isinstance(item, str) for item in value)
        or len(set(value)) != len(value)
        or not set(value) <= ALLOWED_ACTIONS
    ):
        raise SessionError("approved_actions contains an unsupported or duplicate action")
    return frozenset(value)


def _validate_capture_policy(value: Any) -> None:
    capture = _object(
        value, "capture_policy",
        {"retain_sip_bodies", "retain_media_payloads", "packet_hashes", "sanitized_summary"},
    )
    if capture != {
        "retain_sip_bodies": False,
        "retain_media_payloads": False,
        "packet_hashes": True,
        "sanitized_summary": True,
    }:
        raise SessionError("capture_policy must retain only hashes and sanitized summaries")


def _validate_limits(value: Any) -> dict[str, int]:
    limits = _object(
        value, "limits",
        {
            "minimum_available_ram_mib", "managed_rss_mib", "managed_rss_growth_mib",
            "managed_fd_count", "managed_fd_growth", "managed_cpu_percent",
            "managed_cpu_consecutive_samples", "campaign_state_growth_mib", "warmup_minutes",
        },
    )
    expected = {
        "minimum_available_ram_mib": 64, "managed_rss_mib": 128,
        "managed_rss_growth_mib": 8, "managed_fd_count": 256,
        "managed_fd_growth": 8, "managed_cpu_percent": 90,
        "managed_cpu_consecutive_samples": 5, "campaign_state_growth_mib": 8,
        "warmup_minutes": 10,
    }
    if limits != expected:
        raise SessionError("limits must match the fixed campaign resource envelope")
    return dict(limits)


def load_session(path_text: str | None) -> LiveSession:
    path, document = _read_session(path_text)
    _reject_secret_fields(document)
    root = _object(
        document, "session",
        {"schema", "session_id", "operator", "target", "peer", "payload",
         "approved_actions", "capture_policy", "limits"},
    )
    if root["schema"] != SCHEMA:
        raise SessionError("unsupported live-session schema")
    session_id = root["session_id"]
    if not isinstance(session_id, str) or not SESSION_ID.fullmatch(session_id):
        raise SessionError("session_id is unsafe")
    operator = _object(root["operator"], "operator", {"name", "recovery_authorized"})
    if not isinstance(operator["name"], str) or not operator["name"].strip():
        raise SessionError("operator.name is required")
    if operator["recovery_authorized"] is not True:
        raise SessionError("physical recovery authorization is required")
    target_ip, host_ip, expected_mac, ssh_port, ssh_user = _validate_target(root["target"])
    _validate_peer(root["peer"])
    version_a, version_b = _validate_payload(root["payload"])
    actions = _validate_actions(root["approved_actions"])
    _validate_capture_policy(root["capture_policy"])
    limits = _validate_limits(root["limits"])
    return LiveSession(
        path=path,
        session_id=session_id,
        operator_name=operator["name"].strip(),
        recovery_authorized=True,
        target_ipv4=target_ip,
        interface=TARGET_INTERFACE,
        host_ipv4=str(host_ip),
        expected_mac=expected_mac,
        ssh_user=ssh_user,
        ssh_port=ssh_port,
        ssh_fingerprint=root["target"]["ssh"]["ecdsa_sha256"],
        peer_sip_port=15060,
        peer_media_min=15200,
        peer_media_max=15298,
        target_media_min=40000,
        target_media_max=40100,
        version_a=version_a,
        version_b=version_b,
        approved_actions=actions,
        limits=limits,
    )
