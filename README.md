# Aula — TI8168 SIP endpoint

Aula is a SIP endpoint and web console for TI8168 media boards. The
workspace also includes private firmware research, bounded QEMU models, and
appliance-specific integration experiments. It is intended for maintainers,
laboratory operators, and engineers working on the native endpoint.

The **TI DM8168 EVM**, **Z3-DM8168-RPS**, and **iWave DM8168 Qseven SOM**
provide board and module comparisons for this exploration. These examples do
not establish a particular manufacturer, PCB identity, or supported deployment
target for the TI8168 media board. See
[TI8168 media board and comparison platforms](docs/reference/ti8168-board.md).

The repository is not a released appliance image or a supported Zoom client.
The maintained services have host, cross-build, and QEMU test paths, but public
Zoom interoperability, trusted production TLS, sustained hardware media, and
full recovery have not been established as release claims. The synthetic QEMU model does not establish device boot, processor behavior,
or media support.

## What is here

| Path | Purpose | Runtime and independence |
| --- | --- | --- |
| [`product/sipd/`](product/sipd/README.md) | Native SIP, SDP, RTP/RTCP, media, and local-control daemon | C/CMake service; independently buildable |
| [`product/console/`](product/console/README.md) | HTTPS console, FastCGI policy gateway, and React UI | C gateway plus TypeScript/Vite application |
| [`lab/emulator/`](lab/emulator/README.md) | Synthetic local recording/stream state, events, and persistence | Python 3.11 package; no device or network interface |
| [`lab/qemu/`](lab/qemu/README.md) | Synthetic TI8168 media-board model | Pinned QEMU build and verification lane |
| [`lab/sip-peer/`](lab/sip-peer/README.md) | Deterministic SIP/RTP peer for the QEMU and physical private-lab counterpart | Python package shared by both callers |
| [`deployment/`](deployment/README.md) | Manifest-verified payload assembly, QEMU staging, and physical-target lifecycle | Python and shell packaging/installation tools |
| [`dependencies/`](dependencies/README.md) | Third-party pins, advisories, and digest-gated preparation | Policy and offline input verification |
| `evidence/` | Untracked private corpus of sanitized firmware findings, reproducibility records, and private/archive boundaries; present only in checkouts that hold it | Research evidence; never a product dependency |
| [`tooling/quality/`](tooling/quality/README.md) | Maintained-source complexity/clone gate and layout policy | Locked Python analyzer |
| [`tooling/workspace/`](tooling/workspace/README.md) | Shared `.work` output guard, trash-based cleanup, evidence-corpus helpers | Python/shell used by every component's build |
| [`tooling/console/`](tooling/console/) | Console dependency install/cleanup coordination | Python/shell dependency-lease tooling |
| [`tooling/live/`](tooling/live/) | Physical-lab build, package, gate, and campaign orchestration | Python; see [`docs/operator/physical-private-lab.md`](docs/operator/physical-private-lab.md) |
| [`tooling/device-evidence/`](tooling/device-evidence/README.md) | Owned-device evidence collectors and on-device read-only inventory scripts | Authorized-operator tooling |
| [`tooling/performance/`](tooling/performance/README.md) | Synthetic gateway/SIP/media benchmark harness | Python benchmark tooling |

The [architecture guide](docs/ARCHITECTURE.md) describes component boundaries,
dependency direction, state ownership, and the principal runtime flows.

## Prerequisites

The complete repository gate uses:

- Python 3.11 or newer and [`uv`](https://docs.astral.sh/uv/);
- a POSIX shell, `make`, a C compiler, CMake 3.16 or newer, CTest,
  `cmake-format`, and `shellcheck`;
- `pkg-config` with Jansson and OpenSSL development files for console tests;
- `pnpm` for the React console; and
- Git, Make, and Ninja for the QEMU build.

Some targets need additional reviewed, locally supplied inputs. QEMU boot needs
private recovered NAND artifacts. Product packaging needs prebuilt ARM
binaries and nginx support files. Physical-device actions need an operator-owned
session, recovery authority, and explicit confirmations. See
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) before running those lanes.

## Quick start

From the repository root, run the complete local verification gate:

```sh
make verify
```

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) for the narrower component
gates, the evidence and native/QEMU-model lanes, and prerequisites.

The maintained QEMU lane builds and verifies a synthetic TI8168 media-board
model. It accepts only caller-supplied reviewed ELF or raw images; the previous
recovered NAND/web launcher and two-slot product acceptance workflow are not
part of this lane. `make qemu-run`, `make qemu-acceptance`, and `make package-qemu`
fail closed until a reviewed guest workflow exists. See the [QEMU README](lab/qemu/README.md).

## Configuration and generated state

Safe templates are committed at:

- [`product/sipd/config/aula-sipd.example.conf`](product/sipd/config/aula-sipd.example.conf)
- [`product/console/gateway/config/aula-console.example.conf`](product/console/gateway/config/aula-console.example.conf)
- [`docs/operator/live-build-inputs.example.json`](docs/operator/live-build-inputs.example.json)

Templates are not production configuration. The SIP template is inert and
selects plaintext compatibility only for an isolated private-lab profile; the
daemon's compiled media-security default is `prefer_srtp`. Runtime credentials,
certificates, device identifiers, captures, and complete firmware artifacts do
not belong in version control.

All generated build, cache, distribution, report, and live-session state belongs
under ignored [`.work/`](docs/ARCHITECTURE.md#repository-and-data-boundaries).
The entire `evidence/` corpus is ignored, including decompiled source and vendor
documents. Complete device-derived or authenticated evidence belongs under
`evidence/private/`.

## Public release boundary

Public release is blocked by default. `make release-source` exports only
committed files with exact SHA-256, provenance, and license review records;
the approval list is currently empty. Ordinary Git archives exclude all files.
Private payloads, recovered firmware, vendor UI, and this repository's Git
history must not be published. See [public release policy](docs/PUBLIC_RELEASE.md).

## Documentation

- [Development and verification](docs/DEVELOPMENT.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Security and disclosure boundary](docs/SECURITY.md)
- [Physical private-lab workflow](docs/operator/physical-private-lab.md)
- [Upgrade to Aula](docs/operator/upgrade-to-aula.md)
- [Product purpose](docs/product/PRODUCT.md) and [console design system](docs/product/DESIGN.md)
- [QEMU model fidelity](lab/qemu/FIDELITY.md)
- [Logical-emulator fidelity](lab/emulator/FIDELITY.md)
- Firmware-analysis results: `evidence/firmware-analysis/report.md` (in the private corpus)

There is no license file in this worktree, and no public release or supported
product is currently offered.
