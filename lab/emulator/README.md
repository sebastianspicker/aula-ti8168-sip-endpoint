# LS-200 logical emulator

This Python package provides a local model of LS-200 control, web, state, event,
and selected media behavior. It is useful for client development, parser and
state-flow tests, and recovered-interface analysis. It is not a TI8168 virtual
machine and does not establish appliance timing, hardware media, SIP, or Zoom
interoperability.

## Implemented surfaces

- AREC TCP control framing and supported LS-200 commands on loopback;
- recovered vendor web assets with firmware-compatible routes and API models;
- bounded Engine.IO 3 / Socket.IO 1.x event delivery;
- shared logical device state with optional atomic JSON persistence;
- logical TI, MCU, CBox, scheduler, and update state machines;
- optional synthetic H.264/AAC through a pinned MediaMTX container; and
- guarded serial, network-observation, device-access, and evidence CLIs.

Recovered web assets and firmware-derived data remain evidence inputs. The
emulator does not import recovered Python services or execute firmware code.
See [`FIDELITY.md`](FIDELITY.md) for the verified substitutions and exclusions.

## Requirements

- Python 3.11 or newer
- `uv`
- `make`
- `shellcheck` for the complete verification gate
- Docker, FFmpeg, and FFprobe only for optional synthetic media

## Set up and verify

From this directory:

```sh
make verify
```

This creates the locked environment below the repository `.work` root, runs
the maintained quality scope and pytest suite, compiles Python, checks shell
scripts, exercises a loopback control exchange, and performs a dry-run ARM
capability check. It does not start Docker, contact hardware, or execute ARM
firmware.

From the repository root, `make test-lab` additionally runs the QEMU and live
tooling test suites.

## Run the emulator

From this directory:

```sh
make run
```

The command starts the control service on loopback port `5080` and the
vendor-compatible web surface on loopback port `8080`. It prints a generated
administrator password on an interactive terminal and stores fixture state at
`../../.work/cache/emulator-run/ls200-state.json`.

For a deterministic local password, keep it in the process environment rather
than a committed file:

```sh
LS200_EMULATOR_ADMIN_PASSWORD=local-only-value make run
```

Run only the control service with `make server`. Direct CLI invocations use the
safer bearer-first web mutation policy unless `--allow-cookie-mutations` is
explicitly supplied; the Make target enables cookie mutations because the
recovered pages require them.

Use the control client from a second terminal:

```sh
uv run ls200-emulator get GM
uv run ls200-emulator get ST
uv run ls200-emulator set RC
uv run ls200-emulator set LO --params "02"
```

Parameters are hexadecimal bytes. The listener and browser Origin/Host checks
remain loopback-only; there is no supported LAN exposure option.

## Runtime and state

`EmulatorRuntime` creates one shared `DeviceState`, JSON store, event broker,
logical backend set, TCP control server, and web server. Startup establishes
logical TI/media/MCU/CBox readiness before opening public listeners. Shutdown
closes those listeners before resetting the logical backends.

Successful supported mutations update the shared state and publish events. A
configured state file is serialized synchronously, fsynced, atomically
replaced, and followed by a parent-directory fsync before the event and control
acknowledgement are published. With persistence disabled, mutations skip
snapshot serialization while preserving event-before-acknowledgement order.
Web sessions, bearer tokens, login throttling, and lockout state remain in
memory.

`scripts/benchmark-persistence.py` measures the same fixed sequence with and
without storage using seed 1, one warmup, and five recorded repetitions. It
writes its JSON report atomically below `.work/reports/optimization/` and also
checks persisted-state recovery and response ordering.

Logical readiness does not imply native-media readiness. Diagnostics stay
degraded when no vendor encoder, media daemon, SysLink driver, MCU hardware, or
renderer is present.

## Optional synthetic media

The Compose profile runs `bluenviron/mediamtx:1.20.0` read-only, drops all
capabilities, applies resource limits, and publishes only loopback ports. Start
Docker, then run:

```sh
make compose-check
make media-up
make media-check
make media-down
```

Host FFmpeg publishes a synthetic H.264/AAC stream. It is a parser and client
fixture, not hardware encoder output. The image is version-pinned but not
registry-digest-pinned, so it is not a reproducible supply-chain artifact until
an approved platform digest is recorded.

## ARM userspace check

`make arm-check` validates the narrow offline-lab contract without executing an
ARM binary. An actual probe requires an already-local image named by
`LS200_ARMV7_LAB_IMAGE` whose inspected platform is exactly `linux/arm/v7`:

```sh
LS200_ARMV7_LAB_IMAGE=approved-local-image \
  ./scripts/run-armv7-lab.sh --execute
```

The container uses no network, a read-only root, dropped capabilities,
resource limits, and a read-only evidence mount. Its only guest action is a
BusyBox help probe. It does not run init, services, storage, update, media, MCU,
or kernel-module code.

## Physical investigation tools

The installed package also exposes:

- `ls200-rs232-capture` for receive-only bounded serial capture;
- `ls200-rs232-probe` for allowlisted guarded GET probes;
- `ls200-network-observe` for passive host-interface and neighbor-cache reads;
- `ls200-device-access` for one explicit owned direct-link target; and
- `ls200-autonomous-investigation` for manifest-bound serial investigations.

The capture and guarded-probe commands create a private output directory when
it is absent. An existing output directory must be empty, owned by the current
user, a real directory rather than a symlink, and mode `0700`; evidence files
are created with mode `0600`.

Use each command's `--help` for its current arguments. Dry-run or passive modes
do not authorize a hardware action. Any active identity probe, serial write,
service connection, authenticated access, or persistent device change requires
separate explicit authority and private evidence handling. Working exploit or
credential-installation procedures are intentionally not documented here; see
[`docs/SECURITY.md`](../../docs/SECURITY.md).

Run the macOS adapter preflight without opening a serial device:

```sh
make rs232-check
```

For the controlled product deployment campaign, use the separate
[`physical-private-lab.md`](../../docs/operator/physical-private-lab.md).

## Source layout

- `src/ls200_emulator/runtime.py`: service composition and lifecycle
- `state.py`, `persistence.py`, `events.py`: shared state and event ownership
- `protocol.py`, `server.py`, `client.py`: control transport
- `web_*.py`: HTTP authentication, routing, state, and recovered asset serving
- `ti_backend*.py`, `mcu*.py`, `cbox_bus.py`: logical native-service models
- `media.py`, `media_factory*.py`: optional fixture-media lifecycle
- `serial_*.py`, `network_*.py`, `device_access*.py`, `investigation_*.py`:
  guarded evidence and device tooling
- `tests/`: unit and integration coverage for these contracts

Do not broaden a logical model beyond captured or source-derived evidence.
Record unsupported behavior explicitly rather than inventing hardware or
cloud responses.
