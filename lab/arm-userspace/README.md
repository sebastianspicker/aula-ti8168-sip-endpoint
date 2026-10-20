# ARMv7 application execution

This optional lane compiles the maintained `product/sipd` C application and
runs it through `qemu-arm -cpu cortex-a8`. It verifies real ARM instructions,
32-bit data layout, the Linux userspace control/configuration/SDP code, and
the daemon's basic platform and call-state checks. The Python emulator is
not involved.

It does **not** establish TI8168 board fidelity. User-mode QEMU translates
application system calls onto its host kernel; it does not run the board's
Linux 2.6.37 kernel. A modern ARM libc also does not prove compatibility with
the board's glibc 2.12 ceiling. DSP, NAND, capture, audio/display output and
timing remain outside this lane. PJSIP, native AAC/AEC and SRTP integrations
are disabled by default; the fixture call has no SIP or media I/O. The optional
PJSIP mode below adds a real UDP signaling check.

The separate [legacy compatibility lane](LEGACY.md) builds against authenticated
public older development inputs and executes with a supplied private glibc
2.12.2 runtime. Its owned ELF files can also be selected by the
[original-kernel rescue runner](../qemu/firmware/README.md).

## Explicit inputs

Run inside a disposable Linux development environment with already acquired
public tools:

- Soft-float ARM Linux EABI GCC, binutils, C development headers and runtime.
  The compiler target must end in `-linux-gnueabi`; `gnueabihf`, AArch64 and
  host compilers are rejected.
- `qemu-arm` with Cortex-A8 support.
- CMake/CTest 3.21 or newer and a build tool supported by CMake.
- Python 3.10 or newer.

For example, Debian's public `gcc-arm-linux-gnueabi`,
`binutils-arm-linux-gnueabi`, `libc6-dev-armel-cross`, `qemu-user`, `cmake`
and `make` packages provide those development inputs. Installing them is an
explicit preparation step; the runner never downloads or installs anything.
Their distribution versions are development inputs and do not satisfy the
product's separate legacy ABI release gate.

From the repository root in that Linux environment:

```sh
python3 lab/arm-userspace/run.py \
  --cross-prefix /usr/bin/arm-linux-gnueabi- \
  --qemu-arm /usr/bin/qemu-arm \
  --sysroot /usr/arm-linux-gnueabi \
  --output "$PWD/.work/arm-userspace/run-001"
```

`--sysroot` is the ARM runtime tree passed to QEMU's `-L`. GCC retains its
configured compiler sysroot by default: Debian cross-GCC uses `/`, while
its target headers and linker scripts live below `/usr/arm-linux-gnueabi`.
For a standalone toolchain requiring an explicit compiler root, also supply
`--compiler-sysroot /path/to/its/sysroot`. The output directory must be new
and below this checkout's `.work`; existing results are never overwritten.
Missing tools, invalid ELF metadata, missing tests, host execution commands
or any failing check fail the run. There is no host execution fallback.

## Checks and evidence

The runner enables `AULA_SIPD_ENABLE_CROSS_TESTS` and supplies an explicit
`CMAKE_CROSSCOMPILING_EMULATOR`. Default host and production cross builds
retain their existing behavior. Cross builds without this option continue
to omit tests; enabling it without an emulator is a configuration error.

It inspects the real daemon and seven standalone C test executables for
ELF32, little-endian ARM EABI5, ARMv7 CPU attributes, a soft-float calling
convention and `/lib/ld-linux.so.3`. It checks the exact eight-test CTest
inventory and verifies every test command begins with the required QEMU
command, then runs:

- `test_control_frame`, `test_control_protocol`, `test_config_diagnostics`,
  `test_config_parse`, `test_request_decode`, `test_sdp`, `test_pjsip_options`;
- `check_config_explains_rejection`;
- `aula-sipd --self-test` and `aula-sipd --fixture-call`.

`report.json` contains success/failure, tool paths, executable hashes and
version output, compiler target/root, commands and exit codes, artifact
hashes, completed checks and explicit limits. `source-manifest.json` hashes
the maintained SIP source tree; `runtime-manifest.json` records runtime
files and symlink destinations. Both are checked again after execution.
Command logs preserve build and ELF diagnostics; `ctest.xml` records test
results. These identify the supplied local inputs; they do not establish
upstream provenance, redistribution rights or a reproducible toolchain
closure. Keep package/source acquisition receipts alongside the report.

The runner executes caller-supplied tools and test binaries. Use a disposable
container or VM; it is not itself an isolation boundary. It never invokes
physical deployment, private sessions or board firmware.

## Optional actual SIP daemon call

Supply the already acquired pjproject archive identified by
`dependencies/dependency-lock.json` to add a real UDP SIP dialog:

```sh
python3 lab/arm-userspace/run.py \
  --cross-prefix /usr/bin/arm-linux-gnueabi- \
  --qemu-arm /usr/bin/qemu-arm \
  --sysroot /usr/arm-linux-gnueabi \
  --pjsip-archive /inputs/pjproject-b7317ffcaf1d20a38eb231cf15b7b65c4734dbb4.tar.gz \
  --output "$PWD/.work/arm-userspace/sip-001"
```

This mode additionally needs the matching ARM G++ compiler, `patch` and
`shasum` (Debian's `g++-arm-linux-gnueabi` and `libdigest-sha-perl`
packages). Upstream configure probes G++ even though the reviewed patch
removes the unused C++ object from the signaling archives.
It verifies the locked source digest, checks
archive member paths/types, applies the existing digest-gated signaling-only
patch, and cross-builds the minimal PJSIP archives with TLS disabled. Nothing
is downloaded. The report records the lock, patch, preparer and archive hashes
and the built static-library hashes.

After the same eight CTests and daemon checks, `sip_smoke.py` starts the real
ARM daemon under QEMU, connects through its production LSZ1 `SOCK_SEQPACKET`
control socket, originates a private-lab meeting, waits for a correlated SIP
INVITE/200/ACK and established call state, requests hangup, and checks the
peer's BYE and clean daemon shutdown. The existing protocol-neutral
`lab/sip-peer` implementation is bound to loopback only. The daemon's normal
peer-UID authorization stays enabled. In a root-run disposable container the
client runs with UID 10001; otherwise it uses the caller's non-nobody UID.

`sip-evidence.json` records status snapshots, bounded peer counters, helper
hashes and shutdown status. The associated daemon log contains the public,
synthetic loopback identities used by this test. This mode verifies actual
PJSIP signaling and the daemon control path; it does not validate TLS,
registration, media I/O, provider interoperability or board hardware. The
peer answers with inactive media, so no SIP signaling success is reported as
a media pass.

The runner's failure-path checks run independently of ARM tools:

```sh
python3 -m unittest discover -s lab/arm-userspace/tests
```
