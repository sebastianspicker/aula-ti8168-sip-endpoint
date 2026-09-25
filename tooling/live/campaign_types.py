"""Shared contracts for the physical LS-200 campaign modules."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
LIVE_TARGET = ROOT / "deployment/targets/ls200"
PAYLOAD = ROOT / ".work/dist/ls200-live/runtime"
MUTATING_ACTIONS = frozenset(
    {"install", "smoke", "soak-15", "soak-30", "soak-60", "reboot", "remove"}
)
SAFE_ENV = {"LANG": "C", "LC_ALL": "C", "PATH": "/usr/bin:/bin:/usr/sbin:/sbin"}
SECRET_PATTERN = re.compile(
    r"(?i)(authorization|cookie|password|passwd|bootstrap|credential|private[-_ ]?key|set-cookie)"
)
CONTROL_IDENTITY_REQUEST = bytes.fromhex("55 f0 04 01 67 47 4d 0d")
CONTROL_IDENTITY_RESPONSE = bytes.fromhex("55 f0 0a 01 06 47 4d 4c 53 2d 32 30 30 0d")
DTMF_SETTLE_SECONDS = 0.25
MEDIA_SETTLE_SECONDS = 3.0
PEER_STARTUP_TIMEOUT_SECONDS = 10.0
MAX_PEER_TIMEOUT_SECONDS = 65 * 60
PEER_READY_SIGNAL = b"R"
WITHHELD_FINAL_RECONNECT_TIMEOUT_SECONDS = 60.0
WITHHELD_FINAL_RECONNECT_POLL_SECONDS = 0.25
ACTIVE_CALL_STATUS_KEYS = frozenset({
    "call_state", "media_state", "revision", "rx_rendering", "reconnect_attempts",
    "video_transmit_enabled", "audio_muted", "renderer", "aec", "sip", "video",
    "audio", "last_error",
})
INITIAL_CALL_STATES = frozenset({"inviting", "early"})
RETRY_EVIDENCE_CALL_STATES = frozenset({"backing_off", "resolving", "inviting", "early"})
BOOT_ID_PATTERN = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"
)
BOOT_ID_COMMAND = (
    "set -eu; boot=/proc/sys/kernel/random/boot_id; "
    "[ -r \"$boot\" ] && [ ! -L \"$boot\" ]; "
    "value=$(cat \"$boot\"); printf '%s\\n' \"$value\""
)


class CampaignError(RuntimeError):
    """An authorization, topology, target, or evidence gate failed closed."""


class PeerCompletionError(CampaignError):
    """A peer failed after producing a validated, private-only result."""

    def __init__(self, result: dict[str, Any]) -> None:
        super().__init__("private SIP/media functional matrix did not complete")
        self.result = result
