# Aula deployment kit

## Purpose

This deployment kit never writes recovered NAND inputs, downloads a
dependency, or builds target code. It assembles already-built binaries,
configuration templates, service scripts, and UI assets into a
manifest-verified payload for one owned physical target (`targets/ti8168/`).
The old QEMU/bootstrap overlay is not a maintained acceptance path for the
synthetic model.

These payloads are private lab artifacts. Manifest verification establishes
integrity, not redistribution rights. Public binary releases remain blocked;
neither `package-ti8168` nor `live-package` is a public release command.
`package-qemu` is unavailable for the synthetic model. The separate
[public release policy](../docs/PUBLIC_RELEASE.md)
requires reviewed provenance and licenses before any publication.
Existing installations require the stopped-service procedure in the
[Aula upgrade guide](../docs/operator/upgrade-to-aula.md).

Run `make test-deployment` for payload, target, campaign, and disposable
runtime purge checks. The aggregate `make verify` runs these suites once
alongside the product and lab gates.

## Payload assembly

Build a payload only from reviewed prebuilt inputs:

```sh
make package-ti8168 \
  SIPD_BINARY=/approved/aula-sipd \
  GATEWAY_BINARY=/approved/aula-gateway-fcgi \
  DEVICE_BINARY=/approved/aula-device-control \
  NGINX_BINARY=/approved/aula-nginx \
  ATOMIC_REPLACE_BINARY=/approved/aula-atomic-replace \
  MIME_TYPES=/approved/nginx-mime.types \
  FASTCGI_PARAMS=/approved/nginx-fastcgi_params
```

`build-payload.py` rejects missing, non-regular, non-executable, or symlinked
inputs. It copies the four services plus the small audited `rename(2)`
activation helper, UI assets below `ui/zoom` so the nginx `/zoom/` root
mapping resolves correctly, nginx configuration and `mime.types`, both
configuration examples, and maintained service files. It rewrites only the
deployment anchors needed to point the nginx static root, FastCGI executable,
TLS directory, and gateway account store at the versioned release and
persistent state. It then writes a hash-and-mode manifest plus the manifest's
SHA-256 in the adjacent `payload-manifest.sha256` receipt. All build outputs
must resolve strictly below the repository `.work` directory, while reviewed
inputs may live outside the repository and never require recovered firmware.
Keep the receipt with the reviewed build records, separate from the candidate
runtime tree.

The privileged device companion (`DEVICE_BINARY`, builder `--device`) is a
required payload input covered by manifests and build receipts; see the
[unified console rollout boundary](#unified-console-rollout-boundary) below.

Both installer families share the packaged `nginx-records.sh` helper for
nginx selector/journal parsing and the audited `atomic-replace` `rename(2)`
helper. The activation helper requires source and destination symlinks in the
same absolute parent directory, rejects a non-symlink destination, uses
`rename(2)`, and syncs the parent directory. This avoids both BusyBox `mv`
following the old release link and platform-specific `mv -h` behavior.

The shipped runtime creates separate `aula-web`, `aula-gateway`, and
`aula-sip` identities. Only `aula-sip` joins the two media groups. It is a
dispatch contract, not embedded production web, gateway, or SIP software: it
accepts no command text from environment variables and executes only fixed,
version-owned component paths and fixed configuration paths.

Both installer families seed `aula-sipd.conf` and `gateway.conf` from the
verified release examples only when their state files are absent; later
releases never overwrite operator configuration. On a live root, installation
resolves the dynamically allocated SIP and gateway UIDs into those first-use
files, enables the local LSZ1 socket, and creates a random root-readable
bootstrap token. TLS key/certificate material, ARM toolchain/sysroot
evidence, and third-party ARM closures remain external release gates.

Both installers use the packaged `bootstrap-transaction.sh` helper to recover
first-use gateway configuration and its matching bootstrap token. A pending
transaction pins the release digest, SIP UID, and token. Rerunning the
installer reconciles the pair before checking whether that version is already
installed; changed published bytes stop recovery. Removal refuses pending
transactions or orphaned bootstrap staging files.

Packaging also compares every copied binary and UI asset with the build
receipt before publishing a package receipt, and build completion revalidates
reviewed inputs and the maintained-source SHA-256 captured before building.
Packaging and local gates reject a missing or changed source digest.

## QEMU/bootstrap lifecycle

The maintained QEMU model is synthetic and has no recovered guest boot,
payload overlay, or two-slot product acceptance path. The former QEMU guest
launcher and its acceptance test are preserved only in the private source
snapshot. `make package-qemu` and `make qemu-acceptance` fail closed; the
physical deployment path below remains subject to its own authorization and
independent payload trust checks. Legacy overlay scripts are not
validation evidence for the current synthetic machine model.

## Physical target lifecycle

Physical-target deployment lives in
[`targets/ti8168/`](targets/ti8168/README.md). It runs only below the
persistent JFFS2 state root at `/var/lib/cbox/aula-ti8168-sip-endpoint`, exposed to running
services through the `/run/aula-state` bind mount, and never writes
recovered dumps, MTD devices, firmware partitions, or the read-only UBIFS
root. It shares one fixed ownership-journal grammar (`transaction-records.sh`,
installed as `live-transaction-records.sh`) between normal and recovery
passes, validates mutable runtime paths before recording a prepared purge, and
repeats that validation before deleting state. Empty allowlisted nginx
temporary directories are accepted; unexpected files or nonempty temporary
directories stop the purge. The existing journal and `purge-v1` recovery
phases remain the persistent contract.

Physical release verification uses an installer-owned verifier
(`live-verify-payload.sh`) outside release directories and one independently
supplied manifest digest per journaled release. Packaging consumes the
builder receipt; activation, rollback, cleanup service stops, and reboot
validation consume the journal record. Legacy digestless releases fail closed
until recovered through the authorized maintenance workflow with an
independent build receipt. Candidate code never establishes its own
verification authority.

Failed installer activation can restore the verified prior selector while
leaving services stopped. Its recovery result is reported separately from
normal startup and health verification; a failed first installation instead
deactivates its selector.

The physical profile additionally owns configuration and recovery
transactions that the QEMU/bootstrap path does not need: `private-lab-v1`
publishes initial SIP/TLS state, a separate `private-lab-tls-renew-v1`
transaction replaces TLS material only while services are stopped, and DHCP
nameserver handling passes a bounded IPv4 list to the SIP process before
privileges drop. See [`targets/ti8168/README.md`](targets/ti8168/README.md) for
the full configuration, rollback, and removal contract.

## Why two lifecycle implementations

The QEMU/bootstrap path and the physical-target path are separate scripts,
not variants of one installer, because they answer to different install
roots, journals, and recovery models:

- **Install root.** The QEMU/bootstrap installer (`deployment/payload/*.sh`)
  owns `/opt/aula-ti8168-sip-endpoint` directly as both the immutable-release root and the
  active selector. The physical installer
  (`deployment/targets/ti8168/install.sh`) explicitly rejects any payload that
  retains an `/opt` prefix; it installs below `/var/lib/cbox/aula-ti8168-sip-endpoint` and
  exposes the active release to running services only through the
  `/run/aula-state` bind mount.
- **Journal.** The QEMU/bootstrap path uses the payload's own ownership
  journal for releases, the nginx selector, and copied files. The physical
  path shares that journal grammar through
  `transaction-records.sh`/`live-transaction-records.sh` but adds its own
  entry kinds for cron rows, the autostart marker, the root-SSH-adjacent
  service identities, and the private-lab/TLS-renewal transactions described
  above.
- **Recovery model.** QEMU/bootstrap recovery is `rollback.sh` and
  `remove.sh` acting directly on `/opt/aula-ti8168-sip-endpoint` and
  `/var/lib/cbox/aula-ti8168-sip-endpoint`, with no autostart, cron, or root-SSH substrate
  to reconcile. The physical path recovers through `live-rollback.sh` and
  `live-remove.sh`, both of which must additionally validate autostart state,
  the cron transaction, the root-SSH-adjacent identity overlay, and (for
  purge) the `purge-v1` recovery phase before mutating or deleting state.

Transfer assembly archives the canonical `ti8168` target directory. The
bootstrap installer and remover load the owner-controlled sibling
`nginx-records.sh`; distribute it with the entrypoints. Overlay assembly
stages it automatically. The physical profile similarly installs
`live-transaction-records.sh` before its entrypoints and records it in the
ownership journal. Start and rollback validate its ownership and fixed parent
chain before loading it. Removal retains it for retries and removes it during
final purge cleanup. Record parsing is shared; locking, path checks, service
recovery, and the distinct unjournaled-marker policies remain in their owning
entrypoints.

## Unified console rollout boundary

The room-first UI retains `/zoom/` and offers the previous Zoom screens at
`/zoom/#legacy`. Provisioning and any vendor recovery access are outside this
maintained payload and require separately reviewed private inputs. The privileged
[device companion](../product/console/device/README.md) is a required
payload input (`DEVICE_BINARY`, builder `--device`) covered by manifests and
build receipts. It starts before the gateway as root, accepts only the
gateway UID, and stores protected state in the persistent `device/` directory.
Shutdown and health checks include it; removal retains its credentials and
job journal. Missing service state is reported as unavailable; the UI does
not assume that recording or streaming is stopped.

The UI and companion fixture tests do not establish physical acceptance. The
existing ARM ABI, payload trust, and separately authorized physical acceptance
and rollback gates remain in force.
