# Aula — TI8168 SIP endpoint

Aula is a SIP endpoint and web console for TI8168 media boards. You can develop
the console on a host, run the ARM application under QEMU, and test selected
TI8168 peripherals before testing on a physical board.

The **TI DM8168 EVM**, **Z3-DM8168-RPS**, and **iWave DM8168 Qseven SOM**
provide board and module comparisons for this exploration. These examples do
not establish a particular manufacturer, PCB identity, or supported deployment
target for the TI8168 media board. See
[TI8168 media board and comparison platforms](docs/reference/ti8168-board.md).

The QEMU model boots an explicitly supplied original Linux kernel and root
filesystem and runs selected ARM product tests. It models timer clocks,
interrupts, GPIO, watchdog resets and channel-zero Ethernet DMA. DSP, video,
audio, full clock/power control and board wiring remain incomplete; see the
[fidelity matrix](lab/qemu/FIDELITY.md). This is development software, not a
released appliance image. Public Zoom interoperability, production TLS,
sustained hardware media and full recovery still need device testing.

## What is here

| Path | Purpose | Runtime and independence |
| --- | --- | --- |
| [`product/sipd/`](product/sipd/README.md) | Native SIP, SDP, RTP/RTCP, media, and local-control daemon | C/CMake service; independently buildable |
| [`product/console/`](product/console/README.md) | HTTPS console, FastCGI policy gateway, and React UI | C gateway plus TypeScript/Vite application |
| [`lab/emulator/`](lab/emulator/README.md) | Synthetic activity state, LSZ1 backend, and console activity provider | Python 3.11+; inherited local Unix sockets; no SIP, media, or hardware |
| [`lab/qemu/`](lab/qemu/README.md) | Partial TI8168 model, generated probes and private original-kernel checks | Pinned QEMU build; firmware inputs remain external |
| [`lab/arm-userspace/`](lab/arm-userspace/README.md) | Maintained ARM application and legacy ABI checks | Public build inputs; separately supplied execution runtime |
| [`lab/sip-peer/`](lab/sip-peer/README.md) | Deterministic SIP/RTP peer for QEMU and controlled physical-lab use | Python package shared by both callers |
| [`deployment/`](deployment/README.md) | Manifest-verified payload assembly and physical-target lifecycle | Python and shell packaging/installation tools; QEMU packaging is unavailable |
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

QEMU model work additionally needs Git, Ninja, native libraries and GNU ARM
binutils. Its tests generate their own guests and backing data. Other targets
need additional reviewed, locally supplied inputs. Product packaging needs prebuilt ARM
binaries and nginx support files. Physical-device actions need an operator-owned
session, recovery authority, and explicit confirmations. See
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) before running those lanes.

## Quick start

From the repository root, build the maintained components and verify the
synthetic emulator:

```sh
make ui-install
make verify
```

See [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) for component builds,
optional integrations, and prerequisites.

To use the maintained web console with the synthetic emulator:

```sh
make emulator-ui
```

Open `https://127.0.0.1:8443/zoom/`. The launcher prepares a local TLS
certificate and a private login file, then runs nginx, the real gateway, and
both emulator providers. Select **Synthetic room** under **Join a meeting**
to exercise the call controls. See the [browser quick start](lab/emulator/README.md#browser-console)
for credentials, TLS, activity controls, and stopping the services.

The separate QEMU lane builds and verifies the partial TI8168 media-board
model. `make qemu-run` executes the project-owned Cortex-A8 probe; explicitly
supplied private firmware inputs support the original-kernel checks.
The browser launcher above runs on the host. `make qemu-acceptance` and
`make package-qemu` return an error because the product deployment workflow
for QEMU is not implemented. See the [QEMU README](lab/qemu/README.md).

## Configuration and generated state

Safe templates are committed at:

- [`product/sipd/config/aula-sipd.example.conf`](product/sipd/config/aula-sipd.example.conf)
  and its developer-host copy
  [`aula-sipd.host.example.conf`](product/sipd/config/aula-sipd.host.example.conf)
- [`product/console/gateway/config/aula-console.example.conf`](product/console/gateway/config/aula-console.example.conf)
- [`docs/operator/live-build-inputs.example.json`](docs/operator/live-build-inputs.example.json)

Templates are not production configuration. The SIP template names device
paths; the host copy uses `/tmp/aula-sipd/` (create it with
`mkdir -m 700 /tmp/aula-sipd`). The SIP template is inert and
selects plaintext compatibility only for an isolated private-lab profile; the
daemon's compiled media-security default is `prefer_srtp`. Runtime credentials,
certificates, device identifiers, captures, and complete firmware artifacts do
not belong in version control.

All generated build, cache, distribution, report, and live-session state belongs
under ignored [`.work/`](docs/ARCHITECTURE.md#repository-and-data-boundaries).
Private payloads, recovered firmware, vendor UI, credentials, and authenticated
session material are not part of this repository.

Firmware tools require private output directories below `.work`.
`make emulator-source-check` checks the reviewed emulator source inventory,
QEMU patch targets and Git index for accidental private artifacts. It also
runs in `make verify`. Public hardware references and model assumptions are
listed in the [provenance record](lab/qemu/PROVENANCE.md).

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
- [ARM application execution](lab/arm-userspace/README.md)
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
