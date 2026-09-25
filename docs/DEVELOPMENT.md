# Development and verification

## Generated-state policy

Run commands from the repository root unless noted. Build trees, virtual
environments, dependency stores, generated reports, package output, and live
session state belong below `.work/`. Do not create component-local `.venv`,
`.work`, `build`, `dist`, `node_modules`, or report directories.

The root Makefile sets the relevant uv cache and environment paths. The console
dependency wrapper temporarily links `product/console/web/node_modules` to the
cache below `.work` and removes the link after the last concurrent user exits.
Shared process leases cover each command and its supervised children. Installation
and cleanup require exclusive access; neither can interrupt active dependency
users. Unchanged manifests and a valid installation return immediately.
The canonical `.work` directory and console cache and lock parents must be owned
real directories that are not writable by group or others; symlink aliases are
rejected before dependency operations.

Root Make gates run uv offline and require an already prepared console cache.
Prepare the locked Python environments and run `make -C product/console ui-install`
as a separate dependency setup step before verification on a fresh checkout.
Missing caches fail the gate without acquiring dependencies.

## Toolchain

| Area | Required tools or inputs |
| --- | --- |
| Repository and emulator | Python 3.11+, `uv`, POSIX shell, `make`, `shellcheck` |
| SIP daemon | C compiler, CMake 3.16+, CTest, `cmake-format`; optional Clang/libFuzzer for fuzzing |
| Console gateway | C compiler, `pkg-config`, Jansson and OpenSSL development files |
| Console UI | `pnpm`; dependencies are locked by `product/console/web/pnpm-lock.yaml` |
| QEMU | Git, Make, Ninja, a native compiler, and network access only when the pinned source is absent |
| Packaging | Reviewed prebuilt ARM binaries and nginx support files supplied explicitly |
| Physical lab | Private session file, reviewed build manifest, entropy helper, recovery authority, and action confirmations |

Optional PJSIP, libsrtp, FAAD2, SpeexDSP, FastCGI, and nginx inputs are never
downloaded or discovered automatically. Use the versions and integrity records
under [`dependencies/`](../dependencies/README.md) and each component's
third-party policy.

## Repository gates

| Command | What it runs |
| --- | --- |
| `make verify` | Layout, quality, product, lab, and deployment gates |
| `make check-layout` | Canonical roots, generated/private paths, and symlink policy |
| `make quality` | Locked Lizard and exact-clone checks over the maintained-source manifest |
| `make test-product` | SIP daemon verifier, gateway tests, preview tests, UI tests, and UI build |
| `make test-lab` | Emulator verification, QEMU/live pytest suites, QEMU script checks, and deployment contracts |
| `make test-deployment` | Payload, live-target, QEMU-target, and campaign contract suites |
| `make verify-evidence` | Evidence-corpus quality and reproducibility tests, when `evidence/` is present |

`make verify` shares the quality and deployment prerequisites across components,
so each runs once, including under `make -j verify`. Standalone component gates
retain their complete checks. `make quality` also verifies manifest completeness,
layout rules, dependency leases, and compiler dependency rebuilding.

There is no repository-wide Markdown, C, Python, or TypeScript formatter target.
Do not substitute an inferred formatter for a configured gate.

## SIP daemon

The root product gate is the authoritative comprehensive path:

```sh
make test-product
```

For a focused host build from the root:

```sh
cmake -S product/sipd -B .work/build/sipd \
  -DLS200_SIPD_BUILD_TESTS=ON
cmake --build .work/build/sipd
ctest --test-dir .work/build/sipd --output-on-failure
```

The comprehensive SIP-only verifier runs quality, CMake formatting, unit and
integration tests, coverage, sanitizers, and a bounded fuzz smoke test:

```sh
mkdir -p .work/build .work/cache .work/dist .work/reports
LS200_SIPD_VERIFY_ENABLE=1 \
  sh product/sipd/tools/verify-repository.sh --run
```

Verification, coverage, and sanitizer output paths are resolved through existing
ancestors and rejected if they enter evidence, including through symlinks. The
checks do not require an extracted firmware directory. ARM build and execution
helpers apply the same repository-wide evidence boundary to inputs. Reviewed
external inputs are permitted, while output paths must remain beneath `.work`
after resolving symlinks and existing ancestors. Payload and deployment build
helpers enforce that output boundary too. CTest runs with Make
jobserver variables cleared to avoid changing the shared pipe mode used by
macOS Make during parallel aggregate verification.

`--inventory` prints the verifier recipe without running it. Optional native
dependency builds require the explicit CMake paths described in
[`product/sipd/README.md`](../product/sipd/README.md).

## Console

Run the gateway and UI gates from the root:

```sh
make -C product/console test device-control
make -C product/console ui-install ui-test ui-build
```

`ui-test` and `ui-build` both depend on `ui-install`; they can run concurrently.
Changing a manifest while a command is active makes installation fail clearly
until that command exits. Cleanup retains the coordination lock directory below
`.work/locks` while moving generated content to Trash.
An interrupted move can resume with `make clean TRASH_BUNDLE=/path/to/the/same/bundle`.
The bundle records its source inventory before moving any entry and refuses
missing, replaced, or conflicting entries during recovery. Recovery validates
the destination identity and complete inventory on both sides. Use a new bundle
for generated content created after a completed cleanup.

Gateway objects and compiler-generated `.d` files live below `.work/build/console`.
Direct and transitive includes trigger recompilation of their owning translation
unit for both tests and the optional FastCGI executable.

The UI build runs TypeScript project compilation before Vite and writes to
`.work/dist/console-web`. After `ui-install`, start the Vite development server
through the dependency wrapper:

```sh
sh tooling/console/with-dependencies.sh vite --host 127.0.0.1
```

The server is a UI development surface, not the deployed nginx/FastCGI stack.
Building the optional FastCGI executable additionally requires a reviewed
FastCGI 2.4.7 installation:

```sh
make -C product/console fastcgi FCGI_PREFIX=/path/to/reviewed/fastcgi
make test-fastcgi FCGI_PREFIX=/path/to/reviewed/fastcgi
```

The explicit FastCGI integration lane starts the actual worker process and uses
local FastCGI, LSZ1, and RTSP fixtures to check request mapping, concurrent
workers, preview cancellation, and lease release. It requires native host
FastCGI libraries; ARM archives cannot substitute. It does not establish nginx
TLS termination or physical-device acceptance.

## Logical emulator

From `lab/emulator`:

```sh
make verify
make run
```

`make run` serves the logical control and vendor-compatible web surfaces on
loopback and persists fixture state below the root `.work` directory. Run the
control service alone with `make server`.

Optional synthetic media requires Docker and FFmpeg:

```sh
make compose-check
make media-up
make media-check
make media-down
```

`make arm-check` is a dry-run capability check. An actual ARM userspace probe
requires a separately available `linux/arm/v7` image and the explicit
`scripts/run-armv7-lab.sh --execute` path. Hardware investigation commands have
additional authorization and private-evidence requirements; use the
[emulator README](../lab/emulator/README.md).

## QEMU

From the root:

```sh
make qemu-build
make qemu-verify
make qemu-run
```

`make qemu-build` fetches QEMU tag `v11.0.3` when needed, verifies the pinned
source identity, applies the maintained machine worktree, and builds
`qemu-system-arm` with Ninja below `.work`. `make qemu-verify` requires that
source and binary; invoking `lab/qemu/scripts/verify.sh` directly without them
can perform only the available subset.

Booting requires the complete private recovered NAND main/OOB input set. See
[`lab/qemu/README.md`](../lab/qemu/README.md) for slot selection, web mode,
debug endpoints, and the current emulation boundary.

## Packaging and physical-device lanes

`make package-ls200` requires `SIPD_BINARY`, `GATEWAY_BINARY`, `DEVICE_BINARY`, `NGINX_BINARY`,
`ATOMIC_REPLACE_BINARY`, `MIME_TYPES`, and `FASTCGI_PARAMS`. It verifies and
assembles those inputs below `.work/dist/ls200` and emits an adjacent
`payload-manifest.sha256` receipt. `make package-qemu` passes that receipt
to the overlay builder. Direct overlay callers must pass the independently
retained manifest digest as the third argument; general installation requires
`LS200_ZOOM_MANIFEST_SHA256`. Keep the receipt with the reviewed build records,
separate from the candidate runtime tree. QEMU bootstrap carries it alongside
the trusted verifier and validates installed releases against it.

The `live-build`, `live-package`, `live-gates`, `live-preflight`,
`live-install`, `live-smoke`, soak, reboot, and removal targets are not ordinary
developer commands. They are fail-closed physical-lab phases requiring private
reviewed inputs and action-specific authorization. Follow
[`docs/operator/physical-private-lab.md`](operator/physical-private-lab.md).

## Documentation-only validation

For Markdown-only changes:

1. verify repository-relative links and documented paths;
2. compare every command with the owning Makefile, manifest, or script;
3. run an existing Markdown check if the repository later configures one;
4. run `git diff --check`; and
5. inspect `git status` and the final diff to confirm that only intended
   Markdown files changed.

The full application suite is optional for documentation-only changes unless a
repository instruction explicitly requires it.

## Explicit native and model gates

`make verify` remains the baseline host gate. It does not enable optional native
media dependencies or establish a rebuilt QEMU model result. Use these separate
lanes when the reviewed local inputs are available:

```sh
make verify-native NATIVE_INPUTS=/absolute/path/native-inputs.json
make quality-qemu-model QEMU_BASE_SOURCE=/absolute/path/pinned-qemu
make verify-qemu-model QEMU_BASE_SOURCE=/absolute/path/pinned-qemu
make verify-all NATIVE_INPUTS=/absolute/path/native-inputs.json \
  QEMU_BASE_SOURCE=/absolute/path/pinned-qemu
```

The native JSON object maps each `LS200_SIPD_<name>_INCLUDE_DIR` for `PJSIP`,
`FAAD2`, `SPEEXDSP`, and `SRTP` to an explicit native include directory. It maps
`LS200_SIPD_<name>_LIBRARY` for `PJSIP_UA`, `PJMEDIA`, `FAAD2`, `SPEEXDSP`, and
`SRTP` to an explicit library file. `LS200_SIPD_PJSIP_LIBRARIES` is a nonempty
array of absolute library paths in link order, including reviewed transitive
libraries. Relative paths and linker discovery flags are rejected. Review these
inputs against the dependency records before running this lane. ARM target
archives cannot substitute for native host libraries.

Native verification enables PJSIP, loopback TLS checks, AAC decoding, resampling,
AEC, and SRTP together, builds below `.work/build/sipd-native`, and runs CTest.
An absent explicitly requested input fails the lane. The QEMU lane requires the
pinned local checkout, materializes the checked-in patch stack below `.work`,
checks only owned machine files/tests, and builds with downloads disabled.
Missing host build tools or QEMU subprojects fail clearly. Its qtests use
synthetic machine inputs and do not boot firmware.

Optimization measurements belong under `.work/reports/optimization`. Compare
fixed inputs with seed `1`, one warmup, and at least five measured repetitions.
Keep CPU measurements and latency distributions separate from functional test
results and from unavailable native or deployed-server lanes.

Run the maintained synthetic gateway and SIP/media workloads against a preserved
source checkout with:

```sh
make benchmark-latency BENCHMARK_BASELINE=/absolute/path/preserved-source
```

The harness builds both revisions below `.work/build/performance` and writes
`.work/reports/optimization/runtime-benchmarks.json`. It measures status response
latency during login hashing and delayed mutations, plus synthetic SIP/media
work, retaining latency distributions, CPU, memory, and workload counters.
Gateway CPU samples cover the measured status-request window, so they do not
represent total login hashing cost. Run
both revisions under comparable host load. These measurements do not predict
physical LS-200 or network performance.

For a local QEMU tool/subproject cache outside `PATH`, pass
`QEMU_NINJA=/absolute/path/ninja` and
`QEMU_SUBPROJECT_SOURCE=/absolute/path/pinned-qemu/subprojects` to
`make verify-qemu-model` or `make verify-all`. The lane checks each donor
subproject against its reviewed wrap revision, rejects tracked changes, and
copies only committed input plus the checked-in Meson overlays. It never copies
untracked donor build output or downloads a missing subproject.

`QEMU_PYTHON=/absolute/path/python3` can select an existing reviewed build
interpreter with QEMU's pinned Python requirements. Its virtual-environment
path is retained so QEMU can reuse installed parent packages without downloads.
