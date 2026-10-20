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
| QEMU | Git, Make, Ninja, native compiler, local QEMU build dependencies, and GNU ARM binutils for the generated guest |
| Packaging | Reviewed prebuilt ARM binaries and nginx support files supplied explicitly |
| Physical lab | Private session file, reviewed build manifest, entropy helper, recovery authority, and action confirmations |

The default build does not download optional native dependencies. Explicit
preparation commands, including `make sipd-pjsip`, `make console-fastcgi-host`
and `make emulator-ui`, download pinned archives and check their digests.
Use the versions and integrity records under
[`dependencies/`](../dependencies/README.md).

## Default host build

Prepare the locked UI dependencies once, then build the maintained host
components and exercise the synthetic emulator entry point:

```sh
make ui-install
make verify
```

`make verify` builds the SIP daemon, the device companion, and the browser UI,
compiles every gateway translation unit that does not need FastCGI headers,
runs the host unit tests, validates a host copy of the daemon configuration,
and verifies emulator reset, mutation, and reload in a private temporary
directory. It also checks the public emulator source inventory and Git index.
Existing interactive emulator state is not used by verification.

There is no repository-wide formatter target.

Continuous integration (`.github/workflows/verify.yml`) runs `make ui-install`
and `make verify` on Linux (gcc) and macOS (clang) for every push and pull
request. Both jobs select Python 3.11 explicitly for the supported emulator
floor.

## Unit tests

`make test` runs every host unit test; `make verify` includes it. Each lane can
also run on its own:

| Target | Tests | Location |
| --- | --- | --- |
| `make sipd-test` | C programs registered with CTest and linked against `aula_sipd_core` | `product/sipd/tests/` |
| `make console-test` | C programs linked against the gateway and device-client sources, without FastCGI | `product/console/tests/` |
| `make emulator-test` | Python `unittest`, including real C-client protocol contracts | `lab/emulator/tests/` |
| `make ui-test` | Vitest unit tests for the console's pure TypeScript modules (needs `make ui-install`) | `product/console/web/src/*.test.ts` |
| `make arm-userspace-test` | ARM runner input and failure-path checks; no cross compiler required | `lab/arm-userspace/tests/` |
| `make qemu-input-test` | Firmware manifests, generated archives, NAND composition and private output paths | `lab/qemu/firmware/tests/` |
| `make emulator-source-check` | Source inventory, patch targets and private-artifact checks, including staged content | `lab/qemu/tests/` and `lab/PUBLIC_SOURCES.json` |

Tests need no running product service, physical device, browser or external
network. Some create local Unix sockets and child processes. Test data goes
to temporary directories that the tests remove.
Configure the daemon with `-DAULA_SIPD_BUILD_TESTS=OFF` to skip the C test
programs. Cross builds omit tests unless `AULA_SIPD_ENABLE_CROSS_TESTS=ON`
and an explicit `CMAKE_CROSSCOMPILING_EMULATOR` are supplied.

## SIP daemon

For a focused release build:

```sh
cmake -S product/sipd -B .work/build/sipd -DCMAKE_BUILD_TYPE=Release
cmake --build .work/build/sipd
```

Optional native dependency builds require the explicit CMake paths described
in the [SIP daemon README](../product/sipd/README.md). The default build cannot
run `aula-sipd --foreground`; `make sipd-pjsip` downloads and builds the pinned,
digest-checked pjproject below `.work/` and links a daemon that can (see the
README's "Host build with PJSIP"). Reviewed external inputs
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

On a developer host, `make console-fastcgi-host` downloads the FastCGI 2.4.7
archive pinned in `dependencies/dependency-lock.json`, refuses it unless its
SHA-256 matches, builds `libfcgi` below `.work/build/fastcgi/`, and links
`.work/build/console/aula-gateway-fcgi`. CI runs it on Linux and macOS.

### Running the whole console on a host

For the synthetic emulator, run `make emulator-ui` and open
`https://127.0.0.1:8443/zoom/`. This optional launcher builds native nginx,
FastCGI, the gateway and the separate emulator-mode UI, provisions a private
local administrator through the normal API, and starts both synthetic
providers. Login credentials and logs are below `.work/console-local`.
See the [browser-console instructions](../lab/emulator/README.md#browser-console)
for the local certificate, call/activity controls, status and shutdown.

To use a native SIP daemon instead of the synthetic provider, build:

- `make sipd-pjsip` gives a daemon that runs `--foreground`;
- `make console-fastcgi-host` gives the gateway;
- `make ui-build` gives the UI.

Connecting these processes remains manual and has not been tested end to end:

1. Build nginx 1.31.4 for the host with
   `sh dependencies/scripts/build-nginx-host.sh`.
2. Create a TLS certificate.
3. Write a gateway policy (`gateway.conf`) that names the daemon's control
   socket and an account store, with a `bootstrap_code` for the first account
   (see the gateway README).
4. Run the daemon with `enable_local_control = true` and `gateway_uid` set to
   your own UID.
5. Start the gateway with `--policy` and `--listen`, and nginx with an adapted
   copy of `product/console/nginx/nginx-qemu-dev.conf`.

The fixed paths below `/run` and `/var/lib` need adjusting or creating for a
non-root user.

## Synthetic emulator

The package models recording and streaming activity and can stand in for
the maintained LSZ1 daemon and revision-1 device-status interfaces over
inherited Unix sockets. It performs no SIP, media, firmware, or device work.
Its state CLI requires an explicit state file below `.work/` by default:

```sh
PYTHONPATH=lab/emulator/src python3 -m aula_emulator status \
  --state-file .work/cache/emulator-run/synthetic-state.json
```

Run `make emulator-test` for the Python suite and C-client contract tests, or
`make emulator-status` for an isolated CLI smoke test. Installed CLI users can
select a private state directory with `--work-root`.

See the [emulator README](../lab/emulator/README.md) for activity commands,
socket activation, host gateway policy, fault injection, and state migration.
The LSZ1 schema inventory is maintained alongside its C-client contract tests.
`make emulator-ui` handles nginx, TLS, gateway configuration and account
bootstrap for the synthetic providers.

## QEMU

The [QEMU README](../lab/qemu/README.md) describes pinned v11.0.3 source
preparation and offline build prerequisites. `make qemu-run` builds and runs
the project-owned Cortex-A8 guest through UART and two timer interrupt
handlers. `make qemu-verify` checks the source patch and runs generated tests
for NAND, INTC, timers, PRCM clock controls, GPIO, watchdog resets, Ethernet
descriptor chains and kernel loading. It needs no vendor firmware. Missing
tools or failed probes fail the command; source-only checks do not establish
runtime behavior.

The [private original-kernel runner](../lab/qemu/firmware/README.md) accepts
pinned local kernel/runtime inputs, builds a minimal rescue archive, or boots
a verified update-derived NAND fixture. It has reached the original Linux
2.6.37/UBIFS root and run the maintained daemon/C checks and a loopback SIP
dialog. Its outputs must stay below `.work` in private directories; firmware,
board snapshots and receipts must not be committed. Generated input
tests run through `make qemu-input-test` and default `make verify`.

The separate [ARM application lane](../lab/arm-userspace/README.md) cross-compiles
and executes the maintained daemon with QEMU user mode. Its
[legacy variant](../lab/arm-userspace/LEGACY.md) uses authenticated public
development inputs and a separately supplied private runtime; the resulting
ELFs can be selected for the original-kernel rescue checks. Neither lane
establishes hardware media or full appliance acceptance. `make package-qemu`
and `make qemu-acceptance` still fail closed.

When adding an emulator source file, add a reviewed entry to
`lab/PUBLIC_SOURCES.json`. QEMU patch targets also need an entry there.
Run `make emulator-source-check` after staging: it checks staged bytes as well
as working files. The check catches accidental artifacts and invalid paths;
it cannot determine whether arbitrary source text contains confidential data.

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
