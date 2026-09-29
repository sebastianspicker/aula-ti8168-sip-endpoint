# ARM toolchain, sysroot, and ABI workflow

The declared cross-build profile is ARMv7 Linux 2.6.37, ARM EABI5, glibc
2.12, with dynamic interpreter `/lib/ld-linux.so.3`. These are build
constraints, not a claim that an artifact runs on a physical board.

No upstream-only release sysroot or toolchain has been supplied and approved.
Public ARM binaries remain blocked by the
[public release policy](../../../../docs/PUBLIC_RELEASE.md). Previous
firmware-based build records and inputs belong to the private research corpus;
they do not establish release provenance.

## Reviewed inputs

Acquire or build the compiler, binutils, headers, loader, startup objects and
runtime libraries from identified upstream sources. Retain source versions,
hashes, build commands, patches, licenses, and the complete dependency closure.
Do not copy firmware runtime or startup objects into a release sysroot. Host
libraries and a successful cross build cannot substitute for that review.

`tools/construct-sysroot.py` consumes an explicit
`aula-sipd-sysroot-v1` manifest of already acquired regular files. It checks
SHA-256 values and produces a canonical sysroot lock. It neither downloads
inputs nor determines their redistribution rights. Inputs and outputs inside
the entire `evidence/` tree are rejected, including resolved aliases. Keep
sysroot and report output below `.work/`.

## Cross-build contract

`tools/build-arm.sh` requires these environment variables:

| Variable | Meaning |
| --- | --- |
| `AULA_CROSS_PREFIX` | Prefix of the ARM EABI5 toolchain, for example the prefix that supplies `gcc`, `ar`, and `ranlib`. |
| `AULA_SYSROOT` | Existing separately generated development sysroot. It must contain reviewed upstream inputs and must not be inside the evidence tree or an alias into it. |
| `AULA_ARM_BUILD_DIR` | Optional existing, non-symlink build directory outside evidence. Defaults to `.work/build/sipd-arm` below the repository root. |
| `AULA_SIPD_PJSIP_INCLUDE_DIR` | Reviewed pjproject include root inside the development sysroot. |
| `AULA_SIPD_PJSIP_LIBRARIES` | Semicolon-separated absolute paths to reviewed ARM pjproject/OpenSSL dependency libraries inside the development sysroot. Every entry must be a regular non-symlink file. |
| `AULA_SIPD_PJSIP_UA_LIBRARY` | Exact `libpjsip-ua` ARM archive inside the development sysroot, used for invite and session-timer usage. |
| `AULA_SIPD_PJMEDIA_LIBRARY` | Exact minimal `libpjmedia` ARM archive inside the development sysroot, used only for PJSIP invite SDP handling. |
| `AULA_SIPD_FAAD2_INCLUDE_DIR` | Reviewed FAAD2 2.11.2 include root inside the development sysroot. |
| `AULA_SIPD_FAAD2_LIBRARY` | Reviewed FAAD2 2.11.2 ARM library inside the development sysroot. |
| `AULA_SIPD_SPEEXDSP_INCLUDE_DIR` | Reviewed SpeexDSP 1.2.1 include root inside the development sysroot. |
| `AULA_SIPD_SPEEXDSP_LIBRARY` | Reviewed SpeexDSP 1.2.1 ARM library inside the development sysroot. |
| `AULA_SIPD_SRTP_INCLUDE_DIR` | Reviewed libsrtp 2.8 include root inside the development sysroot. |
| `AULA_SIPD_SRTP_LIBRARY` | Reviewed libsrtp 2.8 ARM library inside the development sysroot. |

The script sets `CC`, `AR`, `RANLIB`, and `SYSROOT`, then configures CMake with
`CMAKE_SYSTEM_NAME=Linux`, `CMAKE_SYSTEM_PROCESSOR=armv7`, the supplied
toolchain programs and sysroot, root-path-only library/include/package lookup,
and `AULA_SIPD_TARGET_PROFILE=aula-arm-eabi5`. It enables the exact external
PJSIP, FAAD2, SpeexDSP, and libsrtp production paths and refuses missing, symlinked,
host-side, or out-of-sysroot dependency inputs. It disables tests, fuzzing,
and sanitizers, then builds only `aula-sipd`. It must not fetch a toolchain,
discover host packages, or execute a target binary.

## Static inspection

`tools/check-arm-abi.sh` inspects a candidate without executing it. Supply an
explicit ABI reference built from reviewed upstream inputs for the declared
CPU and float ABI; the tool does not locate a reference automatically or
establish its provenance. Its default library allowlist and GLIBC ceiling
express the declared profile.

Run from `product/sipd` with reviewed inputs already present:

```sh
tools/check-arm-abi.sh \
  --binary ../../.work/build/sipd-arm/aula-sipd \
  --reference ../../.work/inputs/arm-abi-reference \
  --sysroot ../../.work/inputs/arm-sysroot
```

The check compares ELF32, little-endian ARM, EABI5, CPU and float-ABI
attributes, interpreter path, RPATH/RUNPATH absence, allowed `DT_NEEDED`
libraries, GLIBC symbol ceiling, and resolution of non-weak imports against
the supplied sysroot. The resolver library is part of that explicit sysroot.
These checks do not establish source provenance, syscall compatibility or
runtime acceptance.

`tools/audit-arm-interface.py` also requires an explicit JSON imported-symbol
allowlist. It applies the checked-in Linux `2.6.37` policy, hashes scanned C
source files, checks imports, and rejects ARM `svc` instructions and active
`syscall()` wrappers. Conditional source findings remain separate from built
imports. A register-held syscall number is not inferred from an SVC site.

`tools/generate-arm-report.py` combines the exact `check-arm-abi: OK` output
and interface audit JSON. The report still leaves confined execution, fixture
calls and hardware behavior unproven. Report writers protect all of
`evidence/`, without selecting an individual firmware extraction.

## Optional confined execution

`tools/run-qemu-arm.sh` requires `AULA_QEMU_ARM_ENABLE=1`, an explicit
sysroot and binary, a locally available container engine, and
`AULA_QEMU_ARM_IMAGE` with an immutable `@sha256:` image digest. The image
must already contain `qemu-arm`; the runner never pulls it. It uses no network,
a read-only container root and mounts, dropped capabilities, and an
unprivileged user. There is no automatic host-QEMU fallback.

Host verification does not exercise this optional lane. An ARM acceptance
claim needs reviewed inputs, static interface reports and separately
authorized runtime evidence. No private corpus or generated sysroot is a
product build dependency.
