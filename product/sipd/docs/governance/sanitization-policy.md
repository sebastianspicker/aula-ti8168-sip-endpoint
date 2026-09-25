# Sanitization policy

Status: normative policy enforced for retained text evidence by
`tools/sanitize-evidence.py`. The validator is deny-by-default: `check` rejects
an unsafe candidate and `collect` additionally writes a new, mode-`0600` copy
outside immutable recovered evidence. Manual review is additional, not a
substitute.

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

## Validator and review requirements

Run `tools/sanitize-evidence.py check --input <candidate>` before a fixture,
log, packet summary, or discovery report is retained, then use `collect` only
for an approved private destination. Supply each configured test credential as
`--known-secret-file <mode-0600-file>` so raw, repeatedly percent-encoded,
hexadecimal, standard-base64, and URL-safe-base64 padded and unpadded forms
are rejected without exposing the secret in the process argument list. These
checks examine each bounded decoded view. The validator opens inputs once with
no-follow semantics and validates and reads that same descriptor. The validator
rejects:

- SIP/SIPS URIs; credential-labelled lines including authorization, password,
  passcode, secret, token, API key, or host key; Call-ID; and cookie headers;
- IPv4, IPv6, MAC-address, serial-number, absolute target-path, and detected
  hostname or endpoint patterns;
- binary/control-byte data, decodable base64-like payloads, and raw SIP/SDP
  start lines; and
- percent-encoded values after bounded normalization, including fully encoded
  labels, compressed and mapped IP literals, and configured-secret encodings.

Positive fixtures must show valid redacted summaries passing. Negative fixtures
must include each prohibited class, near-miss strings, encoded variants, and
binary payload markers. Sanitized output is still sensitive operational
evidence and must receive a human review before external publication. The
validator is a retention gate, not proof that contextually sensitive material
is safe to publish.
