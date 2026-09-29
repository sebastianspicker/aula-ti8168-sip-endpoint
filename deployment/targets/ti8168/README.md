# Physical target deployment profile

This profile installs the maintained payload on one owned target device. It is
separate from the QEMU installer and operates only below the persistent JFFS2
state root at `/var/lib/cbox/aula-ti8168-sip-endpoint`. It never writes recovered dumps, MTD
devices, firmware partitions, or the read-only UBIFS root.

Use the root [physical-lab workflow](../../../docs/operator/physical-private-lab.md)
for the gated end-to-end campaign. The scripts in this directory are low-level
target lifecycle components, not general discovery or release commands.

## Runtime and persistent state

The live payload is built for the fixed runtime selector
`/run/aula-state/current`. Persistent releases, configuration, ownership
journal, and recovery state remain under `/var/lib/cbox/aula-ti8168-sip-endpoint`; controlled
binds expose only the required state below `/run`.

The installer requires the separately managed root-SSH identity overlay and
its recovery contract. It adds dedicated non-login service identities through
that managed mechanism and maintains a separate owned group overlay. It never
rewrites the root password database, unrelated cron rows, or the vendor
firmware root. Project removal deliberately preserves the root-SSH substrate.
The former board-specific access and provisioning scripts are held only in the
private source snapshot. An operator must establish and review that prerequisite
through the private authorized workflow before using these deployment tools.
Install and removal retain recognition of the existing
`managed-by=open-ls200-root-shell.sh` marker for safe ownership checks and
cleanup; the marker is not a public provisioning command.

## Install and activation

The payload builder emits an independent `payload-manifest.sha256` receipt
beside the runtime directory. Packaging and gate checks retain that digest;
transfer requires `--manifest-sha256` and verifies the payload locally before
any contact. Keep this receipt with the reviewed build records.

Installation records `release-sha256:VERSION:DIGEST` in the root-owned journal
and installs the trusted verifier at `live-verify-payload.sh`, outside the
release. Start, rollback, removal service stops, and reboot checks use this
fixed verifier and the journaled digest before executing release code. A trusted
installer may atomically replace the fixed verifier only after its replacement
accepts every owned release against that release's independent journal digest. Legacy
releases without an independent digest cannot be activated or executed for
cleanup. Recover those releases through the authorized maintenance workflow
using an independently retained build receipt; never derive a new trust record
from an installed candidate manifest.

`install.sh` accepts a complete manifest-verified payload and creates an
immutable versioned release. It validates object type, mode, hash, ownership,
path ancestry, and the supplied manifest digest before executing payload code.
Activation swaps the `current` selector atomically through the payload's
audited `rename(2)` helper and verifies the active manifest afterward.

The installer ships with `bootstrap-transaction.sh`. It records the verified
release digest, SIP UID, and first-use token before publishing gateway
configuration and the matching token. A retry validates and completes the pair
and its ownership records before the same-version check. Changed published bytes
stop recovery; removal refuses pending transactions or orphaned bootstrap staging
files. Keep the helper alongside the installer when assembling a transfer.

Installation is inert by default. `--start` starts only after the fixed vendor
media-readiness helper and service health checks pass. Failure stops the new
release and restores the prior selector when one exists.

Run the non-mutating target-side validation shape from this directory:

```sh
sh install.sh --payload /path/to/payload --version VERSION \
  --manifest-sha256 LOWERCASE_SHA256_OF_PAYLOAD_MANIFEST --dry-run
```

The target verifier rejects special, writable, symlinked, or unlisted files.

## Start and autostart

Services start in privileged companion, daemon, gateway, nginx order and stop
in reverse order. The companion retains its protected credentials and jobs in
the persistent device state directory across replacement and rollback. The
fixed vendor readiness helper runs once for each start attempt. There is no
environment override for that production gate. A failed start attempts to stop
every managed component and release the runtime state mount. Stop likewise
attempts every cleanup step before reporting an aggregate failure.

Autostart requires both the exact owned cron rows and a root-owned enable
marker recorded in the ownership journal. Installing the rows alone cannot
start the product. `start.sh enable-autostart` publishes the marker only after
service health succeeds. Runtime readiness markers and the autostart sentinel
require safe root-owned parents and reject symlink leaves; absent files are
created exclusively.

Automatic startup retains the latest attempt's output in
`/run/aula-ti8168-sip-endpoint-autostart.log`, a root-only file truncated on each attempt.
The log is temporary and does not survive a reboot. Inspect it when cron
cannot bring up the console.

Cron and enable-marker changes use explicit prepared/applied transactions,
durable phase writes, and recovery on every lifecycle entry point. External
changes to the live crontab abort the update rather than being overwritten.

## Transfer boundary

On macOS, `transfer-run.sh` is dry-run by default. `--apply` requires an explicit
private target IPv4 address, interface, expected target MAC, approved SSH host-key
fingerprint, management port, payload path, version, and operator account. Keep
the real target address in private operator records outside maintained source.
The helper verifies only the named
target and performs no subnet discovery. Authentication remains interactive;
credentials are never accepted in arguments, stored, or logged.

```sh
sh transfer-run.sh --target PRIVATE_TARGET_IPV4 --payload /absolute/payload --version VERSION \
  --manifest-sha256 LOWERCASE_SHA256_FROM_INDEPENDENT_RECEIPT \
  --user OPERATOR --interface INTERFACE --expected-mac EXPECTED_MAC \
  --host-key APPROVED_SHA256_FINGERPRINT --port MANAGEMENT_PORT --apply
```

`--apply` installs but does not start the inert example configuration. Start
only through the reviewed campaign after private SIP, media, and TLS state has
been configured.

## Private-lab configuration

`configure-private-lab.sh` writes an isolated-lab profile from explicit numeric
peer and device values plus an optional approved TLS certificate/key pair. It
requires distinct canonical RFC1918 IPv4 addresses for the peer and device. It
uses the native RTSP backend and `plain_compat`; it never creates Zoom
credentials or accepts an HTTP-supplied SIP destination.

Keep all real addresses, identities, and certificate material in the private
session boundary. The command shape is:

```sh
sh configure-private-lab.sh --sip-peer-ip PEER_IPV4 \
  --sip-peer-port PEER_PORT --device-ip DEVICE_IPV4 \
  --tls-cert /absolute/private/server.crt \
  --tls-key /absolute/private/server.key
```

Configuration and TLS publication is transaction-journaled and hash-checked.
Unexpected staged or final bytes stop recovery for manual review.

Private-lab configuration does not enable Zoom direct calls. A separately
authorized Zoom deployment needs the static SIP TLS/public-network baseline and
required SRTP described in the
[configuration reference](../../../product/sipd/docs/operator/configuration-reference.md).
Changing only the console profile cannot supply that baseline. After a static
configuration change and service restart, use the console settings to select
Zoom direct: persisted settings take precedence over the file's profile.

For an invitation-supplied regional CRC address, set optional `[sip] crc_address`
in the owned SIP configuration. Empty or omitted uses normal `zoomcrc.com` DNS.
The regional value is one public IPv4 address; TLS still verifies `zoomcrc.com`.
Validate the staged configuration as the SIP service user, stop services, publish
the configuration and its matching ownership-journal digest, and restart. Retain
the prior configuration for recovery. Remove the key before rollback to a release
that does not recognize it. This setting is independent of the console profile.

An operator-provisioned SIP CA bundle may be stored at
`/var/lib/cbox/aula-ti8168-sip-endpoint/tls/sip-ca.crt`, owned by the SIP service UID (1000),
group 0, mode 0600. Configure `tls_ca_file` with its service-visible path,
`/run/aula-state/tls/sip-ca.crt`; the vendor `/var` hierarchy is not
traversable by the service. Preserve the ownership journal's paired `file:` and
`sha256:` entries for this optional file. Removal validates its ownership and
digest before purge. Validate configuration as the SIP service identity through
the runtime mount before activation. The SIP CA bundle is independent of the
console's HTTPS server certificate and key.

Before dropping privileges, service startup reads the current DHCP resolver
file and passes up to three IPv4 nameservers through `AULA_SIPD_DNS_SERVERS`
to the SIP daemon only. The daemon configures the system resolver with those
addresses and retains its normal peer authorization and TLS verification.
This avoids granting access through the vendor's root-only directories. A
service restart refreshes the nameservers after a DHCP DNS configuration change.

To replace an expired managed TLS pair without changing the SIP configuration,
runtime settings, or console accounts, first disable autostart and stop all
managed services through the installed trusted lifecycle helper. Then supply a
new root-owned pair from restrictive absolute paths:

```sh
sh /var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh disable-autostart
sh /var/lib/cbox/aula-ti8168-sip-endpoint/live-start.sh stop
sh configure-private-lab.sh --replace-tls \
  --tls-cert /absolute/private/replacement.crt \
  --tls-key /absolute/private/replacement.key
```

TLS replacement takes the shared live-operation lock, verifies the stopped
PID, executable, socket, autostart, file-owner, mode, journal, and old-hash
contracts, then durably stages both the replacement pair and recovery copies of
the old pair. Its transaction accepts only the recorded old and new hashes at
each publication cut point. Unexpected bytes stop recovery without overwriting
them. The command leaves services stopped and autostart disabled; the operator
must use `live-start.sh start`, verify `live-start.sh health`, and enable
autostart separately when authorized. An interrupted replacement retains its
transaction and blocks removal until this command validates and completes the
recorded state. Recovery paths are temporary transaction evidence and never
become persistent ownership-journal entries.

## Rollback and removal

Release arguments accept letters, digits, dots, underscores, and hyphens, but
reject `.` and `..`. Installer activation verifies the prior release before
attempting startup. If activation fails, automatic recovery restores its
selector using that prior release's verified atomic helper and leaves services
stopped. The error distinguishes restored selection, failed recovery, and
deactivation of a failed first installation. Inspect the selector and recovery
output before using the normal start and health commands; an installer failure
alone does not establish which release is selected or running.

The installed rollback helper takes the shared operation lock, records old and
new selectors in a transaction, swaps atomically, and declares completion only
after health succeeds:

```sh
sh /var/lib/cbox/aula-ti8168-sip-endpoint/live-rollback.sh --to VERSION
```

`live-remove.sh --apply` removes verified releases and managed runtime binds but
retains the audit journal and product SIP/TLS state. `--purge-owned-state`
attempts final removal only after validating the complete ownership journal,
configuration/TLS records, transactions, mount ancestry, and target objects:

```sh
sh /var/lib/cbox/aula-ti8168-sip-endpoint/live-remove.sh --purge-owned-state
```

Missing, duplicate, malformed, unowned, changed, or unexpected state fails
closed. Do not delete preserved paths manually. Neither removal mode removes
the separately owned root-SSH substrate, so a successful project purge is not
restoration of the original device security posture.

Repository tests exercise these lifecycle contracts without contacting
hardware. Physical acceptance requires the separately authorized campaign.

The physical start profile selects `/usr/bin/ffmpeg` through
`AULA_SIPD_RECEIVE_MONITOR`. It verifies that this firmware executable is a
root-owned, non-writable regular executable before starting services. The SIP
daemon opens and verifies the executable again when spawning bounded receive
workers under its unprivileged service identity. These workers process incoming
H.264 and audio for a send/receive call; they do not provide HDMI or speaker
playback. A missing or unsafe decoder fails the physical start gate.
