# Device-control companion

This C17 component provides read-only OEM status and a durable job journal,
not a complete AREC interface replacement. The gateway and SIP daemon retain
their existing ownership. No
device mutation, firmware, reboot, storage, transfer, or provider action is
implemented by this service.

## Capability coverage

[`capabilities.json`](capabilities.json) records 187 recovered resource
registrations and their 222 paths, method evidence, proposed console location,
role policy, acceptance requirement, and implementation disposition. It also
maps the recovered administrator UI groups and records the static source,
scheduling, audio, and upload options. These defaults do not prove that an
integration is installed or that hardware supports an operation. OEM conference
UI is disabled in the recovered defaults; maintained Zoom calls use SIPD.

`unimplemented` means missing maintained functionality, not unsupported hardware.
`read_projection_only` covers just the recorder status projection.
`server_authentication` describes the implemented internal login/renewal path and
its administrator credential form; its operation contract names local acceptance tests. No resource
has completed workflow or physical acceptance. The inventory coverage test is
not a workflow test. Product builds never read, import, or link recovered code.

## Build and verification

From the repository root:

```sh
make -C product/console device-control test
make quality
```

The executable is `.work/build/console/ls200-device-control`. It uses the reviewed Jansson and OpenSSL inputs already used by the gateway. Cross builds must override `CC`,
`CPPFLAGS`, and `LDLIBS` with explicit reviewed target inputs and keep `BUILD`
under `.work`. Payload assembly requires an explicit companion binary. The service starts before
the gateway and stops after it; persistent state is retained on removal.

## Local contract

The gateway connects only to `/run/ls200-device/control.sock` and verifies a root
peer. The privileged companion accepts only the resolved `ls200-gateway` UID.
Its dedicated runtime directory is root-owned, mode 0750, with the gateway's
primary group; the socket is root-owned with that group and mode 0660.

`ls200-device-control --launch` provisions descriptor 3 using a maintained local
launcher. An exclusive lifetime lock prevents concurrent launchers. Stale recovery
requires the expected owner, group, permissions, socket type, an ECONNREFUSED
probe, and unchanged inode before unlinking. Live, substituted, or ambiguous
sockets are rejected. `--socket-activation` remains supported with the same
listener address, ownership, and permission checks.

Each connection carries one request and one response. Frames have a four-byte
unsigned big-endian length followed by at most 4096 bytes of UTF-8 JSON. Duplicate
keys and unknown fields are rejected. The sole revision-1 request is:

```json
{"revision":1,"operation":"status"}
```

The response contains exactly `revision`, `recording`, and `streaming`. Each
activity is `active`, `inactive`, `paused`, or `unknown`. Server request handling
has a four-second deadline; the OEM read and any login renewal share a three-second deadline. The
client response wait is bounded to 4.5 seconds. Only a definite expired-session
rejection permits one login renewal and one retry of the read.
Unknown commands close the connection before OEM dispatch. The existing LSZ1
protocol, opcodes, and SIPD public headers are unchanged.

Revision 2 adds correlated read queries on the same socket:

```json
{"revision":2,"operation":"jobs.list","correlation":"query-1","arguments":{"offset":0}}
```

The response has exactly `revision`, `operation`, `correlation`, `outcome`, and
`data`. `outcome` is `succeeded` or `unavailable`; unavailable data is null.
Successful job data contains `jobs` (at most eight entries) and `next` (the next
offset or null). Offsets are integers from zero through 32. Each job exposes
only `id`, `operation`, `state`, and `cancellable`. Correlation identifiers are
1–64 ASCII letters, digits, underscores, or hyphens. The `status` operation
requires empty `arguments` and returns the revision-1 status object in `data`.
Unknown operations and malformed requests close the connection without OEM
dispatch. Credential operations are described below; HTTP job routes remain
unimplemented.

## Persistent job state and recovery

The installer provisions `device/` below the persistent deployment state as a
root-owned mode-0700 directory. The service opens its bind-mounted view at
`/run/ls200-zoom-state/device`. Startup validates its ancestors,
takes an exclusive lifetime lock on `jobs.lock`, and loads `jobs.json`. Files
must be owner-only regular files with one link. An incompatible version,
invalid schema, unsafe file, or concurrent owner prevents startup. State belongs
outside the release payload and must be retained across replacement and rollback.
Do not delete the journal to clear an error or downgrade past its format support.

The version-1 journal holds up to 32 intents. Each intent binds a stable
principal and idempotency key to an operation and canonical request fingerprint.
Exact duplicates return the existing job; conflicting reuse is rejected. The
journal never evicts accepted keys, including completed or cancelled jobs. A
full journal rejects new intents; safe archival and expanded retention are not
implemented. Arguments, credentials, OEM responses, and media are not stored
in job records or exposed in job queries.

The maintained C journal API records `queued` intent before dispatch and requires
a durable `running` transition before a caller may send an OEM mutation. These
writes use an exclusive temporary file, file synchronization, atomic rename,
and directory synchronization. A failed write disables the handle until restart;
callers must not dispatch after any journal error. An interrupted temporary file
is removed only after validation under the exclusive lock. The committed journal
remains authoritative.

On startup, `running` becomes `uncertain` durably. Queued and terminal jobs stay
visible. Uncertain jobs can resolve only to `succeeded` or `failed` after external
readback; they cannot return to running. Only queued jobs can become `cancelled`.
There is no worker, automatic replay, percentage estimate, or browser cancellation
endpoint. Operation-specific dispatch and reconciliation adapters still need to
use this API before device mutations can be enabled.

## OEM boundary

OEM access is restricted to `POST /api/login` and `GET /api/recorder/status`
through `/var/run/webapi/webapi.sock` (canonical `/run/webapi/webapi.sock` where
`/var/run` is an alias). The peer must be root and the socket path must pass
ownership and ancestor checks. No TCP target, arbitrary path, or browser cookie
is forwarded.

Administrators provision credentials under Settings → Accounts → Device connection.
`GET /zoom/api/v1/device/credentials` returns only `revision`,
`credentials_present`, and `connection`. `PUT` accepts exactly `revision`,
`username` (1–64 UTF-8 bytes), and `password` (1–128 UTF-8 bytes). Colons and
control characters are rejected because the recovered protocol splits the decoded
credential at colons. Credentials are write-only; changing them does not change
console accounts or SIP credentials.

The gateway enforces admin, Origin, CSRF, and idempotency policy and revalidates
the session before admission and after I/O. Revision-2 `credentials.status`
uses empty arguments; `credentials.replace` uses the PUT body. The gateway
binds its correlation digest to principal, route, and idempotency key. The
companion atomically stores credentials, revision, and up to 32 durable receipts
in version-1 `oem-credentials.json` (0600, one link). Identical retries do not
rewrite credentials or reset the session; conflicting keys and stale revisions
return 409. No receipts are evicted. Capacity requires a supported future
migration, never deletion of the store. Failed persistence disables credential
operations until restart and returns `PERSISTENCE_UNCERTAIN` when applicable.

Login is serialized by the companion event loop. The recovered login body
contains base64 `username:password` and `tab_id: 0`. Session cookies stay in
memory. A read may renew once after HTTP 401 with integer OEM code 103; malformed
responses, timeouts, and other rejections do not trigger renewal. Login attempts
are limited to one per 30 seconds within the process, including after credential
replacement. Connection states distinguish unconfigured, unknown, connected,
authentication failure, and backoff. The previous manually provisioned
`oem-session` file is no longer consumed; enter credentials through Settings.

Do not copy credentials or job state into release payloads. On restart, a safe
incomplete credential temporary file is discarded under the companion's state
lock; the committed file and its receipts remain authoritative. Incompatible
state prevents startup. Rollback/removal retain state and never reset receipts.

HTTP responses require a bounded Content-Length; chunked, oversized, malformed,
ambiguous replies fail closed. Cookie values are bounded; duplicate session
cookies and unsafe characters are rejected. Raw destinations, filenames, OEM
errors and sessions are never included in the projected response. A stopped
recorder with no streams projects inactive activities; recognized healthy file
and network streams project their reported activity. Unknown formats or stream
errors project unknown. This projection is informational and must not be used
as a shared activity reservation or authorization to perform disruptive changes.

## HTTP and rollout limits

Authenticated viewers may read `/zoom/api/v1/device/status`. The gateway
rechecks the console session after companion I/O. A missing companion returns
503 `DEVICE_UNAVAILABLE`; it cannot be mistaken for stopped recording.
Library and schedule reads return 501 `CAPABILITY_UNAVAILABLE`; device-settings
and maintenance reads require admin and also return 501. Unimplemented mutations
are not registered. These routes preserve the existing JSON envelope.

The companion cannot yet provide full parity. Recorder mutation dispatch, shared
SIPD activity reservations, revision-bound configuration, job workflow integration,
operation-specific ambiguous-outcome readback, controlled transfer staging, full UI workflows,
ARM release ABI acceptance, and physical acceptance are absent. Do not switch
the device entry point or restrict the OEM interface on the strength of host
tests or this inventory. Existing deployment, recovery, and authorization gates
remain applicable.
