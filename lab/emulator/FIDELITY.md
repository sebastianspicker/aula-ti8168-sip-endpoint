# Emulator fidelity and evidence matrix

"Full emulator" in this workspace means the complete behavior surface that
can be implemented without inventing undocumented device behavior. It does
not mean full-system TI8168 hardware emulation.

| Surface | Current coverage | Evidence | Confidence | Remaining gap |
| --- | --- | --- | --- | --- |
| Main control framing | F0/FF encode/decode, streaming parser, ACK/NAK/NTFY, invalid NAK | Protocol manual pages 16-18 and 60-61 | High for documented format; not live-verified | Capture accepted/rejected frames from a unit |
| LS-200 command set | All model-applicable commands with documented validation and SET echo | Protocol manual pages 18-54 and LS-200 model tables | High for byte contracts; medium for transitions | Observe state prerequisites, delays, and error cases |
| Key pass-through and legacy events | Not implemented | Protocol manual pages 56-60 | Known missing | Capture whether a deployed LS-200 emits or accepts them |
| Vendor web applications | Extracted login, admin, director, manager, mobile, controller, mini-director, error, and vendor trees served byte-for-byte with nginx-compatible routing | Extracted `/var/www` and nginx configuration | High for static bytes and mapped routing | Runtime behavior still depends on the emulated APIs, events, and media below |
| Factory web state and media assets | Firmware SQL layouts/channels/assets plus contained static `/usr/share/media` files | Extracted `initial.sql`, decompiled serializers, and rootfs media | High for seeded records and file bytes | Hardware capacities and state absent from factory SQL remain deterministic unknowns |
| Web API | Auth, system, recorder, preview, storage, schedule center, asset, channel, layout, audio, network, and conference groups | `evidence/firmware-analysis/decompiled_python/webapi/resources/__init__.py` and resource modules | Medium | Native CBox behavior, uploads, optional routes, and exact uncaptured error edges; cookie mutations require explicit compatibility mode |
| Browser events | Engine.IO 3 direct WebSocket, Socket.IO 1.x `/event`, version response, bounded event bridge | Decompiled event service, bundled client, and nginx Socket.IO proxy | Medium for observed wire contract | Polling transport and hardware-originated device payload captures |
| Native CBox bus | Recovered loopback MQTT endpoint and exact method/response/signal topic grammar; bounded in-process dispatcher; MCU service registration with retained fan/overheat signals; scheduler center `get_center`/`set_center` through its recovered outer `scheduler` method | `libcbox_bus.so.1.0.14`, Mosquitto init/config, native and Python callers | High for transport/topic grammar, MCU method names, and this scheduler center request shape; payload-specific confidence varies | Other scheduler methods and service bodies, broker timing, disconnect/retry behavior, and live cross-process delivery |
| Persistence | Versioned JSON snapshot of shared logical device state | Emulator design, not firmware parity | High for local implementation | Web-only layout/channel/config objects are currently session-local |
| Media | Synthetic H.264 High 1920x1080 at `1000/33` fps with AAC-LC 48 kHz stereo on `movie`, a loopback `channel1` relay, and RTMP/HLS lab relays. MediaMTX is limited to two configured `movie` readers. | Current live RTSP capture and control contract; MediaMTX/FFmpeg development configuration | High for observed metadata/control shape; medium as a test source; low as device parity | Host FFmpeg output is deterministic synthetic media, not appliance encoder/NAL payload parity; firmware HTTP-FLV, timing jitter, errors, audio routing, and AEC remain unmodelled |
| ARM userspace | Offline ARMv7 preflight, structural rootfs contract checks (ELF, symlinks, metadata, recovered-source AST, and nginx text), plus an opt-in dynamic-loader BusyBox-help probe in a fixed read-only Docker lane | Extracted ARM EABI rootfs and recovered Python source | High boundary confidence; no appliance behavior claim | A preloaded Linux ARMv7 Docker image for carefully selected inert utilities, before any broader userspace experiment |
| Full-system TI8168 hardware | A bounded `ti8168-ls200` QEMU machine boots both recovered Linux/UBIFS slots, recovered nginx, and the maintained overlay | Local kernel/module/PCB evidence plus `lab/qemu/README.md` and `lab/qemu/FIDELITY.md` | High for the recorded Linux, NAND, web, and maintained-overlay paths; not appliance parity | Model the absent M3/DSP, SysLink, capture, display, audio, USB, SATA, and vendor-media behavior only where new evidence supports it |
| TI coprocessor lifecycle | Logical three-core state model with ordered load/start/stop callback traces, immutable recovered-region boundary lookup, synchronous in-process Notify-shaped callback dispatch, and a FIFO-only MessageQ/TransportShm substitute. The latter composes logical IDs as `(owner slot << 16) | queue index`, accepts immutable byte payloads with local lengths, requires registered ready targets, and emits the recovered TransportShm Notify tuple: line `0`, event `0xC1D20002`, payload `0`, wait-clear `false`. | TI init/load scripts, `fw_load.out`, `syslink.ko`, M3 shared-memory reports, recovered `MessageQ_create`, `MessageQ_setReplyQueue`, and `TransportShm_put` paths | High for lifecycle ordering, callback values, named regions, logical ID bit composition, and local model determinism; medium for the recovered Notify call tuple | Numeric real processor IDs, application queue names, ioctl devices/structs, shared-memory ownership, MMU/cache behavior, native MessageQ priorities/blocking/allocation, Notify transport semantics, and DSP/M3 product payloads |
| MCU management | Integrated logical register model and in-process CBox facade for the recovered RPC names; retained fan/overheat status | `libcbox_mcu_ops.so`, MCU daemon, init/update/shutdown scripts | High for I2C host contract, reached registers, method names, and bounded status behavior | MCU firmware internals, board GPIO mapping, provisional event IDs, electrical timing, and live I2C behavior |
| Boot/update | Logical AREC1 header/MD5 and core A/B update path; four descriptors include an explicitly unsupported external application-bundle handoff | Init scripts, `fw_update` decompilation, and recovered AREC1 descriptors | High for statically reached header checks, descriptor MD5 loop, core image order, and `fwindex` branch | External application-bundle install semantics, bootloader policy, live partition geometry, power-loss traces, physical NAND faults |
| Zoom/SIP | Not implemented | No confirmed product Zoom client or SIP callsite in analyzed native exports or corpus-level name/string scans | Medium negative finding, pending full library/extension semantic review | Finish native semantic review; external gateway prototype and live bidirectional media tests |

## Deliberate logical substitutions

- Camera, playback, USB backup, snapshot, and bookmark commands record intent
  or counters only.
- Network commands use RFC 5737 documentation addresses and never inspect or
  alter the host network.
- `SU` stores opaque URL bytes and never connects to the URL.
- Web network restart returns the firmware-compatible accepted response but
  starts no service.
- Recorder actions change logical state but do not write a recording.
- Power-off and standby are state gates, not host power operations.

## Explicit exclusions

- No native ARM executable is run by default, including `make verify`.
- The optional ARMv7 lab is an exception only for `/bin/busybox --help` through
  the extracted dynamic loader. It requires an explicit `--execute`, keeps the
  evidence rootfs read-only, has no network or host-device access in its Docker
  lane, and never invokes init/`rcS`, services, CBox, media, MCU, update,
  flash, storage, or kernel-module programs.
- Native `qemu-arm` and `qemu-arm-static` are not execution lanes: `-L` alone
  cannot prove a read-only rootfs or deny host networking.
- No firmware image is repacked or flashed.
- No vendor TLS key or embedded credential is copied.
- No service binds beyond loopback.
- No assumption is made that the LS-200 can establish SIP or join Zoom.
- The CBox MQTT model does not open a broker connection unless a later,
  explicitly bounded integration opts in; it currently represents the proven
  topic and retained-message semantics only.
- MCU and SysLink models must remain logical state machines. The local
  MessageQ/TransportShm substitute preserves only the stated queue-ID layout,
  FIFO, byte-length, readiness, and Notify-call observations; it does not
  claim numeric processor IDs, application queue names, Linux ioctl ABI,
  device structures, native MessageQ or Notify transport semantics, product
  payloads, peripheral timing, DMA, MMU, cache, or electrical parity.

## Acceptance gates

`make verify` is the deterministic local gate. Optional gates are:

- `make compose-check` with a working Docker engine
- `make media-up && make media-check` for a live synthetic stream
- `LS200_EMULATOR_ADMIN_PASSWORD=dev-secret make qa-vendor-ui` while an
  emulator using that password is running
- later hardware transcript comparisons on an isolated owned unit

Local tests demonstrate emulator consistency. They do not demonstrate native
firmware parity, physical media behavior, or conferencing interoperability.

The separate full-system research track is documented in the
[`lab/qemu/README.md`](../qemu/README.md). Its public dependencies and known
missing device models are catalogued in
`evidence/firmware-analysis/reverse_engineering/PUBLIC_HARDWARE_EMULATION_RESOURCES.md` (in the private corpus).
Planning those phases does not raise any row's current fidelity rating.
