#!/usr/bin/env python3
"""Offline contracts for a physical TI8168 media-board private-lab campaign."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import getpass
import hashlib
import http.client
import json
import os
import re
import resource
import select
import secrets
import socket
import ssl
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import time
import uuid
from pathlib import Path
from typing import Any, Iterator, Sequence

from session import ALLOWED_ACTIONS, LiveSession, SessionError, load_session



from campaign_types import (
    ACTIVE_CALL_STATUS_KEYS, BOOT_ID_PATTERN, BOOT_ID_COMMAND as _BOOT_ID_COMMAND,
    DTMF_SETTLE_SECONDS,
    INITIAL_CALL_STATES, LIVE_TARGET, MAX_PEER_TIMEOUT_SECONDS, MEDIA_SETTLE_SECONDS,
    MUTATING_ACTIONS, PAYLOAD, PEER_STARTUP_TIMEOUT_SECONDS, PEER_READY_SIGNAL as _PEER_READY_SIGNAL,
    RETRY_EVIDENCE_CALL_STATES, ROOT, SAFE_ENV, SECRET_PATTERN,
    WITHHELD_FINAL_RECONNECT_POLL_SECONDS, WITHHELD_FINAL_RECONNECT_TIMEOUT_SECONDS,
    CampaignError, PeerCompletionError, require_private_identity_provider as _require_private_identity_provider,
)
from campaign_evidence import (
    PhaseEvidence, _fsync_directory, _journal_root, _new_path, _private_root,
    _publish, _reconcile_journal, _record_journal, _secure_directory, _session_root,
    _write_json_new, _session_lock,
)
import campaign_transport as _transport
import campaign_calls as _calls
import campaign_actions as _actions
from campaign_transfer import (
    _payload_kib, _snapshot_transfer_archive, _tar_transfer, _transfer_member_name,
    _verified_payload_digest,
)

_run = _transport._run
_ssh_options = _transport._ssh_options
_ssh = _transport._ssh
_control_identity = _transport._control_identity
_verify_identity = _transport._verify_identity


_reboot_preflight_command = _transport._reboot_preflight_command
_reboot_postboot_command = _transport._reboot_postboot_command
_certificate = _transport._certificate
_PinnedHTTPSConnection = _transport._PinnedHTTPSConnection
_temporary_console_password = _transport._temporary_console_password

_stop_unready_peer = _calls._stop_unready_peer
_wait_for_peer_ready = _calls._wait_for_peer_ready
_peer_process = _calls._peer_process
_finish_peer = _calls._finish_peer
_short_call = _calls._short_call
_signaling_only_call = _calls._signaling_only_call
_remote_hangup_call = _calls._remote_hangup_call
_wait_for_withheld_final_reconnect_established = _calls._wait_for_withheld_final_reconnect_established


def _scan_host_key(session: LiveSession, directory: Path) -> tuple[Path, list[str]]:
    return _transport._scan_host_key(session, directory, runner=_run)


@contextlib.contextmanager
def _ssh_master(session: LiveSession) -> Iterator[tuple[Path, Path]]:
    with _transport._ssh_master(
        session, topology=_local_topology, scan_host_key=_scan_host_key,
    ) as connection:
        yield connection


def _authenticated_boot_id(session: LiveSession, known: Path, control: Path) -> str:
    return _transport._authenticated_boot_id(session, known, control, ssh=_ssh)


def _remote_baseline(session: LiveSession, known: Path, control: Path) -> dict[str, str]:
    return _transport._remote_baseline(session, known, control, ssh=_ssh)


def _api(
    session: LiveSession, certificate: Path, method: str, path: str,
    body: dict[str, Any], *, cookie: str | None = None, csrf: str | None = None,
) -> tuple[dict[str, Any], str | None]:
    return _transport._api(
        session, certificate, method, path, body, cookie=cookie, csrf=csrf,
        connection_class=_PinnedHTTPSConnection,
    )


def _login_console(session: LiveSession, certificate: Path, password: bytearray) -> tuple[str, str]:
    return _transport._login_console(session, certificate, password, api=_api)


def _refresh_console_session(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
) -> tuple[str, str]:
    return _transport._refresh_console_session(session, certificate, cookie, csrf, api=_api)


def _establish_console(
    session: LiveSession, known: Path, control: Path, certificate: Path, password: bytearray,
) -> tuple[str, str]:
    return _transport._establish_console(
        session, known, control, certificate, password, ssh=_ssh, api=_api,
    )


def _request_call(session: LiveSession, certificate: Path, cookie: str, csrf: str) -> None:
    _calls._request_call(session, certificate, cookie, csrf, api=_api)


def _local_hangup_media_call(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
) -> dict[str, Any]:
    return _calls._local_hangup_media_call(
        session, certificate, cookie, csrf, peer_process=_peer_process,
        request_call=_request_call, api=_api, finish_peer=_finish_peer,
    )


def _withheld_final_reconnect(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
) -> dict[str, Any]:
    return _calls._withheld_final_reconnect(
        session, certificate, cookie, csrf, peer_process=_peer_process,
        request_call=_request_call,
        wait_for_established=_wait_for_withheld_final_reconnect_established,
        api=_api, finish_peer=_finish_peer,
    )

def require_confirmations(action: str, environ: dict[str, str] | os._Environ[str]) -> None:
    if action in MUTATING_ACTIONS and environ.get("LIVE_CONFIRM") != "YES":
        raise CampaignError("mutation requires exact LIVE_CONFIRM=YES")
    if action == "reboot" and environ.get("LIVE_REBOOT_CONFIRM") != "GRACEFUL":
        raise CampaignError("graceful reboot requires exact LIVE_REBOOT_CONFIRM=GRACEFUL")
    if action == "remove" and environ.get("LIVE_PURGE_CONFIRM") != "PURGE":
        raise CampaignError("owned-state purge requires exact LIVE_PURGE_CONFIRM=PURGE")


def _normalize_mac(value: str) -> str:
    octets = value.lower().split(":")
    if len(octets) != 6 or any(not re.fullmatch(r"[0-9a-f]{1,2}", octet) for octet in octets):
        return ""
    return "".join(f"{int(octet, 16):02x}" for octet in octets)


def _local_topology(session: LiveSession, *, contact: bool) -> None:
    if sys.platform != "darwin":
        raise CampaignError("physical campaign orchestration is supported only on macOS")
    route = _run(["/sbin/route", "-n", "get", session.target_ipv4]).stdout.decode("ascii", "strict")
    match = re.search(r"^\s*interface:\s*(\S+)\s*$", route, re.MULTILINE)
    if not match or match.group(1) != session.interface:
        raise CampaignError("target route drifted from the authorized interface")
    interface = _run(["/sbin/ifconfig", session.interface]).stdout.decode("ascii", "strict")
    if not re.search(rf"\binet\s+{re.escape(session.host_ipv4)}\b", interface):
        raise CampaignError("the authorized host IPv4 is not present on en5")
    if not contact:
        return
    _run(["/sbin/ping", "-c", "1", "-W", "1000", session.target_ipv4], timeout=5.0)
    neighbor = _run(["/usr/sbin/arp", "-n", session.target_ipv4]).stdout.decode("ascii", "strict")
    mac_match = re.search(r"\bat\s+(\S+)\s+on\s+", neighbor)
    if not mac_match or _normalize_mac(mac_match.group(1)) != _normalize_mac(session.expected_mac):
        raise CampaignError("neighbor MAC drifted from the authorized Aula")



def _preflight(session: LiveSession, evidence: PhaseEvidence) -> None:
    _actions.preflight(
        session, evidence, run=_run, local_topology=_local_topology, ssh_master=_ssh_master,
        verify_identity=_verify_identity, remote_baseline=_remote_baseline,
        payload_kib=_payload_kib, ssh=_ssh,
    )


def _install(session: LiveSession, evidence: PhaseEvidence) -> None:
    _actions.install(
        session, evidence, verified_payload_digest=_verified_payload_digest,
        certificate=_certificate, local_topology=_local_topology, ssh_master=_ssh_master,
        verify_identity=_verify_identity, tar_transfer=_tar_transfer, ssh=_ssh,
    )


def evaluate_sample(sample: dict[str, int], limits: dict[str, int], *, warm: dict[str, int] | None,
                    cpu_streak: int) -> int:
    if sample["available_ram_mib"] < limits["minimum_available_ram_mib"]:
        raise CampaignError("available RAM fell below the approved floor")
    if sample["rss_mib"] > limits["managed_rss_mib"] or sample["fds"] > limits["managed_fd_count"]:
        raise CampaignError("managed RSS or FD absolute limit was exceeded")
    if warm is not None:
        if sample["rss_mib"] - warm["rss_mib"] > limits["managed_rss_growth_mib"]:
            raise CampaignError("managed RSS growth exceeded the warm-up envelope")
        if sample["fds"] - warm["fds"] > limits["managed_fd_growth"]:
            raise CampaignError("managed FD growth exceeded the warm-up envelope")
        if sample["state_mib"] - warm["state_mib"] > limits["campaign_state_growth_mib"]:
            raise CampaignError("campaign JFFS2 growth exceeded the settled envelope")
    cpu_streak = cpu_streak + 1 if sample["cpu_percent"] >= limits["managed_cpu_percent"] else 0
    if cpu_streak >= limits["managed_cpu_consecutive_samples"]:
        raise CampaignError("managed CPU remained above the approved limit")
    if sample["vendor_health"] != 1:
        raise CampaignError("vendor or kernel/filesystem health regressed")
    return cpu_streak


def _cpu_percent(previous_ticks: int, previous_time: float, current_ticks: int,
                 current_time: float, ticks_per_second: int) -> int:
    elapsed = current_time - previous_time
    if current_ticks < previous_ticks or ticks_per_second <= 0 or elapsed <= 0:
        raise CampaignError("managed CPU counters regressed or were invalid")
    return int(100 * (current_ticks - previous_ticks) / (ticks_per_second * elapsed))


def _sample(session: LiveSession, known: Path, control: Path) -> dict[str, int]:
    command = (
        "set -eu; pids=$(cat /var/run/aula-ti8168-sip-endpoint/*.pid 2>/dev/null | tr '\\n' ' '); "
        "[ -n \"$pids\" ] || exit 1; rss=0; fds=0; cpu_ticks=0; for p in $pids; do [ -d /proc/$p ] || exit 1; "
        "r=$(awk '/VmRSS:/{print int($2/1024); exit}' /proc/$p/status); rss=$((rss+r)); "
        "f=$(find /proc/$p/fd -mindepth 1 -maxdepth 1 | wc -l); [ $f -gt $fds ] && fds=$f; "
        "t=$(awk '{print $14+$15}' /proc/$p/stat); cpu_ticks=$((cpu_ticks+t)); done; "
        "config=$(/bin/zcat /proc/config.gz) || exit 1; "
        "clk=$(printf '%s\\n' \"$config\" | awk '/^CONFIG_HZ=/ { "
        "if ($0 !~ /^CONFIG_HZ=[1-9][0-9]*$/) invalid=1; "
        "else if (seen) invalid=1; else { seen=1; hz=substr($0, 11) } } "
        "END { if (invalid || seen != 1) exit 1; print hz }') || exit 1; "
        "case $clk in ''|*[!0-9]*|0) exit 1 ;; esac; "
        "available_kib=$(awk '$1 == \"MemAvailable:\" { if ($2 !~ /^[0-9]+$/ || available_seen++) invalid=1; "
        "else available=$2 } $1 == \"MemFree:\" { if ($2 !~ /^[0-9]+$/ || free_seen++) invalid=1; else free=$2 } "
        "$1 == \"Buffers:\" { if ($2 !~ /^[0-9]+$/ || buffers_seen++) invalid=1; else buffers=$2 } "
        "$1 == \"Cached:\" { if ($2 !~ /^[0-9]+$/ || cached_seen++) invalid=1; else cached=$2 } "
        "$1 == \"SReclaimable:\" { if ($2 !~ /^[0-9]+$/ || reclaim_seen++) invalid=1; else reclaim=$2 } "
        "$1 == \"Shmem:\" { if ($2 !~ /^[0-9]+$/ || shmem_seen++) invalid=1; else shmem=$2 } "
        "END { if (invalid) exit 1; if (available_seen) { if (available <= 0) exit 1; print available; exit } "
        "if (free_seen != 1 || buffers_seen != 1 || cached_seen != 1 || reclaim_seen != 1 || shmem_seen != 1) exit 1; "
        "available=free+buffers+cached+reclaim-shmem; if (available <= 0) exit 1; print available }' /proc/meminfo) || exit 1; "
        "case $available_kib in ''|*[!0-9]*|0) exit 1 ;; esac; available_ram_mib=$((available_kib / 1024)); "
        "vendor=1; [ -x /usr/share/media/wait_media_ready ] || vendor=0; "
        "vendor_gate_pid=; if [ $vendor -eq 1 ]; then LD_LIBRARY_PATH=/usr/lib/cbox "
        "/usr/share/media/wait_media_ready >/dev/null 2>&1 & "
        "vendor_gate_pid=$!; vendor_gate_elapsed=0; while kill -0 $vendor_gate_pid 2>/dev/null; do "
        "if [ $vendor_gate_elapsed -ge 30 ]; then kill $vendor_gate_pid 2>/dev/null || true; "
        "wait $vendor_gate_pid 2>/dev/null || true; vendor=0; vendor_gate_pid=; break; fi; "
        "sleep 1 || { kill $vendor_gate_pid 2>/dev/null || true; wait $vendor_gate_pid 2>/dev/null || true; "
        "vendor=0; vendor_gate_pid=; break; }; vendor_gate_elapsed=$((vendor_gate_elapsed+1)); done; "
        "if [ -n \"$vendor_gate_pid\" ]; then wait $vendor_gate_pid || vendor=0; fi; fi; "
        "[ -d /var/lib/cbox ] && [ -w /var/lib/cbox ] || vendor=0; "
        "for name in media mcu external_device mosquitto dnsmasq nginx; do pidof $name >/dev/null 2>&1 || vendor=0; done; "
        "awk '$2 == \"/\" && $4 ~ /(^|,)ro(,|$)/ {found=1} END {exit !found}' /proc/mounts || vendor=0; "
        "jffs=$(awk '$3 == \"jffs2\" && $4 ~ /(^|,)rw(,|$)/ {count++} END {print count+0}' /proc/mounts); "
        "ext4=$(awk '$3 == \"ext4\" && $4 ~ /(^|,)rw(,|$)/ {count++} END {print count+0}' /proc/mounts); "
        "[ $jffs -gt 0 ] || vendor=0; kernel_errors=$(dmesg | "
        "grep -Eic 'I/O error|EXT4-fs error|JFFS2.*(error|corrupt)|UBIFS error|Out of memory|oom-killer|kernel panic' || true); "
        "network_errors=0; for net in eth0 eth1; do for counter in rx_errors tx_errors rx_crc_errors rx_frame_errors tx_carrier_errors; do "
        "file=/sys/class/net/$net/statistics/$counter; [ ! -r $file ] || { read value < $file; network_errors=$((network_errors+value)); }; done; done; "
        "printf 'available_ram_mib=%s\\nrss_mib=%s\\nfds=%s\\ncpu_ticks=%s\\nclk_tck=%s\\nstate_mib=%s\\nvendor_health=%s\\n"
        "kernel_errors=%s\\nnetwork_errors=%s\\njffs_mounts=%s\\next4_mounts=%s\\n' "
        "\"$available_ram_mib\" \"$rss\" \"$fds\" "
        "\"$cpu_ticks\" \"$clk\" \"$(du -sm /var/lib/cbox/aula-ti8168-sip-endpoint | awk '{print $1}')\" \"$vendor\" "
        "\"$kernel_errors\" \"$network_errors\" \"$jffs\" \"$ext4\""
    )
    output = _ssh(session, known, control, command).decode("ascii", "strict")
    sample: dict[str, int] = {}
    for line in output.splitlines():
        key, separator, value = line.partition("=")
        if not separator or not value.isdigit():
            raise CampaignError("resource sample was malformed")
        sample[key] = int(value)
    expected = {
        "available_ram_mib", "rss_mib", "fds", "cpu_ticks", "clk_tck", "state_mib",
        "vendor_health", "kernel_errors", "network_errors", "jffs_mounts", "ext4_mounts",
    }
    if set(sample) != expected:
        raise CampaignError("resource sample was incomplete")
    return sample


def _health_regressed(sample: dict[str, int], reference: dict[str, int]) -> bool:
    return (
        sample["kernel_errors"] > reference["kernel_errors"]
        or sample["network_errors"] > reference["network_errors"]
        or sample["jffs_mounts"] != reference["jffs_mounts"]
        or sample["ext4_mounts"] != reference["ext4_mounts"]
    )


def _soak(session: LiveSession, evidence: PhaseEvidence, minutes: int) -> None:
    certificate = _private_root(session) / "tls/server.crt"
    if not certificate.is_file() or certificate.is_symlink():
        raise CampaignError("the exact session certificate is absent")
    password = _temporary_console_password(
        "Temporary liveadmin password for this soak (input is not retained): "
    )
    _local_topology(session, contact=True)
    samples: list[dict[str, int]] = []
    cpu_streak = 0
    warm: dict[str, int] | None = None
    prior_ticks: int | None = None
    prior_time: float | None = None
    peer = _peer_process(session, (minutes + 5) * 60)
    try:
        with _ssh_master(session) as (known, control):
            _verify_identity(session, known, control)
            cookie, csrf = _login_console(session, certificate, password)
            _request_call(session, certificate, cookie, csrf)
            try:
                soak_started = time.monotonic()
                seed = _sample(session, known, control)
                prior_ticks = seed.pop("cpu_ticks")
                seed.pop("clk_tck")
                prior_time = time.monotonic()
                seed["cpu_percent"] = 0
                health_reference = {
                    key: seed[key]
                    for key in ("kernel_errors", "network_errors", "jffs_mounts", "ext4_mounts")
                }
                evaluate_sample(seed, session.limits, warm=None, cpu_streak=0)
                for minute in range(minutes):
                    remaining = soak_started + (minute + 1) * 60.0 - time.monotonic()
                    if remaining > 0:
                        time.sleep(remaining)
                    cookie, csrf = _refresh_console_session(session, certificate, cookie, csrf)
                    _ssh(session, known, control, "/var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh health", timeout=40.0)
                    sample = _sample(session, known, control)
                    sampled_at = time.monotonic()
                    ticks = sample.pop("cpu_ticks")
                    ticks_per_second = sample.pop("clk_tck")
                    if prior_ticks is None or prior_time is None:
                        sample["cpu_percent"] = 0
                    else:
                        sample["cpu_percent"] = _cpu_percent(
                            prior_ticks, prior_time, ticks, sampled_at, ticks_per_second
                        )
                    prior_ticks, prior_time = ticks, sampled_at
                    if _health_regressed(sample, health_reference):
                        raise CampaignError("kernel, network, or filesystem health regressed during soak")
                    if minute == session.limits["warmup_minutes"] - 1:
                        warm = sample.copy()
                    cpu_streak = evaluate_sample(
                        sample, session.limits, warm=warm, cpu_streak=cpu_streak
                    )
                    samples.append(sample)
            except BaseException:
                with contextlib.suppress(CampaignError):
                    _api(session, certificate, "DELETE", "/calls/active", {}, cookie=cookie, csrf=csrf)
                raise
            _api(session, certificate, "DELETE", "/calls/active", {}, cookie=cookie, csrf=csrf)
        peer_result = _finish_peer(peer, 180.0)
    finally:
        _calls._stop_peer(peer)
        for index in range(len(password)):
            password[index] = 0
    evidence.private_observations["samples"] = samples
    evidence.private_observations["peer"] = peer_result
    evidence.checks.update({"one_minute_sampling": len(samples) == minutes, "active_private_call": True,
                            "resource_envelope": True, "vendor_health": True})


def _smoke(session: LiveSession, evidence: PhaseEvidence) -> None:
    manifest_digest = _verified_payload_digest()
    certificate = _private_root(session) / "tls/server.crt"
    if not certificate.is_file() or certificate.is_symlink():
        raise CampaignError("the exact session certificate is absent")
    password = _temporary_console_password(
        "Choose the temporary liveadmin password for this campaign (input is not retained): "
    )
    try:
        _local_topology(session, contact=True)
        with _ssh_master(session) as (known, control):
            _verify_identity(session, known, control)
            _ssh(session, known, control, "set -eu; /var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh start; "
                 "/var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh health", timeout=120.0)
            cookie, csrf = _establish_console(session, known, control, certificate, password)
        signaling_result = _signaling_only_call(session, certificate, cookie, csrf)
        result = _local_hangup_media_call(session, certificate, cookie, csrf)
        reconnect_result = _withheld_final_reconnect(session, certificate, cookie, csrf)
        remote_hangup_result = _remote_hangup_call(session, certificate, cookie, csrf)
        _local_topology(session, contact=True)
        with _ssh_master(session) as (known, control):
            _verify_identity(session, known, control)
            remote = f"/run/.aula-ti8168-sip-endpoint-live-{session.session_id}-b"
            _tar_transfer(
                session, known, control, [PAYLOAD, LIVE_TARGET], remote,
                expected_payload_digest=manifest_digest,
            )
            _ssh(
                session, known, control,
                f"set -eu; base={remote}; trap 'rm -rf \"$base\"' 0 1 2 15; "
                "/var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh stop; "
                "sh $base/deployment/targets/ti8168/install.sh "
                f"--payload $base/.work/dist/aula-ti8168-sip-endpoint-live/runtime --version {session.version_b} "
                f"--manifest-sha256 {manifest_digest}; rm -rf {remote}; trap - 0 1 2 15; "
                f"/var/lib/cbox/aula-ti8168-sip-endpoint/live-rollback.sh --to {session.version_a}; "
                "/var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh health",
                capture=False, timeout=600.0,
            )
            cookie, csrf = _login_console(session, certificate, password)
            rollback_result = _short_call(session, certificate, cookie, csrf)
    finally:
        for index in range(len(password)):
            password[index] = 0
    evidence.private_observations["signaling_only_peer"] = signaling_result
    evidence.private_observations["peer"] = result
    evidence.private_observations["withheld_final_peer"] = reconnect_result
    evidence.private_observations["remote_hangup_peer"] = remote_hangup_result
    evidence.private_observations["post_rollback_peer"] = rollback_result
    evidence.checks.update({"inactive_signaling_only": True, "h264_g711_rtcp": True,
                            "receive_shim_no_rendering_claim": True,
                            "dtmf_and_local_hangup": 11 in result.get("dtmf_tones", []),
                            "remote_hangup": True,
                            "withheld_final_reconnect": True,
                            "release_b_inert_install": True, "rollback_to_a": True,
                            "post_rollback_short_call": True})


def _reboot(session: LiveSession, evidence: PhaseEvidence) -> None:
    certificate = _private_root(session) / "tls/server.crt"
    if not certificate.is_file() or certificate.is_symlink():
        raise CampaignError("the exact session certificate is absent")
    _local_topology(session, contact=True)
    old_fingerprints: list[str]
    with tempfile.TemporaryDirectory(prefix="reboot-", dir=_private_root(session)) as temporary:
        directory = Path(temporary)
        _old_known, old_fingerprints = _scan_host_key(session, directory)
        with _ssh_master(session) as (known, control):
            _verify_identity(session, known, control)
            preboot_boot_id = _authenticated_boot_id(session, known, control)
            _ssh(session, known, control,
                 f"set -eu; /var/lib/cbox/aula-ti8168-sip-endpoint/live-rollback.sh --to {session.version_b}; "
                 "/var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh enable-autostart", capture=False, timeout=300.0)
            _ssh(session, known, control, _reboot_preflight_command(session.version_b), capture=False)
        time.sleep(15.0)
        deadline = time.monotonic() + 300.0
        new_known: Path | None = None
        new_fingerprints: list[str] = []
        while time.monotonic() < deadline:
            try:
                new_known, new_fingerprints = _scan_host_key(session, directory)
                break
            except (CampaignError, FileExistsError):
                with contextlib.suppress(FileNotFoundError):
                    (directory / "known-hosts").unlink()
                time.sleep(5.0)
        if new_known is None:
            raise CampaignError("target did not return after graceful reboot")
        _transport._confirm_reconnect(old_fingerprints, new_fingerprints)
        _local_topology(session, contact=True)
        with _ssh_master(session) as (known, control):
            _verify_identity(session, known, control)
            postboot_boot_id = _authenticated_boot_id(session, known, control)
            if postboot_boot_id == preboot_boot_id:
                raise CampaignError("authenticated boot identifier did not change after reboot")
            _ssh(
                session, known, control, _reboot_postboot_command(session.version_b),
                capture=False, timeout=120.0,
            )
    _local_topology(session, contact=True)
    password = _temporary_console_password(
        "Temporary liveadmin password for the post-reboot call (input is not retained): "
    )
    try:
        with _ssh_master(session) as (known, control):
            _verify_identity(session, known, control)
            cookie, csrf = _login_console(session, certificate, password)
            peer_result = _short_call(session, certificate, cookie, csrf)
    finally:
        for index in range(len(password)):
            password[index] = 0
    evidence.private_observations["post_reboot_peer"] = peer_result
    evidence.checks.update({"graceful_reboot": True, "fingerprint_reconfirmed": True,
                            "boot_identity_changed": True, "selector_c": True,
                            "autostart_marker": True, "autostart_sentinel": True,
                            "autostart_health": True, "post_reboot_short_call": True})


def _remove(session: LiveSession, evidence: PhaseEvidence) -> None:
    _actions.remove(
        session, evidence, local_topology=_local_topology, ssh_master=_ssh_master,
        verify_identity=_verify_identity, remote_baseline=_remote_baseline, ssh=_ssh,
        private_root=_private_root,
    )


def _run_action(action: str, session: LiveSession, evidence: PhaseEvidence) -> None:
    actions = {
        "preflight": lambda: _preflight(session, evidence),
        "install": lambda: _install(session, evidence),
        "smoke": lambda: _smoke(session, evidence),
        "soak-15": lambda: _soak(session, evidence, 15),
        "soak-30": lambda: _soak(session, evidence, 30),
        "soak-60": lambda: _soak(session, evidence, 60),
        "reboot": lambda: _reboot(session, evidence),
        "remove": lambda: _remove(session, evidence),
    }
    actions[action]()


def main() -> int:
    with contextlib.suppress(ValueError, OSError):
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=sorted(ALLOWED_ACTIONS))
    args = parser.parse_args()
    session: LiveSession | None = None
    evidence: PhaseEvidence | None = None
    locks = contextlib.ExitStack()
    try:
        _require_private_identity_provider()
        session = load_session(os.environ.get("LIVE_SESSION"))
        session.require_action(args.action)
        require_confirmations(args.action, os.environ)
        locks.enter_context(_session_lock(session))
        _reconcile_journal(session)
        _record_journal(session, args.action, "started")
        evidence = PhaseEvidence(args.action, time.time_ns())
        _run_action(args.action, session, evidence)
        private, summary = _publish(evidence, session)
        _record_journal(session, args.action, "completed")
        print(f"live campaign phase passed; private={private} sanitized={summary}")
        return 0
    except BaseException as error:
        interrupted = not isinstance(error, Exception)
        if session is not None and evidence is not None:
            evidence.checks["phase_completed"] = False
            if args.action in MUTATING_ACTIONS:
                evidence.checks["manual_recovery_required"] = True
            if isinstance(error, PeerCompletionError):
                evidence.private_observations["peer_failure"] = error.result
            evidence.private_observations["failure_type"] = type(error).__name__
            with contextlib.suppress(CampaignError, OSError, ValueError):
                _publish(evidence, session)
            with contextlib.suppress(CampaignError, OSError, ValueError):
                state = "interrupted" if interrupted else "failed"
                _record_journal(session, args.action, state, failure_type=type(error).__name__)
        if isinstance(error, (CampaignError, SessionError)):
            message = str(error)
        else:
            message = f"{type(error).__name__} (details redacted)"
        print(f"live campaign stopped: {message}", file=sys.stderr)
        if interrupted:
            raise
        return 2
    finally:
        locks.close()


if __name__ == "__main__":
    raise SystemExit(main())
