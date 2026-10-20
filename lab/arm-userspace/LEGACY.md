# Legacy ARM runtime compatibility

This optional development lane builds the current maintained C product with
public ARM EABI development inputs older than the board's glibc 2.12 ABI,
then executes the resulting ELF files against a separately supplied private
glibc 2.12.2 runtime. Firmware is never a compiler, header, startup-object or
linker input. This extends the modern [ARM userspace lane](README.md).

The public build inputs are frozen in `legacy-debian-lock.json`: Debian
Squeeze armel `libc6` and `libc6-dev` 2.11.3-4, plus `linux-libc-dev`
2.6.32-48squeeze6. The lock came from the archived Debian Release signature,
its SHA256 package-index entry, and each package's SHA256 entry. These are
historical compatibility inputs; the archived signing keys have expired and
the packages do not provide current security support or a release SDK.

## Public build inputs

In a disposable Linux preparation environment with public cross-GCC/binutils,
QEMU, CMake/CTest, make, Python, `gpgv`, `dpkg-deb` and Debian's packaged
archive keyrings already available:

```sh
python3 lab/arm-userspace/acquire_legacy_sysroot.py \
  --keyring /usr/share/keyrings/debian-archive-removed-keys.gpg \
  --output "$PWD/.work/legacy/public-001"
```

The tool downloads only public Debian metadata and packages. It checks the
frozen Release hash, validates both available historical signatures against
the explicit keyring and expected Squeeze signer fingerprints, verifies the
signed SHA256 package-index entry, and verifies each frozen package's size
and SHA256 before extracting it. No package installation or maintainer script
is run. Absolute package symlinks are made relative inside the generated
sysroot. The receipt retains signatures, metadata, packages, hashes and the
normalized sysroot manifest.

The historical key fingerprints are:

- `9FED2BCBDCD29CDF762678CBAED4B06F473041FA`
- `0E4EDE2C7F3E1FC0D033800E64481591B98321F9`

## Private execution oracle

Prepare a private runtime copy from the caller's independently retained
evidence, without modifying that evidence:

```sh
python3 lab/arm-userspace/prepare_legacy_runtime.py \
  --rootfs /private/evidence/rootfs \
  --output "$PWD/.work/legacy/private-runtime-001"
```

Only the named glibc loader/system libraries and `libgcc_s.so.1` are copied;
no firmware services, programs, configuration or credentials are included.
The helper checks the ELF32/little-endian marker and libc's glibc 2.12.2
release banner. Original symlinks and archived `.rclonelink` records are
accepted only when their targets equal the expected local library filename.
Files are copied read-only and hashed. This identifies the caller-selected
execution oracle; it does not establish manufacturer provenance or
redistribution rights. Keep the generated tree and its manifests private.

## Build and execute

Use a separate disposable execution container or VM with networking disabled,
a read-only root, dropped capabilities and no physical devices or private
session material. Mount the prepared private `runtime/` and public acquired
`sysroot/` read-only, mount the source read-only, and provide a dedicated
writable output directory below `.work`. Record the external isolation
configuration separately: the Python runner does not attest container flags.

Inside that environment:

```sh
python3 lab/arm-userspace/run_legacy.py \
  --cross-prefix /usr/bin/arm-linux-gnueabi- \
  --qemu-arm /usr/bin/qemu-arm \
  --keyring /usr/share/keyrings/debian-archive-removed-keys.gpg \
  --public-sysroot "$PWD/.work/legacy/public-001/sysroot" \
  --runtime "$PWD/.work/legacy/private-runtime-001/runtime" \
  --output "$PWD/.work/legacy/run-001"
```

The runner independently rechecks the cached Debian signature, package index
and pinned packages, reconstructs the public build sysroot from verified
package snapshots, and compares it with the supplied acquisition tree. A
locally editable JSON receipt cannot substitute for that verification.

Compiler wrappers select the old public startup objects with `-B`, the old
public libraries with explicit library paths, and only GCC's builtin headers
plus the old public headers with `-nostdinc`. This prevents a modern compiler
from silently choosing modern glibc headers or startup files. Selected
`crt*.o` and libc/library paths must resolve into the reconstructed public
sysroot. The private runtime is supplied only to QEMU and the static ABI gate.

The reused ARM runner builds and executes the seven maintained C test
programs, the configuration diagnostic CTest, and the actual daemon's
`--self-test` and `--fixture-call`. Every resulting ELF must also pass the
existing `check-arm-abi.sh` against the supplied private runtime and its
glibc 2.12 ceiling. Compiler, source, artifact, public-sysroot and runtime
hashes plus exact command logs are retained. Source/runtime changes and
missing/skipped tests fail the receipt.

`legacy-report.json` records the public acquisition and private preparation
receipts, verified input selections, tool identities, ELF ABI results and the
nested application execution report. The runner explicitly leaves isolation
attestation to the external container/VM launch record.

To additionally build the exact locked pjproject source and run a real
loopback UDP SIP dialog, supply `--pjsip-archive /inputs/pjproject.tar.gz`.
Run that mode as an ordinary dedicated UID so the normal LSZ1 peer-UID
authorization can operate without adding setuid capabilities. It also needs
the matching ARM G++, `patch` and `shasum`. TLS, AAC/AEC, SRTP, provider
interoperability and media I/O remain outside that optional signaling check.

## Meaning of a pass

A pass demonstrates execution of the current owned ARM application and its
tested interfaces with the supplied legacy runtime. It does not by itself
run Linux 2.6.37 or validate TI8168 peripherals, DSP, capture, physical output
or timing. Running the same owned ELF files and their minimal runtime closure
in the original-kernel full-system rescue lane adds kernel execution evidence;
it remains distinct from board-media or physical acceptance.
