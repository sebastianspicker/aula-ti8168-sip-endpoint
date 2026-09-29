# Development and build guide

Run commands from the repository root unless noted. Build trees, dependency
stores, generated reports, package output, and live-session state belong below
the ignored `.work/` directory. Do not create component-local virtual
environments, build directories, distributions, or persistent `node_modules`
directories.

The console dependency wrapper temporarily links
`product/console/web/node_modules` to its installation below `.work/` and
removes the link after the last concurrent user exits. Dependency installation
requires exclusive access; builds use shared leases.

## Toolchain

| Area | Required tools or inputs |
| --- | --- |
| Repository and emulator | Python 3.11+, POSIX shell, and `make` |
| SIP daemon | C compiler and CMake 3.16+ |
| Console device companion | C compiler, `pkg-config`, Jansson, and OpenSSL development files |
| Console UI | `pnpm`; dependencies are locked by `product/console/web/pnpm-lock.yaml` |
| QEMU | Git, Make, Ninja, a native compiler, and a reviewed QEMU v11.0.3 source checkout |
| Packaging | Reviewed prebuilt ARM binaries and nginx support files supplied explicitly |
| Physical lab | Private session file, reviewed build manifest, entropy helper, recovery authority, and action confirmations |

Optional PJSIP, libsrtp, FAAD2, SpeexDSP, FastCGI, and nginx inputs are never
downloaded or discovered automatically. Use the versions and integrity records
under [`dependencies/`](../dependencies/README.md) and each component's
third-party policy.

## Default host build

Prepare the locked UI dependencies once, then build the maintained host
components and exercise the synthetic emulator entry point:

```sh
make ui-install
make verify
```

`make verify` builds the SIP daemon, the device companion, and the browser UI.
It then queries the emulator using a state file below `.work/`.

There is no repository-wide formatter target.

## SIP daemon

For a focused release build:

```sh
cmake -S product/sipd -B .work/build/sipd -DCMAKE_BUILD_TYPE=Release
cmake --build .work/build/sipd
```

Optional native dependency builds require the explicit CMake paths described
in the [SIP daemon README](../product/sipd/README.md). Reviewed external inputs
are permitted; generated output remains below `.work/`.

## Console

Build the device companion and UI from the root:

```sh
make console-device
make ui-install ui-build
```

The UI build runs TypeScript project compilation before Vite and writes to
`.work/dist/console-web`. After `ui-install`, start the UI-only development
server through the dependency wrapper:

```sh
sh tooling/console/with-dependencies.sh vite --host 127.0.0.1
```

The server does not start nginx, FastCGI, or `aula-sipd`. Building the optional
FastCGI executable requires a reviewed FastCGI 2.4.7 installation:

```sh
make console-fastcgi FCGI_PREFIX=/path/to/reviewed/fastcgi
```

## Synthetic emulator

The maintained package models only local recording and stream state. It has no
control listener, firmware web API, or device access. Its CLI requires an
explicit state file below `.work/`:

```sh
PYTHONPATH=lab/emulator/src python3 -m aula_emulator status \
  --state-file .work/cache/emulator-run/synthetic-state.json
```

See the [emulator README](../lab/emulator/README.md) for the state-changing
commands and persistence compatibility boundary.

## QEMU

Use the [QEMU README](../lab/qemu/README.md) with an existing reviewed QEMU
v11.0.3 source checkout and an explicit reviewed guest image. The synthetic
model does not include a recovered NAND launcher, payload overlay, or product
acceptance workflow. `make qemu-run`, `make package-qemu`, and
`make qemu-acceptance` therefore fail closed.

## Packaging and physical-device lanes

`make package-ti8168` requires explicit `SIPD_BINARY`, `GATEWAY_BINARY`,
`DEVICE_BINARY`, `NGINX_BINARY`, `ATOMIC_REPLACE_BINARY`, `MIME_TYPES`, and
`FASTCGI_PARAMS` values. It verifies and assembles those inputs below
`.work/dist/aula-ti8168-sip-endpoint` and emits an adjacent
`payload-manifest.sha256` receipt. Keep the receipt with the reviewed build
records, separate from the candidate runtime tree.

The `live-build`, `live-package`, `live-gates`, `live-preflight`,
`live-install`, `live-smoke`, soak, reboot, and removal targets are not ordinary
developer commands. They are fail-closed physical-lab phases requiring private
reviewed inputs and action-specific authorization. Follow the
[physical private-lab workflow](operator/physical-private-lab.md).
