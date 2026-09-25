# ARM toolchain, sysroot, and ABI workflow

Status: local cross-build, static ABI inspection, payload installation, and
recovered-QEMU execution verified on 2026-08-27. This is development evidence,
not a release toolchain/sysroot attestation or live-hardware acceptance. The
target profile is ARMv7 Linux 2.6.37, ARM EABI5, glibc 2.12, with dynamic
interpreter `/lib/ld-linux.so.3`.

## 2026-08-27 local asset inventory

The recovered runtime oracle is ELF32 little-endian ARM EABI5, ARMv7-A with
Thumb-2, VFPv3, and NEON, using `/lib/ld-linux.so.3` and glibc 2.12.2. It has
runtime shared objects but no headers, crt/start files, static libc, target
compiler/binutils, or target PJSIP/FAAD2/SpeexDSP/libsrtp libraries. Every located
dependency build product is Mach-O arm64 host code.

A temporary reviewed development environment combined Clang 22.1.8 ARM EABI
code generation, GNU ARM binutils, Debian armel development inputs, and copied
recovered runtime/startup objects in a separate sysroot below `/private/tmp`.
The recovered evidence tree remained read-only. Exact pjproject, nginx,
OpenSSL, FastCGI, Jansson, FAAD2, SpeexDSP, and libsrtp sources built there.
The four payload executables passed `check-arm-abi.sh`: ELF32 little-endian ARM
EABI5, `/lib/ld-linux.so.3`, no RPATH/RUNPATH, allowed target libraries, and a
GLIBC import ceiling no newer than the recovered runtime. A release still
needs durable toolchain/sysroot provenance and the reviewed imported-symbol
allowlist/interface report; the temporary environment is not that artifact.

## Containment and inputs

The extracted LS-200 rootfs is immutable runtime evidence, not a development
SDK. Construct a separate, reproducible development sysroot outside that tree.
It must document provenance and hashes for the target loader/libraries,
compatible userspace headers, any generated pkg-config metadata, and, only if
approved for direct linkage, matching GStreamer 1.0.6 headers. Do not copy
files into the evidence rootfs or use modern host headers as ABI proof.

Before a release cross build, choose a pinned ARM EABI5 cross-toolchain outside
the evidence tree and retain its version, acquisition/build record, license,
and hash. The local minimal-feature ARM build now exists, but distribution
license, durable provenance, live interoperability, and unresolved
advisory-feature exclusions remain input gates.

## Cross-build contract

`tools/build-arm.sh` requires these environment variables:

| Variable | Meaning |
| --- | --- |
| `LS200_CROSS_PREFIX` | Prefix of the ARM EABI5 toolchain, for example the prefix that supplies `gcc`, `ar`, and `ranlib`. |
| `LS200_SYSROOT` | Existing separately generated development sysroot. It must not be the extracted rootfs or an alias inside it. |
| `LS200_ARM_BUILD_DIR` | Optional existing, non-symlink build directory outside evidence. Defaults to `.work/build/sipd-arm` below the repository root. |
| `LS200_SIPD_PJSIP_INCLUDE_DIR` | Reviewed pjproject include root inside the development sysroot. |
| `LS200_SIPD_PJSIP_LIBRARIES` | Semicolon-separated absolute paths to reviewed ARM pjproject/OpenSSL dependency libraries inside the development sysroot. Every entry must be a regular non-symlink file. |
| `LS200_SIPD_PJSIP_UA_LIBRARY` | Exact `libpjsip-ua` ARM archive inside the development sysroot, used for invite and session-timer usage. |
| `LS200_SIPD_PJMEDIA_LIBRARY` | Exact minimal `libpjmedia` ARM archive inside the development sysroot, used only for PJSIP invite SDP handling. |
| `LS200_SIPD_FAAD2_INCLUDE_DIR` | Reviewed FAAD2 2.11.2 include root inside the development sysroot. |
| `LS200_SIPD_FAAD2_LIBRARY` | Reviewed FAAD2 2.11.2 ARM library inside the development sysroot. |
| `LS200_SIPD_SPEEXDSP_INCLUDE_DIR` | Reviewed SpeexDSP 1.2.1 include root inside the development sysroot. |
| `LS200_SIPD_SPEEXDSP_LIBRARY` | Reviewed SpeexDSP 1.2.1 ARM library inside the development sysroot. |
| `LS200_SIPD_SRTP_INCLUDE_DIR` | Reviewed libsrtp 2.8 include root inside the development sysroot. |
| `LS200_SIPD_SRTP_LIBRARY` | Reviewed libsrtp 2.8 ARM library inside the development sysroot. |

The script sets `CC`, `AR`, `RANLIB`, and `SYSROOT`, then configures CMake with
`CMAKE_SYSTEM_NAME=Linux`, `CMAKE_SYSTEM_PROCESSOR=armv7`, the supplied
toolchain programs and sysroot, root-path-only library/include/package lookup,
and `LS200_SIPD_TARGET_PROFILE=ls200-arm-eabi5`. It enables the exact external
PJSIP, FAAD2, SpeexDSP, and libsrtp production paths and refuses missing, symlinked,
host-side, or out-of-sysroot dependency inputs. It disables tests, fuzzing,
and sanitizers, then builds only `ls200-sipd`. It must not fetch a toolchain,
discover host packages, or execute a target binary.

## Remaining static ARM gates

`tools/construct-sysroot.py` builds a new development sysroot from a reviewed
`ls200-sipd-sysroot-v1` manifest. Every input must be an already acquired,
regular, non-symlink file whose SHA-256 matches the manifest. It writes a
canonical sysroot lock alongside the resulting files and refuses every output
inside recovered firmware evidence.

After an approved build, use `tools/audit-arm-interface.py` with an explicit
JSON imported-symbol allowlist. It applies the checked-in Linux `2.6.37`
policy, records the hashes of every scanned C source file, rejects selected
post-2.6.37 interfaces and newer GLIBC imports in the built candidate, and
fails on any ARM `svc` instruction or active `syscall()` wrapper. It records
conditional source findings separately so they can be correlated with the
built imports instead of being mistaken for active code. This is intentionally
conservative: a register-held syscall number cannot be inferred safely from an
SVC site. The report remains static evidence, not target-runtime or
undocumented-driver-ioctl proof.

`tools/run-qemu-arm.sh` is an opt-in user-mode runner. It requires
`LS200_QEMU_ARM_ENABLE=1`, a separate sysroot and binary, a locally available
`docker` or `podman` engine, and `LS200_QEMU_ARM_IMAGE` set to a reviewed image
reference ending in an immutable `@sha256:<64-lowercase-hex>` digest. It runs
with no network, a read-only container root, dropped capabilities, an
unprivileged user, a read-only sysroot mount, and a read-only candidate mount.
It deliberately has no host-QEMU or namespace-only fallback. The image must
already contain `qemu-arm`; this runner never pulls it.

`tools/generate-arm-report.py` consumes the exact plain-text success
line (`check-arm-abi: OK`) emitted by `tools/check-arm-abi.sh` plus the JSON
interface audit. Its `pass` status still leaves confined execution, fixture
call, and hardware behavior unproven.

## ABI inspection contract

After a build exists, `tools/check-arm-abi.sh` inspects a binary without
executing it. It requires `--binary`, a recovered `--reference`, and
`--sysroot`; its optional defaults allow only the target runtime libraries and
limit GLIBC to `2.12`.

The intended command shape is:

```sh
tools/check-arm-abi.sh \
  --binary /approved/build/ls200-sipd \
  --reference /approved/recovered/arm-reference \
  --sysroot /approved/sysroot
```

The interface-audit command shape is:

```sh
tools/audit-arm-interface.py \
  --binary /approved/build/ls200-sipd \
  --allowlist /approved/reviewed-imports.json \
  --source-root src \
  --output /approved/reports/arm-interface.json
```

The gate checks ELF32, little-endian ARM, EABI version 5, CPU and float-ABI
attributes against the recovered reference, interpreter path, absence of
RPATH/RUNPATH, allowed `DT_NEEDED` libraries, GLIBC symbol ceiling, and
resolution of non-weak dynamic imports in the supplied sysroot. It does not by
itself prove old-kernel syscall compatibility, target startup, a functional
daemon, or hardware-media behavior. The final development payload separately
executed in both recovered full-system QEMU slots; that is guest startup and
fixture evidence, not physical-hardware behavior.
The daemon's DHCP DNS initialization also links the firmware's
`libresolv.so.2`, supplied by the reviewed glibc sysroot. It remains subject to
the same dynamic-import and GLIBC-version checks; no replacement resolver
library is deployed.

## Required evidence before an ARM claim

The four final development executables pass the recovered-reference ELF,
loader, GLIBC, RPATH, dependency, and import-resolution gate, and their
manifest payload boots in both recovered full-system QEMU slots. The exact
sipd build excludes its test-only persistence fault seam. This supports an ARM
EABI5 QEMU-prototype claim only.

A release ARM-runtime-compatible claim still requires durable toolchain and
sysroot provenance, the reviewed machine-readable dependency and imported-
interface reports, Linux 2.6.37 syscall/interface closure, and live target
startup/media/interoperability evidence. Do not describe the current temporary
cross environment as reproducible release provenance or hardware acceptance.
