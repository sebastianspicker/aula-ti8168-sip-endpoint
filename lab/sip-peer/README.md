# Private SIP/RTP peer

`private_sip_peer.py` is the bounded, protocol-neutral UDP SIP/RTP peer that
stands in for the one authorised private-lab counterpart of the [TI8168 media
board](../../docs/reference/ti8168-board.md). It accepts
exactly one correlated SIP dialog, validates SDP/RTP/RTCP against the H264
offer, and records only counters, timings, tuples, SSRCs, and SHA-256 packet
digests as evidence; SIP bodies and RTP payloads are transient parsing
inputs, never retained.

`qemu_profile.py` defines `PrivateSipPeer`, the QEMU-profile subclass: it
fixes the guest source address and NAT port range that QEMU's user-mode
networking exposes to the target guest (`QEMU_PEER_ADDRESS`,
`QEMU_AULA_ADDRESS`, `QEMU_NAT_SOURCE_ADDRESS`,
`QEMU_NAT_PORT_MIN`/`QEMU_NAT_PORT_MAX`), and allows the distinct RTCP tuple
QEMU's NAT produces. It is used unchanged for the physical private-lab
campaign as well, since both environments share the same guest-side NAT
contract. It is a separate file from `private_sip_peer.py` only to stay under
the maintained-source file-size gate; both live in this one directory.

`run_private_sip_peer.py` is the CLI entry point:

```sh
python3 lab/sip-peer/run_private_sip_peer.py --bind-address IPV4 \
  [--advertised-address IPV4] [--authorized-source-address IPV4] \
  [--sip-port PORT] [--timeout SECONDS] [--require-bidirectional] \
  [--withhold-first-invite-final] [--withheld-final-timeout SECONDS] \
  [--peer-initiated-hangup] [--signaling-only]
```

It runs the peer for one bounded window, waits for the dialog (and, unless
`--signaling-only`, bidirectional media) to complete, prints the evidence as
JSON, and exits non-zero if the run did not complete.

## Who uses it

- `tooling/live/campaign_calls.py` runs `run_private_sip_peer.py` as a
  subprocess against the physical target private lab.
- `lab/sip-peer/tests/` exercises the shared peer protocol and the QEMU
  profile and CLI together.

Callers add exactly one `sys.path` entry for this directory and import
`private_sip_peer` directly; there is no package chain to follow.
