# Third-party release gate

`dependency-lock.json` is the single package inventory for the prototype. It
records the frozen versions and minimal feature boundaries used by the current
build tooling. The listed official source archives were retrieved and hashed on
2026-08-26. The 2026-08-27 ARM closure strings record the bounded local
cross-build, ABI, and recovered-QEMU execution proof. They are not a release
attestation and do not replace live LS200, license, advisory, or Zoom CRC gates.

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
`prepare-nginx-ls200-cross.sh` applies a digest-gated patch that parses the
target tuple, compile-tests runtime probes, and supplies only facts recovered
for the LS200 target: ARM32 type sizes, little-endian byte order, Linux 2.6.37,
the glibc error-table bound, and an unavailable `accept4()`. Although the
cross sysroot headers expose `accept4()`, the recovered 2.6.37 kernel returns
`ENOSYS`; the target profile explicitly writes `NGX_HAVE_ACCEPT4` as `0` so
nginx uses `accept()` from startup. The console build script applies this patch
before configuring the minimal HTTPS/FastCGI module set.

Builds consume only explicitly supplied local inputs. They do not download,
select a host package silently, or treat a version string as integrity proof.
When reviewed archives are acquired, record their SHA-256 values here and emit
the package records into the deployment SBOM before enabling a release build.
