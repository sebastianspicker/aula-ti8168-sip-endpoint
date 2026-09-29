# Console gateway

The unprivileged HTTPS-to-LSZ1 boundary exposes no raw SIP URI, destination,
path, executable, or environment-driven control input. Every response uses
revision 1: `{"revision":1,"ok":true,"data":...}` or
`{"revision":1,"ok":false,"error":{"code":"...","message":"..."}}`.

## Module map

`gateway.h` is the only header other components may include; `gateway_internal.h`
is the interface shared by the modules below, and nothing outside this
directory may include it. Each module below is its own translation unit, compiled and linked separately
(`gateway/sources.mk`, read via `make -s -C product/console print-gateway-sources`);
`fastcgi_main.c`, `fastcgi_request.c`, and `fastcgi_preview.c` are additional,
independent translation units that use only the public `gateway.h`/`fastcgi_*.h`
interfaces.

| Module | Responsibility |
| --- | --- |
| `gateway_core.c` | Crypto, hex, JSON-shape, and verified-filesystem primitives shared by every other module |
| `gateway_account.c` | Account identity/CRUD, the login/bootstrap auth-rate budget, and `gateway.conf` policy loading |
| `gateway_account_store.c` | Deserializes and validates the persisted account/directory/recents store (v1/v2/v3) |
| `gateway_store.c` | Loads and atomically rewrites the persisted account store; password verification |
| `gateway_session.c` | Session lifecycle, cookie/token handling, Origin policy, and request authentication |
| `gateway_schema.c` | Generic JSON request-body schema helpers (`{}`, exact-keys, enum-value) |
| `gateway_idempotency.c` | Per-session mutation idempotency records and their request fingerprint |
| `gateway_preview_metadata.c` | Local preview capability projection |
| `gateway_settings.c` | Backend settings envelope validation |
| `gateway_aec.c` | Backend AEC status envelope validation |
| `gateway_status.c` | Backend call/media status envelope validation and safe media projection |
| `gateway_metrics.c` | Backend diagnostics-metrics envelope validation |
| `gateway_route_schema.c` | HTTP request-body schema validators referenced by the route table |
| `gateway_route_backend.c` | Backend response normalization (`aula_gateway_backend_response_normalize`) and status-route shaping |
| `gateway_route_events.c` | Call-state event validation and the bounded, de-duplicated event history |
| `gateway_route_directory.c` | Safe directory and recent-call collection validation, storage, and mutation |
| `gateway_route_users.c` | HTTP-driven account listing and mutation |
| `gateway_auth_routes.c` | `/auth/login` and `/auth/bootstrap` |
| `gateway.c` | The route table (`ROUTES[]`) and the per-request dispatch plan built from it |
| `gateway_route_response.c` | Shapes the HTTP response for local and backend-dispatched routes |
| `gateway_route_request.c` | Request validation and redacted/cleansed payload preparation |
| `gateway_device.c` | Unprivileged passthrough to the device companion's credential/status protocol |
| `gateway_transaction.c` | Copies and revalidates a request across control I/O; `aula_gateway_handle` |
| `gateway_control.c` | The LSZ1 wire transport to `aula-sipd` |

Request handling uses a state mutex and separate serialized read and mutation
control lanes. At most one read and one mutation exchange may run concurrently. Bounded
transactions retain copied requests and principals across I/O, then recheck
session lifetime, token grace, CSRF, account permissions, and idempotency before
dispatch. Monotonic elapsed time advances request timestamps while waiting.
Local routes and the read lane can progress during mutation I/O. Issued commands may finish after
revocation, with stale cookies and session-bound exports suppressed.

| Route | Method | Role | LSZ1/schema |
| --- | --- | --- | --- |
| `/auth/bootstrap`, `/auth/login` | POST | none | strict credentials schemas; bootstrap is one time |
| `/auth/logout` | POST | viewer | CSRF/idempotency; `{}` |
| `/auth/session` | GET | viewer | safe role envelope |
| `/status`, `/calls/active`, `/media` | GET | viewer | opcode 1 status request; route-specific safe projection |
| `/media/preview` | GET | viewer | gateway-local authenticated preview metadata; no daemon exchange |
| `/calls`, `/directory` | GET | viewer | persistent safe recents or directory references; no secrets |
| `/events` | GET | viewer | opcode 8; bounded, de-duplicated event history |
| `/calls` | POST | operator | opcode 2; exact `meeting_id`, `profile`, `passcode`, `layout`, `host_key`, `dial_code` object |
| `/calls/active` | DELETE | operator | opcode 3; `{}` |
| `/calls/active/dtmf` | POST | operator | opcode 4; allowed DTMF tone only |
| `/calls/active/media` | PATCH | operator | opcode 5; video mode, audio mute, keyframe request, or Zoom layout cycle |
| `/directory` | POST/DELETE | admin | revision-bound safe reference mutation; no passcode, host key, participant code, credential, or SIP destination |
| `/settings` | GET/PATCH | admin | opcode 9; authoritative safe live policy and revision-bound atomic mutation |
| `/settings/credentials` | POST | admin | opcode 6; sensitive payload is wiped after daemon exchange and reply is redacted |
| `/users` | GET/POST/DELETE | admin | bounded account listing and revision-bound mutation; passwords are write-only |
| `/diagnostics` | GET | admin | status opcode with a shaped safe response |
| `/diagnostics/export/{opaque_id}` | GET | admin | session-bound, expiring, redacted status export selected by a 32-character hexadecimal ID |
| `/diagnostics/metrics` | GET | admin | read-only opcode 10; strict revision-1 reset/streams/poll counters, independent of legacy status |
| `/diagnostics/tests` | POST | admin | opcode 7; requires the exact `{}` body and returns the bounded live diagnostic result |

Settings mutations accept revision, profile, and media. Legacy TLS fields remain
accepted but their claim is ignored; updated clients omit the field. The daemon
projects transport policy as `required` or `not_required`. Certificate status is
always “Not observed,” including when an older backend supplies `verified`.

The daemon validates a complete candidate while idle and commits its owner-only
settings file through a synced temporary file and atomic rename. Rename publishes
the settings and revision. Pre-rename failures preserve the old state; a later
parent-directory sync failure returns `503 PERSISTENCE_UNCERTAIN` with the new
state authoritative. The gateway retains that exact uncertainty response for
idempotent replay. Stale revisions return `409 REVISION_CONFLICT`. Empty settings
reads are never served from the daemon's mutation deduplication cache. This
contract does not establish target-filesystem power-loss durability.

The diagnostics status response
issues a session-bound opaque export ID; its export route re-fetches and
revalidates the redacted status projection, expires with the session-bound
lease, and never serializes credentials or dialing secrets. Repeated diagnostics
refreshes retain the current unexpired ID and extend its lease, so polling cannot
invalidate an export action already rendered by the browser. Video, audio-mute,
keyframe, and layout controls are live opcode-5 operations against an
established committed media session. The Zoom layout action emits the official
SIP Video Interop `1`, then `1` RFC 4733 sequence. `POST /diagnostics/tests` is a non-mutating opcode
7 query protected by the same admin, Origin, CSRF, and idempotency boundaries.
It returns 200 once the daemon ran the bounded query, including when hardware
checks are `unavailable` or `not_ready`. Its fixed, duplicate-rejecting backend
schema contains an overall state and the five state-only checks
`configuration`, `sip_control`, `media_readiness`, `tls_profile_policy`, and
`renderer_truth`; it accepts no paths, credentials, identifiers, or arbitrary
diagnostic detail. Renderer truth is `unavailable` until a dedicated runtime
health interface exists, and media/overall hardware readiness never claim a
pass from host fixtures. `GET /diagnostics` remains a separately shaped opcode
1 status projection. Typed LSZ1 originate, DTMF, hangup, media-control,
credential-update, and event-snapshot routes remain separate daemon operations;
a successful gateway exchange and
the host-fixture proxy-registration proof are not provider-issued credential,
public Zoom, SRTP, or hardware acceptance.

All protected routes require the configured Origin policy. `allowed_origin` may
be an exact static HTTPS origin or `device`. The `device` policy derives the
exact numeric HTTPS origin for each request from nginx's `SERVER_ADDR` and
`SERVER_PORT` FastCGI parameters, so a DHCP address change does not require a
policy rewrite. A `localhost` origin is accepted only when `SERVER_ADDR` is a
loopback address. The gateway never derives this decision from `HTTP_HOST` or a
forwarding header. Mutations additionally require CSRF and a bounded idempotency
key. Each retained key is bound to the
HTTP method and SHA-256 of the validated canonical JSON body; an exact replay
returns its retained response, while reuse for different content fails with
`409 IDEMPOTENCY_CONFLICT`. The gateway derives a stable opaque LSZ1 request ID
from the session, route, and idempotency key. `sipd` retains bounded completed
command replies before delivery and binds them to a per-daemon keyed 128-bit
request fingerprint, so a dropped reply can be retried without retaining a
plaintext secret-bearing request or executing a completed command twice. The
cache is deliberately bounded and volatile across daemon restarts. Schemas reject duplicate/unknown keys and
non-object values. Sessions are 256-bit random values retained as
server-side SHA-256 hashes, capped at four concurrent sessions, idle-expire in
15 minutes, absolute-expire in eight hours, and rotate at 15 minutes. Up to
eight prior token hashes remain valid for a bounded 60-second delivery grace, and a rotated
cookie is retained when downstream route processing returns an error. Cookies
are always `Secure; HttpOnly; SameSite=Strict; Path=/zoom/`. Cookie parsing accepts
only exact semicolon-delimited `aula_session` pairs and rejects duplicates;
unrelated cookie names and values cannot supply a session token.

Passwords use PBKDF2-HMAC-SHA256 with exactly 600000 iterations, a 16-byte
salt, and a 32-byte output. Login and bootstrap spend that work only after
application-side account, source, and global token budgets admit the request;
429 responses carry `Retry-After`. The FastCGI adapter accepts the source only
from nginx's `AULA_CLIENT_ID` parameter, not client JSON or forwarding headers.
Credential failures perform the same PBKDF2 work for configured and absent
accounts. Login snapshots credentials and their revision under the state mutex,
reserves the single hashing slot, then performs PBKDF2 outside that mutex. An
occupied slot returns the existing rate-limit response. After hashing, account
and credential revision revalidation prevents a changed account from receiving
a session; secret buffers are cleansed on all completion paths.

The fixed 0600 `gateway.conf` policy is strict JSON and requires
`allowed_origin`, `control_socket_path`, `account_store_path`, and
`expected_sipd_uid`; it is parsed from a verified descriptor without argv or
environment secrets. `allowed_origin` is either `device` or one exact HTTPS
origin; missing or malformed server address/port metadata is rejected only for
the `device` policy. An optional `control_timeout_milliseconds` may reduce the
fixed two-second maximum. One monotonic end-to-end deadline covers nonblocking
connect, complete send, and complete reply; stalled, partial, trailing, or
truncated LSZ1 traffic closes the socket and becomes `503 BACKEND_UNAVAILABLE`.
Descriptor-walked no-symlink ancestors and final files are
checked before policy/account reads. The account/bootstrap, safe-directory,
and safe recent-reference store uses a 0600
`openat` temporary file, file fsync, `renameat`, and directory fsync. The Unix
control socket has the same safe parent/final validation and its connected peer
must report the configured sipd UID through `SO_PEERCRED` or `getpeereid`.
LSZ1 replies are duplicate-rejecting JSON with an opcode-specific response
allowlist, then compactly reserialized before they cross the HTTPS boundary.
Sessions, CSRF material, and diagnostics export IDs intentionally remain
memory-only. The bounded v3 store migrates v1/v2 account data without inventing
directory or recent entries.

The store distinguishes failure before `renameat` from a committed rename whose
parent-directory durability is uncertain. Callers restore their in-memory
snapshot only for the pre-commit result; after a successful rename they retain
the visible state and never report that the write did not occur. On Darwin,
documented `EINVAL` or `ENOTSUP` directory-sync capability results are accepted
after `F_FULLFSYNC` when available (then file `fsync`).

The optional FastCGI 2.4.7 adapter and nginx 1.31.4 profile never download
third-party sources. The LSZ1 client reuses the daemon's public
`aula_sipd/control_protocol.h` directly for the magic, version, opcode enum,
header size, and frame flags; a gateway-local `GATEWAY_LSZ1_MAX_PAYLOAD`
enforces the gateway's own tighter payload bound (statically asserted against
the daemon's wire limit) over network-order flags/request ID/length and
bounded JSON. Gateway tests cover the capability-unavailable settings response, account and
directory revision conflicts, v1/v2/v3 persistence, safe recents and event
history, export expiry, and the typed supported request boundaries; they are
not a guest deployment or live-device result.

For the [TI8168 media board](../../../docs/reference/ti8168-board.md),
`config/build-gateway-arm.sh` accepts only the explicit reviewed ARM sysroot,
cross-prefix, and an existing output directory. It links the pinned Jansson,
OpenSSL, and FastCGI static archives and refuses to read or write beneath the
repository evidence corpus. Generated output must resolve
strictly beneath the canonical repository `.work` directory.

Run `make test-fastcgi FCGI_PREFIX=/reviewed/native/fcgi` from the repository
root to exercise the actual workers, request mapping, preview cancellation,
and lease release. The core gateway and preview unit checks remain
separate from this optional runtime integration lane.

## Device companion routes

`GET /zoom/api/v1/device/status` requires a viewer session and exchanges a
bounded revision-1 message with the privileged device companion. This runs in the
read lane, independently of SIPD mutations, and rechecks the console session
before releasing the result. The response exposes only recording/streaming
activity enums; unavailable transport returns `DEVICE_UNAVAILABLE` (503).

The gateway links only the client half of the device protocol (`device/client.c`,
transport, wire framing, strict projected-status validation, and credential *schema*
validation). The companion's serving loop and its persistent credential store
(`device/server.c`, `device/credentials_store.c`) link only into
`aula-device-control`; see the [companion contract](../device/README.md).

`GET /zoom/api/v1/library` and `/zoom/api/v1/schedule` require viewer access.
`GET /zoom/api/v1/device/settings` and `/zoom/api/v1/maintenance` require admin
access. These four workflows return `CAPABILITY_UNAVAILABLE` (501). They are
explicitly unimplemented, not empty collections. No OEM mutation route or raw
proxy is registered. See the [companion contract](../device/README.md).

`GET /zoom/api/v1/device/credentials` requires admin and projects only credential
presence, revision, and connection state. `PUT` requires admin, exact Origin,
CSRF, and an idempotency key, and accepts username, password, and the revision
read before editing. The companion persists credentials and bounded deduplication
receipts atomically. Stale revisions and conflicting operation keys return 409;
uncertain persistence returns 503 without an automatic retry. Session validation
runs before companion admission and after I/O. Credential request copies are
cleansed, and neither credentials nor OEM cookies appear in responses.
