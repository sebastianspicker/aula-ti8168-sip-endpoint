# Synthetic emulator fidelity

The emulator provides host fixtures for maintained Aula interfaces:

- Recording and streaming activity in the console's `active`, `inactive`,
  `paused`, and `unknown` vocabulary, with monotonic revisions and explicit
  atomic JSON persistence.
- Bounded in-process activity events. These are a Python library API; separate
  CLI invocations do not share subscriptions.
- An LSZ1 daemon stand-in with synthetic call progression, settings, status,
  diagnostics, metrics, event polling, mutation replay, and fault injection.
- The companion's revision-1 status response over its length-prefixed Unix
  protocol. Job execution and credential management are not emulated.

The optional `make emulator-ui` composition serves the maintained React UI
through native nginx/TLS and the real C FastCGI gateway, using these providers.
Its browser interactions exercise real account/session, Origin, CSRF,
directory and gateway validation with synthetic backend state. The separate
emulator-mode bundle visibly identifies that environment. This host
composition does not deploy the console inside the original ARM guest.

Protocol fixtures are derived from maintained product C sources. Contract
tests exercise the real C clients against the Python servers through inherited
socket pairs. The servers also accept inherited listening Unix sockets; they
do not create or remove socket paths.

Synthetic call transitions and metrics are deterministic fixtures, not SIP,
SDP, RTP, RTCP, recording files, RTSP preview, or real media. The emulator does
not model a TI8168 board, vendor control protocols, firmware APIs, serial
interfaces, or physical behavior. Its results establish no hardware, Zoom,
production TLS, or media acceptance.

The supported boolean `synthetic-local-state-v1` activity snapshot migrates
losslessly to the four-state `synthetic-local-state-v2` model. Other schemas,
including historic vendor snapshots, remain incompatible. Recovery reset is
explicit and cannot preserve a revision that is absent or untrustworthy.

Validation of physical behavior requires separately authorized hardware work.
