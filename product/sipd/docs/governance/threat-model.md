# Threat model

Status: source-derived control model. Host tests validate selected controls;
ARM, live target, private-peer, and Zoom evidence remain separate gates.

## Assets and trust boundaries

| Boundary | Assets at risk | Primary threats | Required controls |
| --- | --- | --- | --- |
| SIP and SDP ingress | call identity, process availability, credentials | oversized or malformed messages, parser faults, replay, authentication abuse | fixed input limits, checked lengths, transaction limits, library selection review, fuzzing, redacted diagnostics |
| RTP and RTCP ingress | media confidentiality/integrity, process availability, replay state, socket state | malformed packets, source spoofing, off-tuple replay poisoning, SSRC churn, packet floods | negotiated payload/size checks, pre-unprotect tuple authorization, SRTP/SRTCP replay and authentication checks, source/SSRC locking, per-SSRC limits, bounded accounting, no per-packet logs |
| SDP media-security changes | media confidentiality and integrity | omitted protection policy, full or mixed re-INVITE downgrade, stale staged keys/resources | compiled `prefer_srtp` default, explicit template/runtime override, strict SDES suite/key grammar, sticky per-stream protection, fresh distinct keys, ACK-gated commit, atomic rollback and zeroization |
| Local RTSP ingest | media service availability, H.264 parser | malicious or unstable local stream, parser faults, service contention | exact canonical IPv4 target allowlist, direct byte-address socket connection without DNS, bounded RTSP/H.264 parsing, separate loopback/non-loopback opt-ins, health limits, stop on media-service regression |
| PCM capture child | host process control, audio privacy | argument injection, child escape, runaway restart, unbounded pipe data | explicit `execve` argv, no shell, fixed environment, bounded IPC, process ownership checks, restart cap |
| Local control socket | call state and daemon control | unauthorized local clients, command injection, resource exhaustion | disabled by default, Unix socket only, owner/mode checks, bounded messages/clients, no network listener |
| Configuration and secrets | SIP credentials, meeting information | disclosure, permissive files, unsafe paths, malformed values | strict schema, reject unknown keys, root-owned restrictive files, secret file indirection, redaction validator |
| Deployment overlay | target integrity and recovery | path overwrite, symlink traversal, partial install, privilege abuse | immutable manifest, ownership/hash checks, staging and atomic publish, no startup link by default, offline rollback |
| Logs and captures | identifiers, addresses, media, authentication data | accidental disclosure or flash exhaustion | sanitization gate, no raw captures in version control, bounded summaries, syslog preference on target |

## Security invariants

1. No daemon-owned TCP or UDP control listener is permitted. Local control, if
   introduced, is an explicitly enabled Unix-domain socket.
2. All network and media parsers enforce a finite size, count, and recursion
   boundary before allocation or field access.
3. Configuration-derived text never reaches a shell. Child capture uses a
   fixed executable path and explicit `execve` argument vector.
4. The receive shim validates and drains inbound media; decoding and rendering
   are confined to the bounded media-session and renderer interfaces.
5. TLS, when required, validates certificate chains and names. A production
   verification-bypass setting is forbidden.
6. Public-network and Zoom actions remain opt-in and require separate
   authorization; host fixtures must have no public route by default.
7. Deployment must preserve unowned files and retain modified files on removal.
8. Once a negotiated stream uses SRTP, no re-INVITE may downgrade that stream.
   Plain RTP remains an explicit `plain_compat` interoperability mode because
   a CRC or isolated lab peer may require it; it is never an implicit fallback
   after protected negotiation and is reported as `rtp` in authenticated
   status.

## Required validation work

- Fuzz the SIP-adapter boundary, SDP, RTP, RTCP, RTSP, Annex-B H.264, and
  configuration parsers with malformed and limit-boundary corpus inputs.
- Exercise authorization failures, source changes, child failure/restart caps,
  Unix-socket permission failures, secret-file permission failures, and
  symlink/path traversal rejection.
- Run sanitization positive and negative fixtures before publishing diagnostics
  or packet summaries.
- Reassess this model before direct GStreamer/CBox/SysLink/DSP integration,
  startup activation, TLS introduction, or any live public-network test.
