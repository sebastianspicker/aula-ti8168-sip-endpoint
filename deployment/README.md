# Aula deployment kit

## Purpose

This deployment kit never writes recovered NAND inputs, downloads a
dependency, or builds target code. It assembles already-built binaries,
configuration templates, service scripts, and UI assets into a
manifest-verified payload for one owned physical target (`targets/ti8168/`).
The old QEMU/bootstrap overlay is not a maintained acceptance path for the
synthetic model.

These payloads are laboratory artifacts. Manifest verification establishes
integrity, not redistribution rights. `package-qemu` is unavailable for the
synthetic model. Review provenance and licenses before distributing a payload.
Existing installations require the stopped-service procedure in the
[Aula upgrade guide](../docs/operator/upgrade-to-aula.md).

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

The physical installer uses the audited `atomic-replace` `rename(2)` helper.
The activation helper requires source and destination symlinks in the
same absolute parent directory, rejects a non-symlink destination, uses
`rename(2)`, and syncs the parent directory. This avoids both BusyBox `mv`
following the old release link and platform-specific `mv -h` behavior.

The shipped runtime creates separate `aula-web`, `aula-gateway`, and
`aula-sip` identities. Only `aula-sip` joins the two media groups. It is a
dispatch contract, not embedded production web, gateway, or SIP software: it
accepts no command text from environment variables and executes only fixed,
version-owned component paths and fixed configuration paths.

The installer seeds `aula-sipd.conf` and `gateway.conf` from the
verified release examples only when their state files are absent; later
releases never overwrite operator configuration. On a live root, installation
resolves the dynamically allocated SIP and gateway UIDs into those first-use
files, enables the local LSZ1 socket, and creates a random root-readable
bootstrap token. TLS key/certificate material, ARM toolchain/sysroot
evidence, and third-party ARM closures remain external release gates.

The installer uses the packaged `bootstrap-transaction.sh` helper to recover
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
launcher and product acceptance workflow are not included. `make package-qemu`
and `make qemu-acceptance` fail closed; the
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

The physical profile owns configuration and recovery transactions:
`private-lab-v1`
publishes initial SIP/TLS state, a separate `private-lab-tls-renew-v1`
transaction replaces TLS material only while services are stopped, and DHCP
nameserver handling passes a bounded IPv4 list to the SIP process before
privileges drop. See [`targets/ti8168/README.md`](targets/ti8168/README.md) for
the full configuration, rollback, and removal contract.

Transfer assembly archives the canonical `ti8168` target directory. The
physical profile installs `live-transaction-records.sh` before its entrypoints
and records it in the
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

The UI and companion do not establish physical acceptance. The existing ARM
ABI, payload trust, and separately authorized physical acceptance
and rollback gates remain in force.
