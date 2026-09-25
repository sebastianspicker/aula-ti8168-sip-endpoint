# Official Zoom compatibility boundary

The LS200 endpoint uses Zoom Cloud Room Connector as a standards-based SIP
peer. It does not embed the Meeting SDK, Video SDK, Zoom Rooms Controller SDK,
or their proprietary runtime libraries. Those products create or control Zoom
clients on newer 64-bit or mobile platforms and do not provide a CRC SIP,
RTP, or SRTP implementation for the LS200 ARM EABI5 target.

A review of the official Zoom GitHub organization on 2026-08-27 found no
official CRC endpoint, SIP transaction stack, RTP implementation, or reusable
CRC wire fixture. The Linux Meeting and Video SDK packages support modern
x86-64 and ARM64 environments, not the LS200 32-bit ARM EABI5/Linux 2.6 target.
Their MIT-licensed samples are wrappers around separately distributed
proprietary SDK binaries and expose raw PCM/I420 callbacks rather than encoded
H.264/G.711 SIP media. They remain external references, not dependencies.

The implementation instead treats current official Zoom documentation as
protocol-test provenance:

- [SIP and H.323 Room Connector dial strings](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0065727)
  define the positional SIP user part, layout commands, host-key fields, and
  dial-code placement.
- [SIP Video Interop controls](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0058000)
  define the in-meeting DTMF menu and the `1`, then `1` layout cycle.
- [Meeting API SIP dialing](https://developers.zoom.us/docs/api/meetings/)
  returns an opaque participant identifier. Such identifiers are bounded
  alphanumeric data, not feature codes restricted to a `1` or `2` prefix.
- [CRC supported devices and protocols](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0061702)
  documents the service-level H.264, G.711, and TLS boundary.
- [Meeting SDK for Linux requirements](https://developers.zoom.us/docs/meeting-sdk/linux/get-started/download/)
  define a runtime and architecture boundary that excludes this target.
- [Video SDK CRC callout](https://developers.zoom.us/docs/video-sdk/web/sip/)
  is an optional modern-host acceptance harness: it asks Zoom to call an
  existing SIP device and is not itself a SIP endpoint implementation.

The console and daemon therefore accept an optional 1-to-64-character ASCII
alphanumeric dial or participant code and reject whitespace, delimiters,
escapes, Unicode, and complete SIP URIs. Meeting IDs, passcodes, and host keys
retain their numeric schemas. The target domain remains administrator-owned or
the fixed direct-CRC domain; HTTP never supplies it.

No official Zoom source located for this target establishes the exact CRC SDES
suite. The configured `AES_CM_128_HMAC_SHA1_80` choice remains a standards and
interoperability decision that requires a live authorized CRC test.

The private PJSIP TLS integration oracle is constrained to TLS 1.2 with
`ECDHE-RSA-AES128-GCM-SHA256`, a suite in Zoom's published CRC policy, and
asserts the negotiated suite as well as certificate name and SNI. This is
offline compatibility evidence only. Zoom's documented `0@dvgo.zmus.us` TLS
test destination is reserved for a separately authorized live check that also
confirms no UDP or TCP fallback occurred.
