# Synthetic Aula emulator

This Python 3.11+ package supplies local activity state and synthetic stand-ins
for the maintained Aula LSZ1 daemon and device-status protocols. It is a host
development tool with no SIP, media, firmware, serial, or hardware behavior.
The product has no runtime dependency on this package.

## Browser console

Start the maintained product UI against the synthetic providers:

```sh
make emulator-ui
```

This explicit, optional command builds the pinned native nginx/FastCGI
dependencies, the real C gateway, and a separate emulator UI bundle. It
listens only on `https://127.0.0.1:8443/zoom/` and runs in the foreground.
It requires the normal console build prerequisites plus `curl`, `tar`,
`shasum`, and an OpenSSL command-line tool.

The development certificate is self-signed. On first opening the URL, review
and accept the browser's local-certificate prompt. The launcher does not add
trust to the system or change browser security settings. Sign in with the
random local credentials in `.work/console-local/credentials.json` (mode
0600); the launcher creates this account through the normal bootstrap API.
There are no shared default passwords or authentication bypasses.

In **Room**, expand **Join a meeting**, choose **Synthetic room** in the
Directory selector, and click **Join call**. The saved entry selects the
emulator's private-lab profile. Mute, video, keypad, layout, keyframe and
hang-up controls operate on synthetic call state through the real gateway.
The banner identifies this environment. Preview, hardware media, Library,
Schedule and device credential workflows retain their explicit unavailable
states.

Change the recording/streaming displays from another terminal:

```sh
PYTHONPATH=lab/emulator/src python3 -m aula_emulator record-start \
  --state-file .work/ec/activity.json
PYTHONPATH=lab/emulator/src python3 -m aula_emulator stream-pause \
  --state-file .work/ec/activity.json
```

The console observes these changes on its next poll. Use `record-stop` and
`stream-stop` to return to the stopped state.

```sh
make emulator-ui-status
make emulator-ui-stop
# Reuse the prepared binaries and UI:
make emulator-ui EMULATOR_UI_ARGS="--no-build"
# Choose a different loopback port:
make emulator-ui EMULATOR_UI_ARGS="--port 9443"
```

Ctrl-C also stops the owned child services. Accounts, directory entries,
activity, and SIP settings persist across restarts; calls and sessions do not.
Logs and TLS/account state live under `.work/console-local`; the short
`.work/ec` path holds the providers' sockets and state. A second launcher is
refused. The separate emulator bundle lives in `.work/dist/emulator-web`;
ordinary product builds remain in `.work/dist/console-web` without the
emulator banner.

This is the host console development lane. Original-kernel ARM execution and
TI8168 device-model verification remain the separate [QEMU lane](../qemu/README.md).

## Activity state

From the repository root:

```sh
export PYTHONPATH=lab/emulator/src
python3 -m aula_emulator status --state-file .work/cache/emulator-run/state.json
python3 -m aula_emulator record-start --state-file .work/cache/emulator-run/state.json
python3 -m aula_emulator stream-pause --state-file .work/cache/emulator-run/state.json
python3 -m aula_emulator reset --state-file .work/cache/emulator-run/state.json
```

`record-start`, `record-stop`, `record-pause`, and `record-unknown` set
`recording` to `active`, `inactive`, `paused`, and `unknown`. The equivalent
`stream-*` commands set `streaming`. `status` prints the current JSON snapshot.
A changed activity increments `revision`; repeating the same activity is a
no-op. `reset` writes both activities as `inactive` and increments revision
even when already idle.

The CLI discovers the nearest working-directory ancestor containing `.git`
(including worktree files) or `.work`, and confines state below its `.work`.
For an installed `aula-emulator` command or an isolated fixture, explicitly
select the confinement directory:

```sh
aula-emulator status --work-root /absolute/private/state-dir \
  --state-file /absolute/private/state-dir/activity.json
```

State paths must be strictly below the selected root. Symlink escapes and
symlink state/lock files are rejected. The work root and its ancestors must
remain under the caller's control; path confinement does not protect against
a hostile same-user process replacing ancestor directories concurrently.

| Exit | Meaning |
| --- | --- |
| 0 | Command completed (or server stopped with Ctrl-C) |
| 2 | Invalid arguments, work-root discovery, path confinement, or inherited descriptor |
| 3 | Persistence or server I/O failure |

Errors use a concise stderr message rather than a traceback.

## Persistence and events

The activity schema is `synthetic-local-state-v2`, inside a version-2 JSON
envelope. It contains `kind`, `revision`, `recording`, and `streaming`.
Supported v1 boolean snapshots migrate losslessly when read and are written
as v2 on the next mutation: `true` becomes `active`, `false` becomes `inactive`,
and `stream_active` becomes `streaming`. Other formats, including old vendor
snapshots, are rejected.

Mutations lock a persistent sibling `.lock` file, reload the latest snapshot,
and commit using a mode-0600 temporary file, fsync, rename, and directory
fsync. Missing parent directories are created mode 0700. Concurrent CLI and
server processes therefore preserve each other's changes. Do not delete a
lock file while any process uses the state.

An explicit `reset` can replace malformed or unsupported payloads. A fully
validated current or v1 snapshot keeps its revision sequence; an untrustworthy
payload starts at revision 1. Unsafe file types, ownership, and permissions
are still refused. If rename succeeds but directory sync fails, the command
reports an uncertain durable write; the new state is already visible.

`SyntheticRuntime.events` is a bounded, in-process Python library API.
Subscriptions observe commits by that runtime instance, including reset.
Slow subscribers disconnect when their queues fill. Separate CLI processes
do not share event subscriptions; poll `snapshot()` and compare revisions for
cross-process activity changes. LSZ1 event polling is a separate protocol
surface.

The package uses the standard library and setuptools for packaging. There is
no runtime dependency lockfile or required `uv`, pytest, ruff, or pyright
installation.

## Run both host providers

The standalone activation example creates private Unix sockets, passes the
listeners to the two servers, and prints the gateway policy fields:

```sh
PYTHONPATH=lab/emulator/src python3 lab/emulator/examples/host.py \
  --work-root "$PWD/.work/run/emulator"
```

Keep it running while the console connects. Ctrl-C stops both children and
removes only sockets created by this invocation; existing entries are never
overwritten. Activity and settings snapshots remain for the next run.
Use a short, canonical checkout path if the platform's Unix socket path
length limit is exceeded. The gateway's existing ancestor ownership checks
reject sockets below world-writable directories such as `/tmp`.

Merge the printed fields into a normal host gateway policy. They include
`control_socket_path`, `expected_sipd_uid`, `device_socket_path`,
`expected_device_uid`, and `development_device_provider: true`. The gateway
logs development device mode. The device path defaults to
`/run/aula-device/control.sock` and the expected UID defaults to root when
these overrides are omitted. A non-root device UID requires explicit
development mode; peer and filesystem validation still apply.

Change the Room page's synthetic activity from another terminal:

```sh
PYTHONPATH=lab/emulator/src python3 -m aula_emulator record-start \
  --state-file .work/run/emulator/activity.json
PYTHONPATH=lab/emulator/src python3 -m aula_emulator stream-pause \
  --state-file .work/run/emulator/activity.json
```

The device stand-in serves revision-1 status only. Jobs and credential
operations are unsupported. The gateway's configured device endpoint also
applies to its credential requests, so those requests fail while this
provider is selected.

The lower-level `host.py` helper only activates the two providers. Use the
[browser launcher](#browser-console) for nginx, TLS, the FastCGI gateway and
account bootstrap. The emulator produces no RTSP preview or real calls.

## Socket activation and synthetic SIP behavior

An external supervisor may pass a connected or listening `AF_UNIX` descriptor:

```sh
python3 -m aula_emulator serve --fd 3 \
  --state-file .work/run/emulator/activity.json
python3 -m aula_emulator serve-device --fd 3 \
  --state-file .work/run/emulator/activity.json
```

Descriptor 3 must already be open and inherited. The package servers never
bind or unlink socket paths. LSZ1 accepts stream and packet sockets (the
gateway uses `SOCK_SEQPACKET` on Linux and `SOCK_STREAM` on macOS); the device
protocol uses stream sockets.

The LSZ1 stand-in handles all ten opcode slots: status, originate, hangup,
DTMF, media control, event polling, diagnostics, settings, and metrics.
Credential replacement returns the protocol's `UNSUPPORTED` error.
Calls progress through synthetic states as time advances; `--step-seconds`
controls the progression interval. Settings are persisted separately at
`<state-file>.sip-settings`. Calls, event history, and the bounded mutation
replay cache are volatile. Reads are always fresh. Reusing a mutation ID with
the same opcode and exact payload replays the prior result; a different
request returns `REQUEST_ID_CONFLICT`.

Use `--fault persistence-uncertain` to exercise an uncertain settings commit,
`--fault oversized` for a reply exceeding the gateway's reply limit, and
`--reply-delay SECONDS` for timeout behavior. These switches apply to `serve`
only and are synthetic test conditions. See the
[LSZ1 inventory](LSZ1_INVENTORY.md) for exact fields, errors, cache rules, and
the maintained C sources that define the contract.

## Verification

```sh
make emulator-test
make emulator-status
make verify
```

The unit suite covers persistence, concurrent processes, migration/recovery,
CLI behavior, events, framing, daemon state, device projection, and actual
C clients connected to the Python servers by socket pairs. C contract tests
need the same compiler, Jansson, OpenSSL, and `pkg-config` as `make verify`.
The status target uses fresh temporary state and checks mutations and reloads;
it never reads interactive emulator state. CI selects Python 3.11 on Linux
and macOS.

Optional local checks:

```sh
ruff check --isolated --select F,E9,B,PLE lab/emulator
(cd lab/emulator && pyright --pythonversion 3.11 src tests)
```

See [FIDELITY.md](FIDELITY.md) for the limits of this synthetic evidence.
