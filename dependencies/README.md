# Third-party release gate

`dependency-lock.json` is the single package inventory for the prototype. It
records the frozen versions and minimal feature boundaries used by the current
build tooling. The listed official source archives were retrieved and hashed on
2026-08-26. The 2026-08-27 review date belongs to that earlier dependency
inventory; its ARM cross-build and recovered-QEMU observations concerned the
previous product build. Every `arm_closure` entry now marks the current Aula ARM
build unverified: no approved replacement sysroot or complete cross-build, ABI,
and QEMU proof has been established. The archive hashes and host results remain
useful, but they do not attest to an Aula release or replace live target-device,
license, advisory, or Zoom CRC gates.

`advisories/pjproject-advisory-snapshot.json` is the dated, feature-scoped PJSIP review.
After acquiring the exact pinned Git history, verify every recorded fix
ancestor and the owned adapter's no-fix feature exclusions with:

```sh
python3 dependencies/scripts/verify-pjproject-advisories.py --checkout /path/to/pjproject
```

The check follows the native adapter's local include closure. Only the SDP
syntax parser and session type are allowed from PJMEDIA; media transport and
negotiation remain excluded.

This offline check does not replace a fresh upstream advisory review before a
release.

The pinned source currently adds a C++ atomic queue object to the otherwise C
`pjlib` archive. Its only consumers are the disabled Android MediaCodec and
Oboe media implementations. Before configuring the ARM signaling-only build,
apply the reviewed, digest-gated patch:

```sh
dependencies/scripts/prepare-pjproject-signaling.sh /path/to/pjproject
```

The preparer accepts only the exact pinned `pjlib/build/Makefile`, is
idempotent, and verifies the patched result. This keeps PJSIP types and media
outside the owned adapter boundary and avoids adding a C++ runtime to the
device payload.

Nginx 1.31.4's upstream `--crossbuild` mode still executes generated target
programs and reports the whole platform tuple as the operating-system name.
`prepare-nginx-aula-cross.sh` applies a digest-gated target profile patch to
nginx 1.31.4. The profile encodes explicit ARM32 sizes, little-endian byte
order, a Linux 2.6.37 target tuple, a glibc error-table bound, and a
conservative `accept()` path. These are build inputs requiring independent
target validation; a version string or recovered artifact alone does not prove
runtime ABI or kernel support. The patch disables `accept4()` for this profile
until that support is verified on the target.
The console build script applies this patch
before configuring the minimal HTTPS/FastCGI module set.

Builds consume only explicitly supplied local inputs. They do not download,
select a host package silently, or treat a version string as integrity proof.
When reviewed archives are acquired, record their SHA-256 values here and emit
the package records into the deployment SBOM before enabling a release build.
