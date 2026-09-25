"""Private SIP peer lifecycle and functional call scenarios."""

from __future__ import annotations

import contextlib
import json
import os
import select
import secrets
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Callable

from campaign_evidence import _private_root
from campaign_transport import _api, _run
from campaign_types import (
    ACTIVE_CALL_STATUS_KEYS, DTMF_SETTLE_SECONDS, INITIAL_CALL_STATES,
    MAX_PEER_TIMEOUT_SECONDS, MEDIA_SETTLE_SECONDS, PEER_READY_SIGNAL as _PEER_READY_SIGNAL,
    PEER_STARTUP_TIMEOUT_SECONDS, RETRY_EVIDENCE_CALL_STATES, ROOT,
    WITHHELD_FINAL_RECONNECT_POLL_SECONDS, WITHHELD_FINAL_RECONNECT_TIMEOUT_SECONDS,
    CampaignError, PeerCompletionError, SAFE_ENV,
)
from session import LiveSession

def _stop_peer(peer: subprocess.Popen[bytes]) -> None:
    """Terminate a peer and reap it, escalating when graceful shutdown times out."""
    if peer.poll() is None:
        peer.terminate()
        try:
            peer.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            peer.kill()
            peer.wait(timeout=5.0)
    output = peer.stdout
    if output is not None:
        output.close()


_stop_unready_peer = _stop_peer


def _wait_for_peer_ready(peer: subprocess.Popen[bytes], descriptor: int,
                         timeout: float) -> None:
    readable, _, _ = select.select((descriptor,), (), (), timeout)
    if not readable:
        raise CampaignError("private SIP peer did not become ready before the startup deadline")
    try:
        signal = os.read(descriptor, len(_PEER_READY_SIGNAL) + 1)
    except OSError as error:
        raise CampaignError("private SIP peer startup handshake failed") from error
    if signal != _PEER_READY_SIGNAL or peer.poll() is not None:
        raise CampaignError("private SIP peer startup handshake failed")


def _peer_process(session: LiveSession, timeout_seconds: int, *extra: str,
                  require_bidirectional: bool = True) -> subprocess.Popen[bytes]:
    if type(timeout_seconds) is not int or not 1 <= timeout_seconds <= MAX_PEER_TIMEOUT_SECONDS:
        raise CampaignError(
            f"private SIP peer timeout must be an integer between 1 and {MAX_PEER_TIMEOUT_SECONDS} seconds"
        )
    if any(type(option) is not str or not option or "\0" in option for option in extra):
        raise CampaignError("private SIP peer options must be non-empty text")
    try:
        ready_read, ready_write = os.pipe()
    except OSError as error:
        raise CampaignError("private SIP peer startup handshake could not be created") from error
    argv = [
        sys.executable, str(ROOT / "lab/sip-peer/run_private_sip_peer.py"),
        "--bind-address", session.host_ipv4, "--advertised-address", session.host_ipv4,
        "--authorized-source-address", session.target_ipv4, "--sip-port", str(session.peer_sip_port),
        "--timeout", str(timeout_seconds), "--ready-fd", str(ready_write), *extra,
    ]
    if require_bidirectional:
        argv.append("--require-bidirectional")
    try:
        try:
            peer = subprocess.Popen(
                argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                env=SAFE_ENV, pass_fds=(ready_write,),
            )
        except OSError as error:
            with contextlib.suppress(OSError):
                os.close(ready_read)
            raise CampaignError("private SIP peer could not start") from error
        except BaseException:
            with contextlib.suppress(OSError):
                os.close(ready_read)
            raise
    finally:
        with contextlib.suppress(OSError):
            os.close(ready_write)
    try:
        _wait_for_peer_ready(peer, ready_read, min(PEER_STARTUP_TIMEOUT_SECONDS, float(timeout_seconds)))
    except BaseException:
        _stop_peer(peer)
        raise
    finally:
        os.close(ready_read)
    return peer


def _finish_peer(peer: subprocess.Popen[bytes], timeout: float) -> dict[str, Any]:
    try:
        output, _ = peer.communicate(timeout=timeout)
    except BaseException:
        _stop_peer(peer)
        raise
    try:
        result = json.loads(output)
    except (json.JSONDecodeError, UnicodeDecodeError):
        raise CampaignError("private SIP peer result was malformed") from None
    forbidden = {"sip_body", "sdp", "media_payload", "packet_payload"}
    if not isinstance(result, dict) or forbidden & set(result):
        raise CampaignError("peer result retained forbidden protocol or media content")
    if peer.returncode != 0:
        raise PeerCompletionError(result)
    return result


def _request_call(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
    *, api: Callable[..., tuple[dict[str, Any], str | None]] = _api,
) -> None:
    api(session, certificate, "POST", "/calls", {
        "meeting_id": "100000001", "profile": "private_lab", "passcode": "",
        "layout": "full_screen", "host_key": "", "dial_code": "",
    }, cookie=cookie, csrf=csrf)


def _short_call(session: LiveSession, certificate: Path, cookie: str, csrf: str) -> dict[str, Any]:
    peer = _peer_process(session, 180)
    try:
        _request_call(session, certificate, cookie, csrf)
        time.sleep(MEDIA_SETTLE_SECONDS)
        _api(session, certificate, "DELETE", "/calls/active", {}, cookie=cookie, csrf=csrf)
        return _finish_peer(peer, 180.0)
    except BaseException:
        _stop_peer(peer)
        raise


def _signaling_only_call(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
) -> dict[str, Any]:
    peer = _peer_process(session, 180, "--signaling-only", require_bidirectional=False)
    try:
        _request_call(session, certificate, cookie, csrf)
        time.sleep(1.0)
        _api(session, certificate, "DELETE", "/calls/active", {}, cookie=cookie, csrf=csrf)
        result = _finish_peer(peer, 180.0)
    except BaseException:
        _stop_peer(peer)
        raise
    media_keys = ("outbound_video_packets", "outbound_audio_packets", "inbound_video_packets", "inbound_audio_packets")
    if any(result.get(key) != 0 for key in media_keys):
        raise CampaignError("signaling-only call unexpectedly exchanged media")
    return result


def _local_hangup_media_call(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
    *, peer_process: Callable[..., subprocess.Popen[bytes]] = _peer_process,
    request_call: Callable[..., None] = _request_call,
    api: Callable[..., tuple[dict[str, Any], str | None]] = _api,
    finish_peer: Callable[..., dict[str, Any]] = _finish_peer,
) -> dict[str, Any]:
    peer = peer_process(session, 600)
    try:
        request_call(session, certificate, cookie, csrf)
        time.sleep(MEDIA_SETTLE_SECONDS)
        dtmf, _ = api(session, certificate, "POST", "/calls/active/dtmf", {"tone": "#"}, cookie=cookie, csrf=csrf)
        if set(dtmf) != {"ok"} or dtmf["ok"] is not True:
            raise CampaignError("DTMF # was not accepted by the exact API response schema")
        time.sleep(DTMF_SETTLE_SECONDS)
        api(session, certificate, "DELETE", "/calls/active", {}, cookie=cookie, csrf=csrf)
        result = finish_peer(peer, 600.0)
        tones = result.get("dtmf_tones")
        if (
            not isinstance(tones, list)
            or any(type(tone) is not int for tone in tones)
            or 11 not in tones
            or result.get("bye_seen") is not True
            or not isinstance(result.get("dialogs_completed"), int)
            or result["dialogs_completed"] < 1
        ):
            raise CampaignError("DTMF # RFC4733 event 11 and completed local hangup were not proven")
        return result
    except BaseException:
        _stop_peer(peer)
        raise


def _wait_for_withheld_final_reconnect_established(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
    *, api: Callable[..., tuple[dict[str, Any], str | None]] = _api,
) -> None:
    deadline = time.monotonic() + WITHHELD_FINAL_RECONNECT_TIMEOUT_SECONDS
    initial_generation_seen = False
    retry_evidence_seen = False
    while True:
        status, _ = api(session, certificate, "GET", "/calls/active", {}, cookie=cookie, csrf=csrf)
        if (
            set(status) != ACTIVE_CALL_STATUS_KEYS
            or not isinstance(status.get("call_state"), str)
            or type(status.get("reconnect_attempts")) is not int
        ):
            raise CampaignError("active call status response was incomplete or malformed")
        call_state = status["call_state"]
        reconnect_attempts = status["reconnect_attempts"]
        if reconnect_attempts < 0:
            raise CampaignError("active call status response was incomplete or malformed")
        if not initial_generation_seen:
            initial_generation_seen = (
                call_state in INITIAL_CALL_STATES and reconnect_attempts == 0
            )
        elif not retry_evidence_seen:
            retry_evidence_seen = (
                call_state in RETRY_EVIDENCE_CALL_STATES and reconnect_attempts >= 1
            )
        elif call_state == "established":
            return
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise CampaignError("withheld-final reconnect did not establish before the host deadline")
        time.sleep(min(WITHHELD_FINAL_RECONNECT_POLL_SECONDS, remaining))


def _withheld_final_reconnect(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
    *, peer_process: Callable[..., subprocess.Popen[bytes]] = _peer_process,
    request_call: Callable[..., None] = _request_call,
    wait_for_established: Callable[..., None] = _wait_for_withheld_final_reconnect_established,
    api: Callable[..., tuple[dict[str, Any], str | None]] = _api,
    finish_peer: Callable[..., dict[str, Any]] = _finish_peer,
) -> dict[str, Any]:
    peer = peer_process(
        session, 180, "--withhold-first-invite-final", "--withheld-final-timeout", "10"
    )
    try:
        request_call(session, certificate, cookie, csrf)
        wait_for_established(session, certificate, cookie, csrf, api=api)
        time.sleep(MEDIA_SETTLE_SECONDS)
        api(session, certificate, "DELETE", "/calls/active", {}, cookie=cookie, csrf=csrf)
        result = finish_peer(peer, 180.0)
    except BaseException:
        _stop_peer(peer)
        raise
    if result.get("withheld_final_invites") != 1 or result.get("dialogs_started", 0) < 2:
        raise CampaignError("withheld-final reconnect evidence was incomplete")
    return result


def _remote_hangup_call(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
) -> dict[str, Any]:
    peer = _peer_process(session, 180, "--peer-initiated-hangup")
    try:
        _request_call(session, certificate, cookie, csrf)
        result = _finish_peer(peer, 180.0)
    except BaseException:
        with contextlib.suppress(CampaignError):
            _api(session, certificate, "DELETE", "/calls/active", {}, cookie=cookie, csrf=csrf)
        _stop_peer(peer)
        raise
    if result.get("peer_bye_sent") != 1 or result.get("peer_bye_200_seen") != 1:
        raise CampaignError("remote hangup evidence was incomplete")
    return result
