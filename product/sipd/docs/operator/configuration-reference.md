# Configuration and redaction reference

Status: source-derived reference for the current `0.1.0` implementation. It is
not an authorization to place a SIP call or expose the unit to a network.

The parser accepts only the five sections and keys listed below. It rejects
unknown and duplicate keys, malformed syntax, control characters, `..` path
segments, values of 512 bytes or more, configuration files larger than 64 KiB,
and lines longer than 1,023 bytes. Use
`config/aula-sipd.example.conf` as the non-operational starting point.

The physical service launcher supplies DHCP nameservers separately through
`AULA_SIPD_DNS_SERVERS`: one to three comma-separated IPv4 literals, without
spaces or ports. This initializes the glibc system resolver where vendor
directory permissions prevent reading `/etc/resolv.conf` after privilege drop.
Unset leaves the ordinary system resolver unchanged; malformed explicit values
fail resolution. Other platforms reject an explicit override. Restarting the
service refreshes the launcher's DHCP nameserver list. This variable does not
authorize a SIP destination or replace certificate verification.

## Safe baseline

The example configuration is deliberately inert: `backend = fixture`, no URI,
no credentials, loopback-only media addresses, `enable_public_network = false`,
TLS disabled, and local control disabled. The current foreground loop
deliberately performs no SIP, RTP, RTSP, or public-network action. A non-empty
`sip.uri` is rejected unless `enable_public_network = true`; conversely,
public-network enablement requires a non-empty `sip:` URI and a profile-valid
configuration.

The parser accepts a TLS-shaped direct or proxy configuration only when its
profile, `transport = tls`, `enable_tls = true`, and a CA file agree. The exact
verified pjproject host build supports TLS 1.2/1.3, validates the peer against
that CA file and the original authorized URI hostname, sends SNI, and never
falls back to cleartext or independent PJSIP DNS. This is host-fixture proof,
not trusted-device-certificate or Zoom interoperability evidence.

## Zoom and Zoom-X destinations

An invitation's SIP/H.323 address identifies a room connector; its web meeting
hostname does not. The regional route changes only connector selection, while
the authenticated service supplies the negotiated media addresses. Zoom-X uses
shared Zoom domain names and changing server resources, as described in section
4.3 of the [Telekom Zoom-X whitepaper](https://www.uni-bremen.de/fileadmin/user_upload/dezernate/dezernat8/Medienstelle/Dokumente/Zoom_X_Whitepaper_Sicherheit_und_Datenschutz_V2.1.pdf).
Domain suffixes alone do not establish processing location.

Consult the [Zoom network firewall reference](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0060548)
and the separate [room connector firewall requirements](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0065748)
for network provisioning. The daemon does not embed these changing IP lists.
Its Direct CRC media policy accepts service-reported public IPv4 targets within
the documented network-scope and encryption checks.

## Runtime

| Key | Default | Accepted values and constraints |
| --- | --- | --- |
| `runtime.foreground` | `true` | Must be `true`; `false` is rejected. |
| `runtime.pid_file` | `/run/aula-sipd.pid` | Required absolute output path. No `..`, controls, or symlink target. Its parent must be a suitable existing directory. |
| `runtime.log_sink` | `stderr` | `stderr` or `syslog`. |
| `runtime.log_level` | `info` | `debug`, `info`, `notice`, `warning`, or `error`. |
| `runtime.log_summary_interval_seconds` | `60` | Positive unsigned decimal integer. |

## SIP

| Key | Default | Accepted values and constraints |
| --- | --- | --- |
| `sip.profile` | `private_lab` | `zoom_direct`, `zoom_proxy`, or `private_lab`. It selects the typed dial/registration policy; it never permits an HTTP-supplied SIP URI. |
| `sip.uri` | empty | Empty while public networking is disabled. When enabled, starts with `sip:` and uses the parser's restricted URI character set. Treat its whole value as secret operational data. |
| `sip.crc_address` | empty | Optional canonical public IPv4 address for an operator-approved regional connector. Requires `zoom_direct`, public networking, TLS and required SRTP. Routes initial CRC calls to TLS port 5061 while retaining `zoomcrc.com` for SIP and certificate verification. Empty uses normal DNS. Restart after changing it; remove this key before rollback to an older binary. |
| `sip.registrar_uri` | empty | Empty for `zoom_direct` and `private_lab`. `zoom_proxy` requires a syntactically valid `sip:` registrar URI. The adapter resolves and authorizes it once, pins the numeric transport, retains the original hostname for TLS identity, and never accepts it from HTTP. |
| `sip.transport` | `udp` | `udp`, `tcp`, or `tls`. TLS must agree exactly with `sip.enable_tls`; the current exact pinned host build proves TLS 1.2/1.3 only against deterministic loopback fixtures. |
| `sip.auth_username` | empty | Optional text. Treat as a credential identifier and do not publish it. |
| `sip.auth_secret_file` | empty | Optional absolute regular file. It must be owned by the effective user, have one link, have a non-writable parent owned by root or that user, be non-empty and at most 64 KiB, and have no group or other permissions. In normal root operation, use `0600`. |
| `sip.tls_ca_file` | empty | Optional absolute regular CA file. It is required for a TLS-shaped public `zoom_direct` or `zoom_proxy` configuration and is used as the server trust anchor. Supplying it does not authorize a live call. |
| `sip.enable_tls` | `false` | Must exactly match `sip.transport = tls`. A public `zoom_direct` or `zoom_proxy` profile requires it. The adapter never falls back to TCP or UDP after TLS failure. |
| `sip.enable_public_network` | `false` | `true` permits only a profile-valid syntactic configuration. It does not grant operator, account, meeting, firewall, private-lab, or Zoom authorization. A private-lab profile may use UDP or TCP; direct/proxy profiles require TLS-shaped configuration. |
| `sip.transaction_timeout_ms` | `32000` | `1` through `300000`. |
| `sip.max_retransmissions` | `6` | `0` through `32`. |
| `sip.max_reconnect_attempts` | `3` | `0` through `32`. |

## Media

| Key | Default | Accepted values and constraints |
| --- | --- | --- |
| `media.backend` | `fixture` | `fixture`, `rtsp_native`, or the compatibility spelling `rtsp_gst_process`. The latter two use the native RTSP seam, not a GStreamer process; selecting either is not proof of an approved or discovered target media contract. |
| `media.video_source` | empty | Aggregate RTSP URI for `rtsp_native`. Do not publish a live endpoint. Native setup selects the H.264 and MPEG4-GENERIC AAC tracks only from its DESCRIBE SDP; it does not assume track names or payload types. |
| `media.audio_source` | empty | Retained compatibility source detail. Native RTSP does not use it to choose an audio track; SDP selection is authoritative. Do not publish a device or local capture command. |
| `media.authorized_rtsp_ipv4` | `127.0.0.1` | Canonical numeric IPv4 only. A URI must use this exact target. Loopback is accepted; non-loopback targets must be RFC 1918 private space or IPv4 link-local. Unspecified, multicast, broadcast, reserved, and global addresses are rejected. The value is operational data and is not included in formatted configuration or logs. |
| `media.child_uid` | `65534` | Required non-zero, non-`4294967295` numeric value for the native-RTSP compatibility configuration. The current native backend does not execute a GStreamer, ALSA, or media child. |
| `media.child_gid` | `65534` | Required non-zero, non-`4294967295` numeric primary GID for the native-RTSP compatibility configuration. It does not establish device access or a child-process proof. |
| `media.bind_address` | `127.0.0.1` | Canonical IPv4 literal reserved for local RTP/RTCP binding. With public networking disabled, it must be in `127.0.0.0/8`; with that gate enabled, it must be loopback or RFC 1918 private space. |
| `media.advertised_address` | `127.0.0.1` | Canonical IPv4 literal reserved for local media metadata. It must exactly equal `media.bind_address`; this implementation performs no NAT mapping or address discovery. A reviewed future NAT policy is required before the values may differ. |
| `media.preferred_audio_codec` | `pcmu` | `pcmu` or `pcma`. |
| `media.security` | `prefer_srtp` | `prefer_srtp`, `required`, or `plain_compat`. `prefer_srtp` and `required` create protected initial offers; `required` rejects any plain media in a re-INVITE. Once a stream is protected, `prefer_srtp` rejects any full or mixed re-INVITE downgrade, while permitting a plain-to-protected upgrade. `plain_compat` advertises and accepts only RTP/AVP and must be selected intentionally for a CRC or isolated lab peer that requires plaintext. An SRTP-disabled binary fails protected operation as unsupported and never downgrades silently. |
| `media.rtsp_use_udp` | `false` | `false` keeps TCP RTP interleaving. `true` asks native RTSP to allocate bounded UDP RTP/RTCP ports during SETUP. It does not relax literal-IP authorization or either environment opt-in. |
| `media.rtsp_jitter_max_ms` | `120` | Unsigned decimal `1` through `2000`; copied to the native RTSP backend's bounded jitter policy. |
| `media.rtsp_reconnect_limit` | `3` | Unsigned decimal `0` through `32`; copied to the native RTSP backend reconnect policy. |
| `media.local_rtp_port_min` | `40000` | Even unsigned 16-bit port, at most `65532`. |
| `media.local_rtp_port_max` | `40100` | Even unsigned 16-bit port, at least two greater than the minimum. |
| `media.rtp_mtu` | `1200` | `256` through `limits.rtp_packet_bytes`. |
| `media.aec_enabled` | `false` | Enables the compiled SpeexDSP AEC seam only; unavailable native support remains inactive. |
| `media.aec_reference_delay_frames` | `0` | Fixed far-end delay in 20 ms frames, range `0` through `8`. |
| `media.aec_delay_calibrated` | `false` | Set only after a measured live-path delay. Host or QEMU testing must leave this `false`. |
| `media.video_queue_frames` | `8` | `1` through `128`. Compatibility name: the value is the maximum number of queued RTP packets, not video frames or H.264 access units. A complete H.264 access unit is rejected atomically when all of its packets and payload bytes do not fit. |
| `media.audio_queue_frames` | `10` | `1` through `128`. Compatibility name: the value counts queued RTP packets. |

RTP/RTCP ports are selected only through a retained media-session reservation.
The daemon keeps the selected sockets bound until that session is released and
uses those exact assignments in SDP. There is no standalone RTP port allocator
or probe-and-close workflow.

Size `video_queue_frames` against the largest observed encoded access unit
after packetization at the configured RTP MTU. The queue admits each complete
access unit atomically; a bound smaller than a keyframe's packet count causes
that keyframe to be dropped. Source-packet loss and audio queue behavior require
their own diagnostics.

`rtsp_native` starts only when the binary was explicitly linked with the exact
FAAD2 2.11.2 and SpeexDSP 1.2.1 inputs. Its AAC-LC decoder and resampler retain
state for the RTSP session, reject SBR/PS and format changes, bound every
access unit and PCM frame, and release state on stop. A dependency-disabled
binary returns `UNSUPPORTED`; it never substitutes ALSA, GStreamer, or a host
codec.

The target Linux 2.6.37 baseline predates `PR_SET_NO_NEW_PRIVS`, so no native
RTSP setting claims that facility. The portable child-execution boundary has
an exact non-root identity drop, cleared supplementary groups, verified
executable, allowlisted descriptors, and hard resource caps, but the current
native RTSP backend does not execute a media child.

For `rtsp_native` (and the `rtsp_gst_process` compatibility spelling), the URI
scheme must be lowercase `rtsp`, its host must be the exact
`media.authorized_rtsp_ipv4` literal, and its path must use the restricted
safe-path grammar. DNS, credentials, IPv6, alternate numeric forms, queries,
and fragments are rejected. The daemon copies the parsed IPv4 bytes directly
into its TCP socket address and performs no resolver call. A loopback target
connects only when `AULA_SIPD_ENABLE_LOCAL_RTSP=1`; a non-loopback target
instead requires `AULA_SIPD_ENABLE_AUTHORIZED_RTSP=1`. When `AULA_LAB_PEER`
is present, it must be the same canonical IPv4 literal before the socket is
created. These gates do not authorize a live test.

## Local control

| Key | Default | Accepted values and constraints |
| --- | --- | --- |
| `control.enable_local_control` | `false` | Enables a Unix-domain socket only. It never creates a network listener. |
| `control.unix_socket_path` | `/run/aula-sipd/control.sock` | Required absolute output path when control is enabled; no `..`, controls, or symlink target. |
| `control.unix_socket_mode` | `0660` | `0600` or `0660`; deployment uses `0660` with the dedicated control group. |
| `control.gateway_uid` | `65534` | Unsigned numeric UID authorized to connect as the gateway. It must not be `4294967295`; Unix peer credentials must match it. |

The local LSZ1 protocol has typed status, originate, hangup, DTMF,
credential-replacement, diagnostics, and subscribe opcodes. Originate accepts
only the six fixed dial fields and an approved profile; it never accepts a raw
URI. DTMF accepts exactly one RFC 4733 symbol. Credential replacement requires
the configured username and an already configured secret-file path, then wipes
the decoded password after use. Status includes `rx_rendering=false`; that
field means inbound media is not rendered. Once media is prepared, each video
and audio status object reports `security` as exactly `rtp` or `srtp` from the
negotiated session, not from configured preference. The protocol is local-only
and is not a remote administration interface.

## AEC status

The optional AEC seam is compiled only with the explicitly supplied FAAD2
2.11.2 and SpeexDSP 1.2.1 inputs. It accepts fixed 20 ms, 8 kHz mono S16LE
frames: decoded remote G.711 is accumulated into the bounded far-end reference
and local `/movie` AAC PCM is accumulated into complete frames before G.711
encoding; each side retains at most one sub-frame tail. An audio playout loss
resets the reference timeline. Status exposes only
aggregate `aec` counters and a delay state. `available` means the exact native
build can create the seam; `uncalibrated` means a configured fixed delay has
not been calibrated; only an explicit calibrated configuration may report
`calibrated`. Host or QEMU output must never be read as delay calibration or
vendor sink proof. Missing native support and missing reference frames bypass
audio unchanged and report inactive or underflow counters.

## Limits

Limits are unsigned decimal values. They must be non-zero except where a
different range is listed, and may only reduce the compiled ceiling.

| Key | Default and maximum |
| --- | --- |
| `limits.sip_message_bytes` | `16384` maximum `16384` |
| `limits.sdp_bytes` | `8192` maximum `8192` |
| `limits.rtp_packet_bytes` | `1500`, range `256` to `1500` |
| `limits.video_access_unit_bytes` | `2097152` maximum `2097152` |
| `limits.audio_frame_bytes` | `4096` maximum `4096` |
| `limits.control_message_bytes` | `2048` maximum `2048` |
| `limits.log_events_per_interval` | `120`, range `1` to `10000` |

## Secrets, diagnostics, and retained evidence

Do not store meeting IDs, passcodes, host keys, full SIP URIs, usernames,
digest material, authorization headers, cookies, or secret-file content in
versioned configuration, ordinary logs, reports, or screenshots. The
configuration formatter is designed to emit only operational booleans and
`rx_rendering=false`; it must not be relied on as a reason to retain raw input.

Use the sanitization policy in
[`../governance/sanitization-policy.md`](../governance/sanitization-policy.md)
before retaining any operational evidence. That policy permits role tokens,
field-presence summaries, and aggregate counters, not raw SIP, SDP, RTP, RTCP,
audio, video, addresses, or identifiers.

## Local syntax check

When a built binary and a root-owned candidate configuration are available,
the implemented syntax-check entry point is:

```sh
aula-sipd --check-config /path/to/aula-sipd.conf
```

This reference does not claim that this command has passed on ARM hardware,
against a live target, or against an authorized Zoom peer. The exact host
pjproject build did pass its deterministic 13/13 suite, including real local
UDP and authenticated TLS private-lab LSZ1 calls and authenticated
`zoom_proxy` registration; that is not ARM,
trusted-device-certificate deployment, provider-issued proxy credentials, live
SRTP, or Zoom acceptance. The exact host lane also passes strict protected SDP,
re-INVITE, RTP, and SRTCP oracles with locked libsrtp 2.8.
