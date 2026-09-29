#!/usr/bin/env python3
"""Run the deterministic SIP/RTP peer on one explicitly selected IPv4 address."""

from __future__ import annotations

import argparse
import dataclasses
import json
import os
import sys
import time
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

from qemu_profile import PrivateSipPeer, QEMU_AULA_ADDRESS  # noqa: E402


MAX_TIMEOUT_SECONDS = 65 * 60


def _bidirectional_media_complete(peer: PrivateSipPeer) -> bool:
    evidence = peer.evidence
    counts = (
        evidence.outbound_video_packets,
        evidence.outbound_audio_packets,
        evidence.outbound_video_rtcp_packets,
        evidence.outbound_audio_rtcp_packets,
        evidence.inbound_video_packets,
        evidence.inbound_audio_packets,
    )
    return all(count > 0 for count in counts)


def complete(peer: PrivateSipPeer, require_bidirectional: bool,
             withhold_first_invite_final: bool = False,
             peer_initiated_hangup: bool = False,
             signaling_only: bool = False) -> bool:
    evidence = peer.evidence
    dialog_complete = evidence.invite_seen and evidence.ack_seen and (
        evidence.peer_bye_200_seen == 1 if peer_initiated_hangup else evidence.bye_seen
    )
    if withhold_first_invite_final:
        dialog_complete = (
            dialog_complete and evidence.withheld_final_invites == 1
            and evidence.withheld_final_cancels + evidence.withheld_final_timeouts >= 1
            and evidence.dialogs_started >= 2 and evidence.dialogs_completed >= 1
        )
    if not require_bidirectional:
        media_counts = (
            evidence.outbound_video_packets, evidence.outbound_audio_packets,
            evidence.inbound_video_packets, evidence.inbound_audio_packets,
        )
        return dialog_complete and (not signaling_only or all(count == 0 for count in media_counts))
    return dialog_complete and _bidirectional_media_complete(peer)


def _signal_ready(descriptor: int | None) -> None:
    if descriptor is None:
        return
    if os.write(descriptor, b"R") != 1:
        raise OSError("short readiness write")


def _validate_args(args: argparse.Namespace, parser: argparse.ArgumentParser) -> None:
    if not 1024 <= args.sip_port <= 65535:
        parser.error("--sip-port must be between 1024 and 65535")
    if not 1.0 <= args.timeout <= float(MAX_TIMEOUT_SECONDS):
        parser.error(f"--timeout must be between 1 and {MAX_TIMEOUT_SECONDS} seconds")
    if args.withhold_first_invite_final and not 0.05 <= args.withheld_final_timeout <= args.timeout:
        parser.error("--withheld-final-timeout must be between 0.05 seconds and --timeout")
    if args.signaling_only and (args.require_bidirectional or args.peer_initiated_hangup):
        parser.error("--signaling-only cannot require bidirectional media or peer hangup")
    if args.ready_fd is not None and args.ready_fd < 3:
        parser.error("--ready-fd must identify a non-standard descriptor")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind-address", required=True,
                        help="exact local IPv4 literal to bind")
    parser.add_argument("--advertised-address",
                        help="exact SDP/contact IPv4 literal; defaults to bind address")
    parser.add_argument("--authorized-source-address", default=QEMU_AULA_ADDRESS,
                        help="exact authorised Aula IPv4 literal")
    parser.add_argument("--sip-port", type=int, default=15060)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--require-bidirectional", action="store_true")
    parser.add_argument("--withhold-first-invite-final", action="store_true",
                        help="send only 100/180 for the first INVITE, then require a clean reconnect")
    parser.add_argument("--withheld-final-timeout", type=float, default=5.0,
                        help="seconds before an unanswered withheld INVITE is released")
    parser.add_argument("--peer-initiated-hangup", action="store_true",
                        help="after bidirectional media proof, send one correlated peer BYE")
    parser.add_argument("--signaling-only", action="store_true",
                        help="answer both media sections inactive and reject any media proof")
    parser.add_argument("--ready-fd", type=int, metavar="FD",
                        help=argparse.SUPPRESS)
    args = parser.parse_args()
    _validate_args(args, parser)

    peer = PrivateSipPeer(
        args.sip_port,
        bind_address=args.bind_address,
        advertised_address=args.advertised_address or args.bind_address,
        authorized_source_address=args.authorized_source_address,
        withhold_first_invite_final=args.withhold_first_invite_final,
        withheld_final_timeout=args.withheld_final_timeout,
        peer_initiated_hangup=args.peer_initiated_hangup,
        signaling_only=args.signaling_only,
    )
    deadline = time.monotonic() + args.timeout
    ready_descriptor = args.ready_fd
    try:
        peer.__enter__()
        peer.assert_healthy()
        try:
            _signal_ready(ready_descriptor)
        finally:
            descriptor, ready_descriptor = ready_descriptor, None
            if descriptor is not None:
                try:
                    os.close(descriptor)
                except OSError:
                    pass
        while time.monotonic() < deadline:
            peer.assert_healthy()
            if complete(peer, args.require_bidirectional, args.withhold_first_invite_final,
                        args.peer_initiated_hangup, args.signaling_only):
                break
            time.sleep(0.05)
    finally:
        if ready_descriptor is not None:
            descriptor, ready_descriptor = ready_descriptor, None
            try:
                os.close(descriptor)
            except OSError:
                pass
        peer.close()
    result = dataclasses.asdict(peer.evidence)
    result["complete"] = complete(peer, args.require_bidirectional,
                                  args.withhold_first_invite_final, args.peer_initiated_hangup,
                                  args.signaling_only)
    result["bind_address"] = peer.bind_address
    result["advertised_address"] = peer.advertised_address
    print(json.dumps(result, sort_keys=True))
    return 0 if result["complete"] else 3


if __name__ == "__main__":
    raise SystemExit(main())
