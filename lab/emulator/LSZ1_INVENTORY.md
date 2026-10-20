# Synthetic LSZ1 backend contract

Derived from `product/sipd/include/aula_sipd/control_protocol.h`,
`src/control/{framing,protocol,request_decode}.c`, `src/core/endpoint_control.c`,
`src/core/endpoint_metrics.c`, `src/sip/zoom.c`, and the console gateway validators.
The `src/` paths above are relative to `product/sipd/`. The starting contract
was inspected at repository commit `f140ac9`; `tests/lsz1_contract.c` compiles
the current gateway transport and normalizers so contract drift fails tests.

LSZ1 v1 has a 16-byte network-order `4s BB H I I` header (magic, version,
opcode, flags, request ID, payload length), followed by at most 65,536 bytes.
The configured inbound bound and gateway response bound are 2,048 bytes.
Responses set flag 1; errors add flag 2. A connection carries one request and
one response then EOF, matching Darwin STREAM gateway framing. Linux uses one
SEQPACKET packet per frame. Unknown opcodes, bad magic/version, oversized or
truncated frames and trailing bytes close the connection without a reply.
Known nonzero request flags produce INVALID_FLAGS.

| Opcode | Request | Response / rejection |
| --- | --- | --- |
| 1 STATUS | Empty bytes or literal `{}` | Exact gateway status schema; other input INVALID_REQUEST |
| 2 ORIGINATE | Exactly six ASCII string fields: profile, layout, meeting_id, passcode, host_key, dial_code | `{ok:true}`; invalid/state/profile failures COMMAND_FAILED; disabled media PERMISSION_DENIED |
| 3 HANGUP | Empty bytes or whitespace-tolerant empty JSON object | `{ok:true}` or COMMAND_FAILED; invalid body INVALID_REQUEST |
| 4 DTMF | Exactly `{tone:"0..9,* ,#,A..D"}` (one symbol) | `{ok:true}` only when established, otherwise COMMAND_FAILED |
| 5 MEDIA | `{mode:"enabled|disabled"}` (<=32 bytes), or `{action:"audio_mute",value:"enabled|disabled"}`, `{action:"keyframe|layout_next",value:"request"}` (<=48 bytes) | `{ok:true}` only when established with managed media; other failures COMMAND_FAILED |
| 6 REPLACE_CREDENTIAL | Real schema is two strings username (1..256), password (12..256) | Stand-in deliberately returns UNSUPPORTED; never retains credentials |
| 7 DIAGNOSTICS | Literal `{}` only | Exact five checks: configuration, sip_control, media_readiness, tls_profile_policy, renderer_truth; invalid body COMMAND_FAILED |
| 8 SUBSCRIBE | Literal `{}` only | Current bounded call-state snapshot with revision, events array (not an unsolicited push stream); invalid body COMMAND_FAILED |
| 9 SETTINGS | Empty bytes or whitespace-tolerant empty JSON object; or revision (integer 1..UINT32_MAX), profile, media, optional tls | Safe enums only; stale/overflow revision REVISION_CONFLICT; update during call COMMAND_FAILED; postcommit fault PERSISTENCE_UNCERTAIN |
| 10 METRICS | Empty bytes or whitespace-tolerant empty JSON object | Exact revision/reset/streams/poll schema; invalid body COMMAND_FAILED |

Response fields (names are exact; synthetic values describe this fixture):

| Response | Fields |
| --- | --- |
| STATUS | `call_state` (synthetic state string), `media_state` (`none`, `preparing`, or `committed`), `revision:1`, `rx_rendering:false`, `video_transmit_enabled` and `audio_muted` booleans, `reconnect_attempts:0`, `renderer`, `aec`, `sip`, `video`, `audio`, `last_error:0` |
| STATUS.renderer | `healthy:false`; `audio` and `video` each contain `rendering:false`, `fresh:false`, `last_success_ns:0` |
| STATUS.aec | `delay_state:"inactive"`, `available:false`, `active:false`, `processed_frames:0`, `reference_underflows:0`, `resets:0` |
| STATUS.sip | `profile` from settings, `transport:"tls"` or `"udp"` from policy, `registration:"disabled"`, `status_code:0`, `expires_seconds:0` |
| STATUS.audio/video | `direction:"sendrecv"` when established, otherwise `"inactive"`; `security:"rtp"`, `frames:0`, `packets:0`, `lost:0`, `jitter_ns:0`; `payload_type:0` for audio or `96` for video; video adds `h264_profile_level_id:"none"` |
| DIAGNOSTICS | `state:"not_ready"`, `checks` containing the five named objects above, each with a `state` string; configuration/TLS policy are ready, SIP control not ready, media/renderer unavailable |
| SUBSCRIBE | Integer `revision`, `events` array with the current call-state entry: string `id`, `type:"call_state"`, and string `message` |
| SETTINGS read/success | Integer `revision`, enum strings `profile`, `media`, `tls`; an accepted update increments revision |
| METRICS | `revision:1`, `reset:{streams:"session",poll:"process"}`, two `streams` (`kind:"audio"`/`"video"`), each with zero `queue_depth`, `packet_drops`, `access_unit_drops`, `backend_drops`, `backend_restarts`; `poll:{late_count:0,max_lateness_ms:0}` |
| ORIGINATE/HANGUP/DTMF/MEDIA success | `ok:true` |
| Error | `ok:false`, `error:{code:<string>}` and RESPONSE+ERROR frame flags |

Profiles: zoom_direct, zoom_proxy, private_lab. Layouts: gallery, full_screen,
dual_video. Meeting ID: 1..16 ASCII digits; optional passcode <=32 digits,
host key <=16 digits, dial code <=64 ASCII alphanumeric. No JSON escapes,
non-ASCII, duplicate keys, unknown members, booleans, nesting, floats or signs
are accepted by the real bounded command parser. Up to six members, key length
<=31 and string/token length <=256. SETTINGS media is managed/disabled; tls is
verified/required/not_required and is informational on requests: public profiles
force TLS, private_lab preserves prior transport policy.

Only successful mutations and PERSISTENCE_UNCERTAIN are cached, before delivery.
Cache: 16 entries, FIFO replacement, 120 seconds; identity is request ID plus
opcode and exact payload bytes. Matching retry returns original response;
changed mutation with retained ID returns REQUEST_ID_CONFLICT. Reads (including
SETTINGS read) bypass cache. Ordinary failures are uncached. Payload fingerprints
are keyed hashes so the cache never holds meeting/passcode/credential input.
An oversized successful mutation is recorded as `REPLAY_UNAVAILABLE` for retries.
Read opcodes are 1, 7, 8, 10, and empty SETTINGS (9); mutation cache candidates
are 2–6 and nonempty SETTINGS. Unsupported credential requests fail without
entering the cache.

Synthetic behavior: monotonic timed resolving -> inviting -> early ->
establishing_media -> established; hangup -> terminating -> terminated.
Renderer/AEC remain unavailable, actual frame/packet counters remain zero, and
diagnostics never certify hardware or a real SIP registration. Settings alone
can persist to a separately named sibling `<state-file>.sip-settings`; call state,
events and replay cache are process-local. Runtime activity uses its own state
file and is exposed by the separately scoped console activity provider.
Persisted settings are reloaded and validated under the same lock as the
revision comparison and save. Multiple emulator instances cannot overwrite a
newer settings revision with stale state; reads see external committed updates.

Serving requires a caller-owned inherited connected or listening AF_UNIX socket.
The emulator duplicates it and never binds, connects, or opens a network socket.
The activation server bounds clients, read buffers, timeouts, queued replies and
request cache. Faults are explicit local options: deterministic postcommit
PERSISTENCE_UNCERTAIN for SETTINGS, bounded reply delay, and an intentionally
oversized reply for gateway rejection checks.
