# Aula SIP daemon

`aula-sipd` is the native C SIP daemon of Aula, built for the
[TI8168 media
board](../../docs/reference/ti8168-board.md). It owns SIP and media session
state, exposes a local typed control protocol to the console gateway, and keeps
platform, signaling, and media integrations behind explicit adapters.

It is a prototype, not a released Zoom endpoint. Host and QEMU fixtures do not
establish provider interoperability, trusted target TLS/SRTP, sustained device
media, calibrated AEC, or recovery under power loss.

## Architecture

The CMake project builds:

- `aula_sipd_core`, which contains configuration, endpoint, SIP/SDP,
  RTP/RTCP, media, backend, platform, and LSZ1 control modules;
- `aula-sipd`, the foreground daemon entry point.

Public headers live under `include/aula_sipd/`. The gateway-facing control
server uses bounded `SOCK_SEQPACKET` messages on a Unix socket and authenticates
the configured peer UID. It has no TCP or UDP control listener. Supported
operations are typed status, call, DTMF, media, settings, credentials,
diagnostics, event, and read-only metrics requests; the protocol does not
accept raw SIP URIs, commands, or arbitrary paths. Metrics use opcode 10 and
do not alter the legacy opcode-1 status payload. Stream counters reset with a
media session and poll-lateness counters reset with the process. The current
backend health contract is session-wide, so its drop and restart counters are
repeated for both stream entries.

The standalone RS-232 observation-to-call bridge and its public header were
removed from this maintained source. Existing LSZ1 call operations remain;
there is no RS-232 adapter in the daemon. This is an intentional public C API
removal, separate from any future protocol migration.

The opcode-10 response shape is:

```json
{
  "revision": 1,
  "reset": {"streams": "session", "poll": "process"},
  "streams": [
    {
      "kind": "audio",
      "queue_depth": 0,
      "packet_drops": 0,
      "access_unit_drops": 0,
      "backend_drops": 0,
      "backend_restarts": 0
    },
    {
      "kind": "video",
      "queue_depth": 0,
      "packet_drops": 0,
      "access_unit_drops": 0,
      "backend_drops": 0,
      "backend_restarts": 0
    }
  ],
  "poll": {"late_count": 0, "max_lateness_ms": 0}
}
```

Every counter is a nonnegative integer capped at `2147483647`. Poll lateness is
measured against the earliest internal service deadline captured when polling
starts. An earlier caller deadline only bounds that call's wait and does not
create a late sample.

Active send-only streams still authenticate and process RTCP feedback.
PLI/FIR must target the local media SSRC and pass the existing rate limit;
inbound RTP is discarded without playout. Inactive streams remain skipped.
When committed media is released, the daemon writes numeric packet, rejection
and keyframe-request totals to its log without peer identifiers or payloads.
Early authenticated send-only RTCP reports also provide bounded numeric loss
and sequence diagnostics. Trusted in-dialog SIP INFO observations report only
bounded content-type/token classifications. Authenticated RFC 5168 full-picture
INFO requests use a private, dialog-bound adapter queue. A 200 response confirms
acceptance; the endpoint dispatches recovery after SIP polling, at most once per
second. The bounded parser accepts the unqualified full-picture XML structure
without general XML, entities, attributes or stream selection. Unsupported
backend keyframe results remain visible. Unsupported refresh preserves intact
source assembly and transmission so the next periodic IDR can reach the peer.

The foreground loop keeps a 20 ms idle ceiling and services active video within
1 ms, shortening waits further for queued RTP/DTMF and actionable receive
playout. Reorder holds prevent overdue timestamps from causing a spin. RTP
transmission preserves its scheduled cadence across brief delays, with catch-up
limited to four video packets and two audio packets per poll. DTMF and the
public strict-spacing queue API retain their original pacing.
RTSP UDP drains process at most eight datagrams per channel before moving on.
Each RTP socket requests a bounded 256 KiB receive buffer for camera bursts;
RTCP sockets and system-wide buffer settings are unchanged.
Expired source-packet gaps discard partial H.264 assembly. Valid orphan
fragments are dropped, and a new fragment start can recover the assembly.
Sequence jumps beyond the reorder window wait for the configured jitter
deadline before resynchronizing. Buffered frames remain queued until the
consumer has read the preceding frame. Malformed payloads, incompatible
profiles and resource-limit failures retain their error handling.
After a valid IDR establishes synchronization, dependent frames are delivered.
Packet loss, changed parameter sets, reconnect or a keyframe request clears
synchronization until another valid IDR arrives.
The native reader refreshes its RTSP session with an empty `GET_PARAMETER`
request at most ten seconds apart, using the advertised session timeout.
Only one refresh may be pending; a missing or invalid reply enters bounded
reconnect recovery while malformed media retains its existing error handling.
Media continues to drain while a refresh response is pending.
Eligible RTP/RTCP sockets also wake control and PJSIP waits. PJSIP observes
owned duplicate descriptors through one-byte asynchronous peek operations;
callbacks only signal readiness, leaving the media session as the sole packet
consumer. Registration or arm failure retains the 20 ms bounded polling path.

Outbound H.264 access units are admitted atomically. The daemon preflights the
complete packet and byte requirement, builds private queue entries, and links
the batch only after every allocation succeeds. A rejected access unit leaves
the existing backlog and RTP sequence unchanged. The historical
`video_queue_frames` configuration name remains compatible and counts RTP
packets, not decoded frames or H.264 access units.

Control peers are authorized on acceptance and retained in nonblocking slots
with an absolute one-second deadline. The runtime uses eight slots (maximum 64).
Each poll accepts at most one connection and rotates service among ready
clients; blocked responses remain bounded and every retained descriptor closes
on expiry or destruction.

Runtime dependency direction is:

```text
console gateway -> LSZ1 Unix socket -> endpoint
endpoint -> SIP adapter -> SIP/Zoom peer
endpoint -> media backend -> authorized RTSP source
endpoint -> renderer adapter -> fake fixture output or target renderer seam
```

Bounded diagnostics recognize an established SIP call even when registration is
disabled for direct CRC. This signaling check does not certify remote media
reception or renderer readiness.

## Build

From the repository root, a focused default host build is:

```sh
cmake -S product/sipd -B .work/build/sipd \
  -DCMAKE_BUILD_TYPE=Release
cmake --build .work/build/sipd
```

## Optional integrations

The default build uses the fixture adapter and performs no dependency download
or host-package discovery. Production-shaped integrations are enabled only with
explicit reviewed inputs:

- PJSIP for SIP transactions;
- libsrtp for SDES-SRTP/SRTCP;
- FAAD2 and SpeexDSP together for AAC-LC decoding, downmix/resampling, and AEC;
  and
- a platform renderer implementation for target audio/display output.

SDES supports `AES_CM_128_HMAC_SHA1_80` with an inline key and an optional
positive key lifetime, written as decimal or `2^N`, up to `2^48` packets.
Transmit and receive lifetimes are enforced separately for RTP and RTCP;
RTCP is additionally capped at `2^31` packets. Reaching either limit exhausts
that direction's key for both protocols. The answer must select the
offered crypto tag. MKI, additional key/session parameters, and other crypto
suites are rejected. This does not establish AES-GCM support.

SDP answer selection can ignore unsupported audio formats with valid payload
mappings when a supported codec remains available. Transmit and receive payload
numbers are negotiated separately: an offerer sends with the answer's number
and receives with its own offered number. Codec, H.264 profile, packetization,
transport, and encryption checks still apply.

Telephone-event tones are optional. When the peer does not select a compatible
tone format, G.711 audio and H.264 video can still start; DTMF commands remain
unavailable for that call.

The native RTSP backend discovers the source H.264 profile from its first SPS
before creating an offer. This bounded preflight opens a temporary RTSP stream,
discards media, and closes it after reading the codec metadata. The offer uses
that observed profile; the call source and transmit gate check it again.
Equivalent RFC 6184 subprofiles can match at the same level. This does not
transcode video or lower the source level.

Zoom Direct CRC has one send-only interoperability exception: if both offered
media sections are send-only and Zoom answers both as send/receive, negotiation
keeps the actual answer for diagnostics and limits the target device to
sending. It does not enable receive processing or rendering. Generic SIP negotiation and
other incompatible direction combinations remain strict; media security and
codec validation are unchanged.

Direct CRC over TLS with required SRTP accepts service-reported unicast IPv4
media targets outside the signaling peer address. Targets still pass network-scope
checks; private, loopback, link-local and multicast destinations are rejected.
This broad interoperability policy accepts the reported nonzero RTP/RTCP ports
and does not require a provider-specific CIDR list, allowing hosted services such
as Zoom-X to supply their media routes. Explicit operator media authorizers keep
precedence. SIP identity checks and other profiles are unchanged.

An explicit `AULA_SIPD_RECEIVE_MONITOR` absolute executable path selects the
headless receive monitor for a non-fixture backend. The operator must verify
that target FFmpeg executable before enabling it. The daemon starts two bounded,
unprivileged children through the verified-inode process API: H.264 keyframe
decoding and incoming PCM processing. Nonblocking queues preserve partial
writes and reject overflow. Child failure makes receive readiness fail; call
release closes inputs and reaps both children, and subsequent calls create
fresh workers.

H.264 reception assembles all NALs through the access unit's RTP marker,
including STAP-A and FU-A. Bounded parameter-set prefixes may precede the first
picture timestamp. Broken fragments and video queue overflow request keyframe
recovery instead of terminating the call. Queue acceptance counters are not
proof of physical playback. This monitor does not drive HDMI or speakers; the
vendor hardware renderer factory still returns `unsupported`. Fixtures keep
their fake renderer.

The CMake option contract is in `cmake/AulaSipdOptions.cmake`. Version,
integrity, feature, and license status is recorded in
[`third_party/README.md`](third_party/README.md),
[`../dependencies/README.md`](../../dependencies/README.md), and the
[SIP dependency gate](docs/governance/sip-dependency-gate.md). Supplying a
library path does not approve distribution or live network use.

## Configuration

Start from [`config/aula-sipd.example.conf`](config/aula-sipd.example.conf)
and read the complete
[`configuration-reference.md`](docs/operator/configuration-reference.md).
The parser rejects unknown or duplicate keys, unsafe paths, malformed values,
over-limit inputs, and inconsistent network/security profiles.

The precedence is:

1. compiled defaults;
2. one strict file passed with `--config`, or a sealed descriptor passed with
   `--config-fd`; and
3. validated persisted runtime settings for the fields exposed through LSZ1.

There is no generic environment-variable configuration layer. Environment
variables that enable local or authorized RTSP are additional execution gates,
not configuration substitutes.
For a regional Zoom or Zoom-X room connector, optional `sip.crc_address`
selects an operator-provided public IPv4 address on TLS port 5061. Leave it empty
for ordinary `zoomcrc.com` DNS routing. The SIP destination and certificate
identity remain `zoomcrc.com`; the numeric route still passes peer authorization.
The setting requires the direct profile, public networking, TLS and required SRTP.
It is static configuration and requires a service restart. Remove it before
rolling back to a binary that predates this setting.

The physical launcher also supplies `AULA_SIPD_DNS_SERVERS`, a bounded list of
up to three comma-separated IPv4 DHCP nameservers, before dropping privileges.
On glibc this initializes the system resolver used by `getaddrinfo` when the
vendor resolver file is inaccessible to the service. An absent variable leaves
normal resolver behavior unchanged; a malformed explicit list fails closed.
This does not change SIP destinations or resolved-peer authorization. Startup
refreshes the list from DHCP state, without changing vendor directory permissions.

A synchronous initial call failure records its status and releases the call so
that another originate request can be attempted; it does not leave the console
stuck in the resolving state.
If preparation of an automatic retry fails, the call terminates with that
error and no further retry is scheduled. The daemon keeps serving local control,
and an operator can start a fresh call. This does not suppress SIP-adapter,
clock, or unexpected call-state errors.
Native SIP failures also emit an immediately flushed `sip-stage` line on
standard error, with a fixed operation label, numeric code, and OS error where
available. Resolver preparation, hostname lookup, transport acquisition,
dialog setup, session timers, and initial INVITE sends have distinct labels.
These lines omit destinations, SIP messages, and credentials. Numeric peers
retain their actual address length when passed into PJLIB, so socket conversion
does not trigger a second hostname lookup.
Dialog reauthorization retains its pinned address when that exact address and
port still appear anywhere in the current DNS answer set. A different answer
order therefore does not invalidate the socket; an absent peer, changed port,
or denied network scope still fails closed.

The compiled media-security default is `prefer_srtp`. The inert example selects
`plain_compat` because it describes an isolated private-lab fixture. Public Zoom
profiles require TLS-shaped configuration and protected media. Effective
authenticated status, not a default or template, is the authority for a running
process.

The LSZ1 runtime-settings `tls` projection describes daemon policy, not an
observed certificate result. It is `required` when the effective SIP transport
is configured for TLS and `not_required` for a plaintext private-lab transport.
Settings mutations may omit `tls`; the daemon ignores the legacy `verified` and
`required` mutation values so older clients and persisted state remain readable.
Persisted files retain the legacy `tls` value `required` for rollback to an
older daemon; the running daemon derives the response projection from its actual
transport configuration.

Useful non-network commands are:

```sh
aula-sipd --version
aula-sipd --self-test
aula-sipd --fixture-call
aula-sipd --check-config /path/to/aula-sipd.conf
```

The fixture call does not prove SIP, RTP, hardware, or Zoom interoperability.

## State and failure semantics

Configuration is immutable for a running call. Accepted idle runtime-settings
updates advance a revision and are written through a mode-`0600` temporary file,
file `fsync`, and atomic rename. Rename is the commit point. A parent-directory
`fsync` failure after rename returns `PERSISTENCE_UNCERTAIN`, but the new settings
and revision remain authoritative in the running daemon. An ordinary restart
reloads the renamed file; power-loss durability is not established.
Failures before rename retain the previous settings and revision. A successful
response means the renamed state is authoritative; it is not proof of
target-filesystem power-loss durability.

Secrets remain in external owner-only files. Diagnostics and status use safe
projections. Follow the [sanitization policy](docs/governance/sanitization-policy.md)
before retaining output.

## Maintained references

- [Configuration and redaction](docs/operator/configuration-reference.md)
- [Threat model](docs/governance/threat-model.md)
- [Sanitization policy](docs/governance/sanitization-policy.md)
- [Zoom compatibility boundary](docs/governance/zoom-official-compatibility.md)
- [Zoom reference refresh](docs/governance/zoom-reference-refresh.md)
- [ARM toolchain and ABI boundary](docs/arm/toolchain-sysroot-abi.md)
- [Repository architecture](../../docs/ARCHITECTURE.md)

Outgoing endpoint offers advertise H.264 PLI, FIR, and generic NACK. The selected
codec's feedback capabilities are intersected with the answer, including
independent transmit and receive payload numbers. Generic NACK enables a bounded
500 ms cache of successfully sent RTP wire packets; repairs reuse the original
protected bytes, have packet and byte budgets, and never relax SRTP replay
checks. Target or key changes clear the cache. PLI and FIR are targeted and rate
limited; repeated FIR sequence numbers do not trigger repeated refreshes. The
native RTSP backend preserves its running stream when immediate refresh is
unsupported and waits for the encoder's periodic IDR.

Authenticated TMMBR requests are parsed and retained internally as requested
limits. Live OEM bitrate control is not yet available, so the endpoint does not
advertise TMMBR support or acknowledge an unapplied rate. Changing the OEM
preset remains an operator action between calls; packet pacing does not change
the encoder's bitrate.
