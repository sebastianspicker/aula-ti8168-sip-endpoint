# LS-200 Zoom

LS-200 Zoom is a research and development workspace for understanding the AREC
LS-200, running recovered firmware in QEMU, and building a reversible SIP and
web-console extension for an owned device. It is intended for maintainers,
laboratory operators, and engineers working on the native endpoint.

The repository is not a released appliance image or a supported Zoom client.
The maintained services have host, cross-build, and QEMU test paths, but public
Zoom interoperability, trusted production TLS, sustained hardware media, and
full recovery have not been established as release claims. The recovered QEMU
machine also lacks the device's M3, DSP, capture, display, and complete media
behavior.

## What is here

| Path | Purpose | Runtime and independence |
| --- | --- | --- |
| [`product/sipd/`](product/sipd/README.md) | Native SIP, SDP, RTP/RTCP, media, and local-control daemon | C/CMake service; independently buildable |
| [`product/console/`](product/console/README.md) | HTTPS console, FastCGI policy gateway, and React UI | C gateway plus TypeScript/Vite application |
| [`lab/emulator/`](lab/emulator/README.md) | Logical control, web, state, media-fixture, and investigation environment | Python 3.11 package with independent CLIs |
| [`lab/qemu/`](lab/qemu/README.md) | Recovered Linux and NAND model for the TI8168-based unit | Pinned QEMU build and run lane |
| [`deployment/`](deployment/README.md) | Manifest-verified payload assembly, QEMU staging, and physical-target lifecycle | Python and shell packaging/installation tools |
| [`dependencies/`](dependencies/README.md) | Third-party pins, advisories, and digest-gated preparation | Policy and offline input verification |
| [`evidence/`](evidence/firmware-analysis/README.md) | Sanitized firmware findings, reproducibility records, and private/archive boundaries | Research evidence; never a product dependency |
| [`tooling/`](tooling/quality/README.md) | Repository quality, live-campaign, console, and evidence utilities | Developer and authorized-operator tooling |

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

Useful narrower checks are:

```sh
make check-layout
make quality
make test-product
make test-lab
make test-deployment
```

Build and start the recovered QEMU development machine with:

```sh
make qemu-build
make qemu-verify
make qemu-run
```

The build fetches the pinned QEMU source when it is absent. `make qemu-run`
requires the private recovered NAND set. To start the recovered loopback web
path instead of the default shell, use:

```sh
LS200_BOOT_MODE=web make qemu-run
```

The recovered web interface can start, but recovered API and media services may
remain unavailable because the modeled machine does not provide all original
processors and hardware.

## Configuration and generated state

Safe templates are committed at:

- [`product/sipd/config/ls200-sipd.example.conf`](product/sipd/config/ls200-sipd.example.conf)
- [`product/console/gateway/config/ls200-console.example.conf`](product/console/gateway/config/ls200-console.example.conf)
- [`docs/operator/live-build-inputs.example.json`](docs/operator/live-build-inputs.example.json)

Templates are not production configuration. The SIP template is inert and
selects plaintext compatibility only for an isolated private-lab profile; the
daemon's compiled media-security default is `prefer_srtp`. Runtime credentials,
certificates, device identifiers, captures, and complete firmware artifacts do
not belong in version control.

All generated build, cache, distribution, report, and live-session state belongs
under ignored [`.work/`](docs/ARCHITECTURE.md#repository-and-data-boundaries).
Complete device-derived or authenticated evidence belongs under ignored
`evidence/private/`; only its policy README is versioned.

## Documentation

- [Development and verification](docs/DEVELOPMENT.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Security and disclosure boundary](docs/SECURITY.md)
- [Physical private-lab workflow](docs/operator/physical-private-lab.md)
- [Product purpose](docs/product/PRODUCT.md) and [console design system](docs/product/DESIGN.md)
- [QEMU fidelity and boot status](lab/qemu/BOOT_STATUS.md)
- [Logical-emulator fidelity](lab/emulator/FIDELITY.md)
- [Firmware-analysis results](evidence/firmware-analysis/report.md)

There is no license file in this worktree and no public release or support
policy is asserted here.
