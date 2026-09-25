#!/usr/bin/env python3
"""QEMU-profile SIP/RTP peer: fixed guest NAT addresses and port range.

`PrivateSipPeer` is also used unmodified for the physical private-lab
campaign in `tooling/live/campaign_calls.py`, since both environments share
the same guest-side NAT contract.
"""
from __future__ import annotations

from private_sip_peer import MEDIA_PORT_MAX, MEDIA_PORT_MIN, SipRtpPeer

QEMU_PEER_ADDRESS = "10.0.2.2"
QEMU_LS200_ADDRESS = "10.0.2.15"
QEMU_NAT_SOURCE_ADDRESS = "127.0.0.1"
QEMU_NAT_PORT_MIN, QEMU_NAT_PORT_MAX = 1024, 65535


class PrivateSipPeer(SipRtpPeer):
    """Historical QEMU surface, including its fixed guest source defaults."""
    def __init__(self, port: int, *, bind_address: str = "127.0.0.1",
                 advertised_address: str = QEMU_PEER_ADDRESS,
                 authorized_source_address: str = QEMU_NAT_SOURCE_ADDRESS,
                 withhold_first_invite_final: bool = False,
                 withheld_final_timeout: float = 5.0,
                 peer_initiated_hangup: bool = False,
                 signaling_only: bool = False) -> None:
        super().__init__(port, bind_address=bind_address, advertised_address=advertised_address,
                         authorized_source_address=authorized_source_address,
                         media_port_min=MEDIA_PORT_MIN, media_port_max=MEDIA_PORT_MAX,
                         ls200_media_port_min=QEMU_NAT_PORT_MIN,
                         ls200_media_port_max=QEMU_NAT_PORT_MAX,
                         withhold_first_invite_final=withhold_first_invite_final,
                         withheld_final_timeout=withheld_final_timeout,
                         peer_initiated_hangup=peer_initiated_hangup,
                         signaling_only=signaling_only,
                         allow_distinct_rtcp_tuple=True)
