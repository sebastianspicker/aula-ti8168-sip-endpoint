# Aula — TI8168 SIP endpoint

Aula is a SIP endpoint and web console for TI8168 media boards. The repository
also includes a bounded QEMU model, a synthetic state emulator, and deployment
support for controlled laboratory use. It is intended for maintainers,
laboratory operators, and engineers working on the native endpoint.

The **TI DM8168 EVM**, **Z3-DM8168-RPS**, and **iWave DM8168 Qseven SOM**
provide board and module comparisons for this exploration. These examples do
not establish a particular manufacturer, PCB identity, or supported deployment
target for the TI8168 media board. See
[TI8168 media board and comparison platforms](docs/reference/ti8168-board.md).

The repository is not a released appliance image or a supported Zoom client.
The maintained services have host, cross-build, and QEMU development paths, but public
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
| [`lab/sip-peer/`](lab/sip-peer/README.md) | Deterministic SIP/RTP peer for QEMU and controlled physical-lab use | Python package shared by both callers |
| [`deployment/`](deployment/README.md) | Manifest-verified payload assembly, QEMU staging, and physical-target lifecycle | Python and shell packaging/installation tools |
| [`dependencies/`](dependencies/README.md) | Third-party pins, advisories, and digest-gated preparation | Policy and offline input verification |
| [`tooling/console/`](tooling/console/) | Console dependency install/cleanup coordination | Python/shell dependency-lease tooling |
| [`tooling/live/`](tooling/live/) | Physical-lab build, package, gate, and campaign orchestration | Python; see [`docs/operator/physical-private-lab.md`](docs/operator/physical-private-lab.md) |

The [architecture guide](docs/ARCHITECTURE.md) describes component boundaries,
dependency direction, state ownership, and the principal runtime flows.

## Prerequisites

The default build uses:

- Python 3.11 or newer;
- a POSIX shell, `make`, a C compiler, and CMake 3.16 or newer;
- `pkg-config` with Jansson and OpenSSL development files for the console;
- `pnpm` for the React console.

QEMU model work additionally needs Git and Ninja plus a reviewed QEMU source
checkout and caller-supplied guest or synthetic backing images. Other targets
need additional reviewed, locally supplied inputs. Product packaging needs prebuilt ARM
binaries and nginx support files. Physical-device actions need an operator-owned
session, recovery authority, and explicit confirmations. See
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) before running those lanes.

## Quick start

From the repository root, build the maintained components and query the
synthetic emulator:

```sh
make verify
```

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) for component builds,
optional integrations, and prerequisites.

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
Private payloads, recovered firmware, vendor UI, credentials, and authenticated
session material are not part of this repository.

The physical-target scripts refer to a small number of pre-existing firmware
paths and readiness interfaces. Those references are integration facts only:
the referenced executables, libraries, services, firmware, and filesystem
content are not copied, linked into this source tree, or redistributed here.

## Documentation

- [Development and verification](docs/DEVELOPMENT.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Security and disclosure boundary](docs/SECURITY.md)
- [Physical private-lab workflow](docs/operator/physical-private-lab.md)
- [Upgrade to Aula](docs/operator/upgrade-to-aula.md)
- [QEMU model fidelity](lab/qemu/FIDELITY.md)
- [Logical-emulator fidelity](lab/emulator/FIDELITY.md)

## License

Unless a file or path carries a more specific notice, the maintainer-owned work
in this repository is Copyright (c) 2026 Sebastian J. Spicker and licensed
under the [GNU General Public License, version 3 or later](LICENSE).

No upstream source tree, prebuilt dependency, firmware image or recovered
vendor artifact is vendored here. The repository does contain three textual
patches representing maintained changes for separately obtained QEMU, PJSIP
and nginx sources; those paths retain their compatible upstream license terms.
See [the third-party notices](THIRD_PARTY_NOTICES.md) and the canonical texts
under [`LICENSES/`](LICENSES/).

The source license does not by itself approve or establish a supported binary
distribution. Every release artifact still needs its dependency, notice,
security and target-validation gates completed.
