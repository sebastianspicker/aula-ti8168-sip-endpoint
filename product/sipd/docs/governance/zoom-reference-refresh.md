# Zoom reference refresh checklist

Status: dated pre-live gate. These references describe product configuration
and policy context; they do not prove that a particular codec, transport,
account mode, or endpoint interoperates with this daemon.

## References retrieved 2026-08-27

- [Conference Room Connector requirements](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0060661)
- [SIP dial-string formats and commands](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0065727)
- [SIP TLS cipher policy](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0057632)
- [SIP Video Interop feature and resolution distinctions](https://support.zoom.com/hc/en/article?id=zm_kb&sysparm_article=KB0058000)
- [Meeting SDK for Linux platform requirements](https://developers.zoom.us/docs/meeting-sdk/linux/get-started/download/)
- [Video SDK CRC callout helper](https://developers.zoom.us/docs/video-sdk/web/sip/)

The official SDK review found no CRC SIP/RTP/SRTP implementation suitable for
the 32-bit ARM EABI5 target. Meeting/Video SDK packages and their proprietary
runtimes are excluded from the firmware dependency closure. A modern-host
Video SDK CRC callout harness remains optional and entitlement-gated; it must
never become a default build or CI dependency.

## Typed SIP dial model, recorded 2026-08-26

The optional signaling wrapper models the official SIP positional form as
`meeting.passcode.command.host-key.reserved.dial-code@domain`. It accepts only
typed meeting ID, optional passcode, gallery/full-screen/dual-video layout,
optional host key, and optional dial code inputs. The reserved field is always
empty, and direct CRC always uses `zoomcrc.com`; raw HTTP(S) targets are not a
signaling input. The resulting dial target is secret material and is never a
diagnostic value.

This is an offline construction/validation rule, not current-account or live
interoperability evidence. The official dial-string reference must still be
refreshed immediately before an authorized Zoom attempt.

## Mandatory pre-live refresh

Immediately before an authorized live test, record the retrieval timestamp and
review the then-current official sources for:

1. account entitlement, licensing, and Conference Room Connector availability;
2. approved dial-string form, meeting authorization, and command behavior;
3. required transport, encryption, certificate, and TLS policy for the chosen
   account and regional endpoint;
4. current video-interoperability limitations and the selected resolution mode;
5. any directly documented codec, payload, SDP, DTMF, NAT, firewall, and
   endpoint requirements applicable to the planned test;
6. differences from this project's implemented G.711/H.264, UDP-first,
   bounded receive and renderer profile; and
7. the explicit operator, account, network, cost, capture, and recovery
   authorization for the test.

The live TLS sub-gate may call Zoom's documented `0@dvgo.zmus.us` confirmation
destination only after those approvals are recorded. It must capture the
negotiated TLS version and cipher and prove that the endpoint did not fall back
to UDP or plaintext TCP. The offline private-peer oracle uses TLS 1.2 with
`ECDHE-RSA-AES128-GCM-SHA256` and does not substitute for that result.

If current requirements conflict with the implemented profile, stop before
contact with Zoom, retain only sanitized reference notes, and open a new
reviewed decision. Do not infer codec support from a page that does not state
it.
