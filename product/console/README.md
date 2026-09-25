# LS200 Console

LS200 Console provides Room, Library, Schedule, and Settings navigation using
the existing Zoom console design system. Room retains Zoom joining, directory,
recents and call controls alongside the authenticated local preview. Meeting
mute and meeting layout are labelled separately from device input and picture
settings. An activity bar keeps meeting hangup available on other destinations.
The previous Zoom interface remains available at `/zoom/#legacy`.

This is an incomplete unified interface: Library, Schedule and OEM settings
explicitly report unavailable functionality. They do not invent empty data or
claim unsupported hardware. The optional C17 device companion implements
read-only recording/streaming status and a persistent job journal with restart
reconciliation. Settings → Accounts includes a write-only OEM credential form with revision checks
and automatic bounded session renewal. Recorder commands and browser job workflows
are not connected yet. See its
[coverage and contract](device/README.md) before enabling it. The original OEM
interface and default device entry point have not been changed.

## Components

| Path | Responsibility |
| --- | --- |
| `web/` | React 19, TypeScript, and Vite browser application |
| `gateway/` | Authentication, authorization, persistence, HTTP schemas, LSZ1 translation, and preview delivery |
| `nginx/` | TLS, static UI, request limits, browser headers, and explicit FastCGI route allowlist |
| `device/` | Bounded OEM status, persistent job journal, and capability inventory |
| `tests/` | Gateway, FastCGI mapping, preview, and full request-contract tests |

The browser calls same-origin `/zoom/api/v1/*` routes. nginx forwards only
listed routes to the gateway. The gateway validates the request, enforces
viewer/operator/admin roles, and sends typed LSZ1 messages over the daemon's
Unix socket. It validates the daemon response before exposing a safe JSON
projection.

The [gateway contract](gateway/README.md) is the API and security source of
truth. The [preview integration contract](gateway/preview/INTEGRATION.md)
defines the socket, parser, H.264, and FLV ownership boundary.

## Build and test

From the repository root:

```sh
make -C product/console test
make -C product/console ui-install ui-test ui-build
```

Gateway tests require a C compiler plus `pkg-config` metadata and development
files for Jansson and OpenSSL. UI installation uses the checked-in pnpm lock and
stores dependencies below `.work/cache`. Both UI targets declare installation as
a prerequisite. Concurrent commands retain shared leases and the dependency
symlink until the last user exits; installation changes and cleanup require
exclusive access. The build writes
`.work/dist/console-web`.

After `ui-install`, start the UI-only development server with:

```sh
sh tooling/console/with-dependencies.sh vite --host 127.0.0.1
```

This does not start nginx, FastCGI, or `ls200-sipd`. Run the root
`make test-product` for the integrated source gate.

The optional FastCGI executable requires a separately reviewed FastCGI 2.4.7
installation:

```sh
make -C product/console fastcgi FCGI_PREFIX=/path/to/reviewed/fastcgi
```

Gateway translation units compile into separate objects with compiler-generated
dependency files below `.work`, including transitive private C includes.

No console build target downloads third-party source.

## Runtime configuration

[`gateway/config/ls200-console.example.conf`](gateway/config/ls200-console.example.conf)
is a schema example, not production configuration. The required policy defines
the browser Origin policy, daemon control socket, account store, expected daemon
UID, and optional fixed preview source. Set `allowed_origin` to `"device"` for
DHCP devices: the gateway matches HTTPS origins against nginx's local connection
address and port, without trusting browser-supplied hostnames. An explicit
`https://...` value retains the fixed-origin policy. The FastCGI process loads the strict
policy first and then its account store.

Production nginx configuration is
[`nginx/nginx.conf`](nginx/nginx.conf). Certificates, keys, account/bootstrap
state, and runtime policy are operator-owned external files. The QEMU profile
uses a separate development certificate and must not be treated as trusted
production TLS. The PCRE-free `/zoom/assets/` prefix serves only Vite's
content-fingerprinted build directory with immutable caching; the broader
`/zoom/` location remains non-cacheable. `/zoom` redirects to `/zoom/` using a
relative URL, preserving the address used by the browser.

## Authentication and state

The gateway owns password hashes, accounts, sessions, CSRF state, token-rotation
grace, idempotency records, directory entries, safe recent-call references,
event history, and one preview lease. Sessions, CSRF values, export identifiers,
and idempotency entries are memory-only. Account, directory, and recent-call
state uses the gateway's owner-only atomic store.

The four main destinations remain visible for every role. Settings requires an
administrator before loading protected data. Its groups are Device, Picture &
sound, Outputs, Network, Storage, Accounts, and Maintenance. SIP policy and write-only SIP credentials live in
Outputs, console accounts in Accounts, and diagnostics in
Maintenance. The previous Zoom interface retains its administrator-only
navigation. Diagnostics exports download the gateway's redacted JSON response
as a local file.

State operations hold a gateway mutex; daemon exchanges hold a separate control
mutex. Transactions copy bounded requests and principals, recheck authorization,
expiry and completed idempotency after waiting, then finish under the state
mutex. Local routes continue during daemon I/O. Revocation before admission
prevents dispatch; already-issued commands finish without stale session writes,
cookies, or diagnostic export tokens.

Mutations require exact Origin, CSRF, authorization, and an idempotency key
bound to the request method and canonical body. Credential values are write
only. Raw SIP destinations, internal paths, arbitrary diagnostic detail, and
untrusted preview addresses never cross the HTTP boundary.

JSON requests time out after ten seconds per attempt, including body reads.
Protected mutations may retry once with the same operation identifier; reads,
login/bootstrap, canceled requests, malformed replies, and persistence-uncertain
results do not retry. Polling cancels obsolete reads on hide or unmount, refreshes
once on visibility restoration, and waits five seconds after completion.
Mutations continue when the tab is hidden.

## UI maintenance

The product intent is documented in
[`docs/product/PRODUCT.md`](../../docs/product/PRODUCT.md); the visual and
interaction contract is in
[`docs/product/DESIGN.md`](../../docs/product/DESIGN.md). Non-shipping concept
images live under `docs/product/design/concepts/` and must never be bundled as
runtime media or substituted for live device output.

## Status snapshots and runtime metrics

Calls requests one daemon status snapshot per refresh, plus gateway-local recents,
directory collections, and authenticated preview metadata. Preview metadata errors
do not discard a successful call snapshot. Media requests one status snapshot plus authenticated,
gateway-local preview metadata. Shared browser projections distinguish established SIP signaling, negotiated media
direction and security, local source-frame reads, and receive rendering. Idle
stream defaults are not presented as negotiated codecs or security. Send-only
streams do not require a receive renderer. Source-frame counts do not prove remote
reception; local preview is independent of the SIP call. A failed refresh marks
the retained connection snapshot stale. The legacy HTTP routes remain supported.

Diagnostics displays current call state separately from bounded readiness checks.
The legacy literal version value `1` is not presented as a release version.
Missing timing, remote reception, or release telemetry remains unreported.

Administrators can request `/zoom/api/v1/diagnostics/metrics` (read-only LSZ1
opcode 10) through **Load runtime metrics**. This snapshot is not polled. Its
revision-1 schema contains `reset`, ordered audio/video `streams`, and `poll`.
Stream queue depth, packet/access-unit drops, backend drops and restarts reset
with the media session. Audio access-unit drops are zero; shared backend counters
are repeated on both streams. Poll late count and maximum lateness in milliseconds
reset with the process. All counters saturate at 2147483647. An older daemon's
unsupported response affects only this panel and leaves legacy status intact.

Production asset URLs begin with `/zoom/`. Only fingerprinted JavaScript and CSS
under `/zoom/assets/` receive a one-year immutable cache policy. HTML and
unversioned files remain `no-store`; both policies preserve the browser security
headers. Release assembly must retain the referenced asset files alongside its
HTML, and a release switch must serve the new HTML without a cached index.

The ARM nginx build operates on a verified source copy under `.work/build/live`.
The wrapper requires an explicit canonical build directory beneath `.work` and
reviewed sysroot/compiler inputs outside the evidence corpus. The live build
leaves the reviewed source tree unchanged and records artifacts from the staged
copy.
