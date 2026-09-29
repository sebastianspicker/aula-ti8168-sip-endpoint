# Physical target private-lab workflow

The maintained public campaign CLI refuses all physical actions before loading
a session or contacting a target. Physical campaigns require separately reviewed
private identity tooling and action-specific authorization. The contract below
describes an owned target device on an isolated link; it is not a public Zoom
test, release approval, firmware update, or authorization to access another device.

The workflow can install, start, smoke-test, soak, reboot, roll back, and remove
project-owned state. It never writes MTD/UBI firmware partitions. It depends on
a separately managed root-SSH and recovery substrate that this project does not
install or remove.

## Required private inputs

Keep all target-specific values outside version control. Before contact, the
operator must have:

- documented ownership and recovery authority for the unit;
- a mode-`0600`, singly linked `aula-ti8168-sip-endpoint-live-session-v1` JSON file below `.work`
  or ignored `evidence/private`;
- the approved host interface, target/host addresses, expected MAC, and
  independently verified SSH host-key fingerprint;
- reviewed version identifiers, port ranges, resource limits, and approved
  phase list;
- an absolute hash-reviewed `aula-ti8168-sip-endpoint-live-build-inputs-v1` manifest; and
- an absolute reviewed ARM entropy helper for the QEMU gate.

The session parser rejects unknown or duplicate keys, secret fields, public or
special addressing, a target and host outside the same private `/24`, equal
target and host addresses, inconsistent interface/routing data, unsafe links or
permissions, and relaxed capture limits. The target address is supplied by the
session; every phase still binds it to the exact approved interface, local host
address, expected MAC, SSH port, and host-key fingerprint. Do not place passwords, bootstrap
codes, console credentials, cookies, CSRF values, private keys, or certificate
contents in the session file.

Start the build manifest from
[`live-build-inputs.example.json`](live-build-inputs.example.json). Every
placeholder intentionally fails until replaced with an approved private path
and lowercase SHA-256 value.

## Build, package, and gate

From the repository root:

```sh
LIVE_BUILD_MANIFEST=/absolute/private/build-inputs.json make live-build
make live-package
QEMU_ENTROPY_HELPER=/absolute/reviewed/arm-helper make live-gates
```

`live-build` consumes the manifest's reviewed toolchain, sysroot, nginx,
native dependency, and ABI inputs plus the prepared, locked console dependency
cache. Add the optional `toolchain.node` path and SHA-256 record when the host
Node executable is outside the restricted system PATH; the example includes it.
The UI build requires a ready cache and cannot install dependencies.
`live-package` emits a restrictive digest receipt
only after the independent payload verifier accepts the runtime. `live-gates` requires the repository gate and two-slot QEMU product acceptance
before binding a receipt to the payload, build inputs, entropy helper, and
maintained-source digest. The old acceptance lane is absent from the synthetic
QEMU model, so this gate currently fails closed and cannot issue a receipt.

Any source or input change invalidates the receipt. Do not edit or replace an
approved payload after packaging.

## Console address

For DHCP access, set `allowed_origin` to `"device"` in the persistent
`gateway.conf`. The gateway validates the browser origin against nginx's local
connection address and port for every request. Any permitted network client can
use `https://<current-device-address>:8443/zoom/`; this setting does not restrict
the client IP. The TLS certificate must separately cover the address or trusted
DNS name used by the browser. Campaign HTTP requests use the session's target
address, so refresh the private session and verify identity after an address
change.

## Ordered campaign

The maintained `live-*` campaign actions cannot run until a separately
reviewed private identity provider is supplied. A private campaign plan must
order preflight, install, smoke, soak, reboot, and removal. Before contact or
mutation it must revalidate route, neighbor and SSH identity, root-SSH
substrate, media readiness, capacity, package, and gate receipts. The offline
contracts in this tree are not physical acceptance evidence.

Run only phases listed in the reviewed session. Do not skip from preflight to a
later phase, reuse a session for a different target, or edit the journal to
bypass interrupted-state reconciliation.

Installation leaves release A inert. Smoke starts A, tests the call paths,
installs B, then verifies rollback to A. Reboot selects B and enables autostart
before its strict readiness gate and graceful reboot. Removal validates the
preflight restoration baseline before contacting or purging the target.

The reboot phase prints the old and new key fingerprints and requires interactive
confirmation before reconnecting. It then authenticates the device and verifies
that its boot identity changed. Soak phases require the private
media dialog and enforce resource-growth bounds; service idle time is not a
substitute.

An explicitly approved private bounded soak keeps an active media dialog for
15 minutes after smoke and samples once
per minute, keeps the fixed 10-minute warmup and the same resource envelope,
and uses the existing call-hangup and peer-process cleanup path. It requires
`soak-15` in the session's approved phase list and does not satisfy either
longer soak in the complete campaign.

## Evidence and interruption handling

Each phase writes create-new private evidence and a separately allowlisted
summary below the private session root. Summaries exclude addresses, MACs,
interfaces, user names, host keys, certificates, paths, raw protocol bodies,
and media. Review them against the
[`sanitization policy`](../../product/sipd/docs/governance/sanitization-policy.md)
before sharing.

One host process may run a phase for a given session at a time; a second process
fails before reconciliation or device contact. Journal filenames remain ordered
when the wall clock moves backward.
An unmatched `started` journal entry becomes `interrupted` on the next run.
Stop and reconcile the target state manually; never delete the journal or force
the next phase. Preserve unexpected target objects and verifier failures for a
private recovery review.

Removal validates the complete ownership journal before mutation, restores
only managed identity state, and deletes only project-owned objects. It
preserves the separately managed root-SSH substrate. Therefore a successful
project removal is not restoration of the device's original security posture.

## Acceptance boundary

Local tests and QEMU gates do not establish physical acceptance. A complete
private-lab campaign requires the ordered install and smoke checks, both soaks,
graceful reboot recovery, rollback/restart coverage, and verified final
project-state removal. Forced power-loss durability, trusted production TLS,
public Zoom interoperability, and provider-issued credentials remain separate
gates unless a future reviewed campaign explicitly proves them.
