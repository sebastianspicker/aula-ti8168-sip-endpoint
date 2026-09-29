# Sanitization policy

Status: normative policy for diagnostic text and operational summaries.
Automated redaction is not a substitute for human review.

## Prohibited material

Do not version, print in ordinary logs, or publish any of the following:

- meeting IDs, meeting passcodes, host keys, dial strings, full SIP URIs, user
  names, digest responses, authorization headers, cookies, or secret-file data;
- raw SIP/SDP messages, packet captures, private IP addresses, MAC addresses,
  device serials, DNS names, public endpoints, or full Call-IDs;
- raw audio, video, H.264 NAL payloads, PCM, RTP payloads, or screenshots that
  can identify people, meetings, locations, or devices;
- unreviewed `gst-inspect`, process, mount, interface, or configuration output.

## Allowed representations

| Data class | Allowed form |
| --- | --- |
| SIP peer and addresses | stable role token such as `peer-1` or a salted, non-reversible review identifier |
| Call correlation | daemon-generated random correlation ID, never the SIP Call-ID |
| URI | scheme and field-presence summary only, for example `sip:credential-present@peer-1` |
| SDP | codec, payload type, direction, and port-presence summary; no raw attribute lines |
| RTP/RTCP | header and aggregate counters only; no payload bytes |
| Media | codec/profile/level/frame-size/rate/counter summary only |
| Credentials | `present`, `absent`, or `redacted`; never length, prefix, suffix, or hash |
| Paths | approved project-relative path or role token; redact target/home paths where publication could expose identity |

## Review requirement

Sanitized output remains sensitive operational material. Review it for both
the prohibited classes above and contextual identifiers before sharing it.
Do not retain a diagnostic merely because an automated redaction step accepts
it.
