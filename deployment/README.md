# LS-200 Zoom deployment kit

## Purpose

This is a maintained QEMU/deployment fixture. It never writes recovered NAND
inputs, downloads a dependency, or builds target code. It assembles
already-built binaries, configuration templates, service scripts, and UI
assets into a manifest-verified payload, then carries that payload through
two independent install/lifecycle implementations: a QEMU/bootstrap path for
the reversible overlay used by `lab/qemu`, and a physical-target path for one
owned LS-200 (`targets/ls200/`).

Run `make test-deployment` for payload, target, campaign, and disposable
runtime purge checks. The aggregate `make verify` runs these suites once
alongside the product and lab gates.

## Payload assembly

Build a payload only from reviewed prebuilt inputs:

```sh
make package-ls200 \
  SIPD_BINARY=/approved/ls200-sipd \
  GATEWAY_BINARY=/approved/ls200-gateway-fcgi \
  DEVICE_BINARY=/approved/ls200-device-control \
  NGINX_BINARY=/approved/ls200-nginx \
  ATOMIC_REPLACE_BINARY=/approved/ls200-atomic-replace \
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

`recovered-slot-profiles.conf` carries the two established launcher mappings:
slot 1 uses kernel MTD3 and root MTD4; slot 2 uses kernel MTD5 and root MTD6.

Both installer families share the packaged `nginx-records.sh` helper for
nginx selector/journal parsing and the audited `atomic-replace` `rename(2)`
helper. The activation helper requires source and destination symlinks in the
same absolute parent directory, rejects a non-symlink destination, uses
`rename(2)`, and syncs the parent directory. This avoids both BusyBox `mv`
following the old release link and platform-specific `mv -h` behavior.

The shipped runtime creates separate `ls200-web`, `ls200-gateway`, and
`ls200-sip` identities. Only `ls200-sip` joins the two media groups. It is a
dispatch contract, not embedded production web, gateway, or SIP software: it
accepts no command text from environment variables and executes only fixed,
version-owned component paths and fixed configuration paths.

Both installer families seed `ls200-sipd.conf` and `gateway.conf` from the
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

`make package-qemu` runs payload assembly and overlay staging together.
Equivalently, stage an already-built payload into a copy-on-write QEMU
overlay through the `LS200_ROOT_OVERLAY` route, passing the receipt value
independently:

```sh
sh deployment/payload/build-overlay.sh \
  "$PWD/.work/dist/ls200/runtime" \
  "$PWD/.work/dist/qemu/ls200-zoom-overlay" \
  "$(cat "$PWD/.work/dist/ls200/payload-manifest.sha256")"
```

The overlay carries the fixed installer-side verifier and digest receipt
outside the candidate runtime. Packaging, QEMU bootstrap, and installation use
that trusted verifier; they never execute the candidate verifier to establish
trust. `install.sh` (`deployment/payload/install.sh`) creates immutable
version directories below `/opt/ls200-zoom` and persists state and its
owned-file journal below `/var/lib/cbox/ls200-zoom`, then changes `current`
atomically. `rollback.sh VERSION` verifies and selects an installed release
atomically using the independent digest retained in the ownership journal;
legacy releases without a journaled digest fail closed. `remove.sh` removes
only the recorded init/nginx links and the owned prefix, retaining the state
directory for audit, and refuses to replace or remove unowned or retargeted
paths.

With a staged payload overlay, set `LS200_ZOOM_AUTOINSTALL=1` to invoke the
fixed QEMU installer (`deployment/targets/qemu/qemu-install.sh`) after the COW
root overlay is applied. The helper verifies an existing active release or
installs the fixed `qemu-prototype` version once through the shared
`install.sh`. Autoinstall alone does not create the vendor-media readiness
marker or start services. The separate explicit `LS200_ZOOM_QEMU_PROFILE=1`
profile installs a development certificate, fixture configuration, and fake
renderer readiness, then starts the maintained stack for
[`lab/qemu/tests/test_qemu_zoom_stack.py`](../lab/qemu/tests/test_qemu_zoom_stack.py).
This QEMU-only profile is rejected outside the isolated guest address and is
not renderer or live-hardware proof.

Web-mode QEMU keeps the recovered port-80 forward and also forwards guest
8443 to `LS200_CONSOLE_PORT` (default `8443`) on loopback. Choose a distinct
host port, for example `LS200_WEB_PORT=18080 LS200_CONSOLE_PORT=18443`, when a
local service already owns either default.

The same QEMU profile starts a deterministic host RTSP peer for the console's
local-source preview. The guest gateway connects only to its fixed private
numeric policy target, performs the exact video-only RTSP sequence, and
streams H.264 as authenticated HTTP-FLV without transcoding. Acceptance checks
the FLV header, AVC configuration record, keyframe, single-viewer reservation,
and API responsiveness. This proves the maintained parser/remux path only; it
does not prove live `/movie` coexistence or vendor rendering.

The two-slot stack test also reads live settings through LSZ1 opcode 9,
applies an idle revision-1 candidate that changes media policy from `managed`
to `disabled`, verifies revision 2, rejects a stale revision-1 write, and
proves the rejection did not change current state. It then restarts the ARM
SIP daemon, gateway, and maintained nginx instance, logs in again, and
verifies the exact changed state survived through the owner-only persistent
settings file.

## Physical target lifecycle

Physical-target deployment lives in
[`targets/ls200/`](targets/ls200/README.md). It runs only below the
persistent JFFS2 state root at `/var/lib/cbox/ls200-zoom`, exposed to running
services through the `/run/ls200-zoom-state` bind mount, and never writes
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
privileges drop. See [`targets/ls200/README.md`](targets/ls200/README.md) for
the full configuration, rollback, and removal contract.

## Why two lifecycle implementations

The QEMU/bootstrap path and the physical-target path are separate scripts,
not variants of one installer, because they answer to different install
roots, journals, and recovery models:

- **Install root.** The QEMU/bootstrap installer (`deployment/payload/*.sh`)
  owns `/opt/ls200-zoom` directly as both the immutable-release root and the
  active selector. The physical installer
  (`deployment/targets/ls200/install.sh`) explicitly rejects any payload that
  retains an `/opt` prefix; it installs below `/var/lib/cbox/ls200-zoom` and
  exposes the active release to running services only through the
  `/run/ls200-zoom-state` bind mount.
- **Journal.** The QEMU/bootstrap path uses the payload's own ownership
  journal for releases, the nginx selector, and copied files. The physical
  path shares that journal grammar through
  `transaction-records.sh`/`live-transaction-records.sh` but adds its own
  entry kinds for cron rows, the autostart marker, the root-SSH-adjacent
  service identities, and the private-lab/TLS-renewal transactions described
  above.
- **Recovery model.** QEMU/bootstrap recovery is `rollback.sh` and
  `remove.sh` acting directly on `/opt/ls200-zoom` and
  `/var/lib/cbox/ls200-zoom`, with no autostart, cron, or root-SSH substrate
  to reconcile. The physical path recovers through `live-rollback.sh` and
  `live-remove.sh`, both of which must additionally validate autostart state,
  the cron transaction, the root-SSH-adjacent identity overlay, and (for
  purge) the `purge-v1` recovery phase before mutating or deleting state.

Transfer assembly archives the canonical `ls200` target directory. The
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
`/zoom/#legacy`. The original AREC entry point and recovery access are
unchanged. The privileged
[device companion](../product/console/device/README.md) is a required
payload input (`DEVICE_BINARY`, builder `--device`) covered by manifests and
build receipts. It starts before the gateway as root, accepts only the
gateway UID, and stores protected state in the persistent `device/` directory.
Shutdown and health checks include it; removal retains its credentials and
job journal. Missing service state is reported as unavailable; the UI does
not assume that recording or streaming is stopped.

Do not switch the default device entry point or restrict OEM recovery access
until capability parity, the existing ARM ABI gate, and separately authorized
physical acceptance and rollback checks pass. Host UI and companion fixture
tests do not satisfy these gates. No build, packaging, or installation check
is waived by the new UI.
