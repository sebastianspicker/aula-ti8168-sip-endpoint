"""Preflight, install, and removal campaign actions."""

from __future__ import annotations

import contextlib
import json
import sys
from pathlib import Path
from typing import Any, Callable, ContextManager

from campaign_evidence import PhaseEvidence
from campaign_types import CampaignError, LIVE_TARGET, PAYLOAD, ROOT
from session import LiveSession


def preflight(
    session: LiveSession, evidence: PhaseEvidence, *, run: Callable[..., Any],
    local_topology: Callable[..., None], ssh_master: Callable[..., ContextManager[Any]],
    verify_identity: Callable[..., None], remote_baseline: Callable[..., dict[str, str]],
    payload_kib: Callable[[], int], ssh: Callable[..., bytes],
) -> None:
    run([sys.executable, str(ROOT / "tooling/live/gates.py"), "--check"], timeout=120.0)
    local_topology(session, contact=True)
    with ssh_master(session) as (known, control):
        verify_identity(session, known, control)
        baseline = remote_baseline(session, known, control)
        required_free_kib = 2 * payload_kib() + 4096
        if (
            baseline["root_ssh"] != "ready"
            or int(baseline["mem_available_kib"]) < session.limits["minimum_available_ram_mib"] * 1024
            or int(baseline["free_kib"]) < required_free_kib
        ):
            raise CampaignError("target root-SSH, memory, or free-space preflight failed")
        ssh(session, known, control, "set -eu; [ ! -e /var/lib/cbox/aula-ti8168-sip-endpoint ]; "
            "[ ! -L /var/lib/cbox/aula-ti8168-sip-endpoint ]")
        evidence.private_observations["baseline"] = baseline
        evidence.private_observations["required_free_kib"] = required_free_kib
        evidence.checks.update({"route_mac_identity": True, "target_baseline": True, "no_conflict": True})


def install(
    session: LiveSession, evidence: PhaseEvidence, *, verified_payload_digest: Callable[[], str],
    certificate: Callable[[LiveSession], tuple[Path, Path]], local_topology: Callable[..., None],
    ssh_master: Callable[..., ContextManager[Any]], verify_identity: Callable[..., None],
    tar_transfer: Callable[..., None], ssh: Callable[..., bytes],
) -> None:
    digest = verified_payload_digest()
    certificate_path, key = certificate(session)
    local_topology(session, contact=True)
    with ssh_master(session) as (known, control):
        verify_identity(session, known, control)
        remote = f"/run/.aula-ti8168-sip-endpoint-live-{session.session_id}"
        tar_transfer(
            session, known, control, [PAYLOAD, LIVE_TARGET, certificate_path, key], remote,
            expected_payload_digest=digest,
        )
        command = (
            f"set -eu; base={remote}; "
            "trap 'rm -rf \"$base\"' 0 1 2 15; "
            f"sh $base/deployment/targets/ti8168/install.sh --payload $base/.work/dist/aula-ti8168-sip-endpoint-live/runtime "
            f"--version {session.version_a} --manifest-sha256 {digest}; "
            f"sh $base/deployment/targets/ti8168/configure-private-lab.sh --sip-peer-ip {session.host_ipv4} "
            f"--sip-peer-port {session.peer_sip_port} --device-ip {session.target_ipv4} "
            f"--tls-cert $base/.work/live/{session.session_id}/private/tls/server.crt "
            f"--tls-key $base/.work/live/{session.session_id}/private/tls/server.key; "
            "sh /var/lib/cbox/aula-ti8168-sip-endpoint/live-verify-payload.sh "
            f"--payload /var/lib/cbox/aula-ti8168-sip-endpoint/releases/{session.version_a} --manifest-sha256 {digest}; "
            f"rm -rf {remote}; trap - 0 1 2 15"
        )
        ssh(session, known, control, command, capture=False, timeout=600.0)
        evidence.checks.update({"release_a_inert_install": True, "tls_config_installed": True, "payload_verified": True})


def remove(
    session: LiveSession, evidence: PhaseEvidence, *, local_topology: Callable[..., None],
    ssh_master: Callable[..., ContextManager[Any]], verify_identity: Callable[..., None],
    remote_baseline: Callable[..., dict[str, str]], ssh: Callable[..., bytes],
    private_root: Callable[[LiveSession], Path],
) -> None:
    candidates = sorted(private_root(session).glob("*-preflight.json"))
    if not candidates:
        raise CampaignError("pre-write baseline evidence is absent")
    try:
        baseline_document = json.loads(candidates[-1].read_bytes())
        baseline = baseline_document["private_observations"]["baseline"]
    except (OSError, ValueError, KeyError, TypeError) as error:
        raise CampaignError("pre-write baseline evidence is malformed") from error
    immutable_keys = (
        "mounts", "crontab", "passwd", "group", "shadow", "passwd_meta", "group_meta",
        "shadow_meta", "persistent_paths", "root_ssh",
    )
    if not isinstance(baseline, dict) or any(
        type(baseline.get(key)) is not str for key in immutable_keys
    ):
        raise CampaignError("pre-write baseline evidence is incomplete")
    local_topology(session, contact=True)
    with ssh_master(session) as (known, control):
        verify_identity(session, known, control)
        before = remote_baseline(session, known, control)
        ssh(session, known, control, "set -eu; /var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh stop; "
            "/var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh disable-autostart; "
            "sh /var/lib/cbox/aula-ti8168-sip-endpoint/live-remove.sh --purge-owned-state", capture=False, timeout=300.0)
        after = remote_baseline(session, known, control)
    restored = all(baseline.get(key) == after[key] for key in immutable_keys) and after["profile"] == "absent"
    if not restored:
        raise CampaignError("post-removal baseline did not restore exactly")
    tls = private_root(session) / "tls"
    for leaf in (tls / "server.key", tls / "server.crt"):
        if leaf.exists() and not leaf.is_symlink():
            leaf.unlink()
    with contextlib.suppress(OSError):
        tls.rmdir()
    evidence.private_observations.update({"before": before, "after": after})
    evidence.checks.update({"owned_state_purged": True, "baseline_restored": restored,
                            "power_cut_durability_untested": True})
