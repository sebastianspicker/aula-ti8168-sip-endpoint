# Owned original-kernel SIP fixture

`legacy_sip_smoke.c` tests the real PJSIP-linked daemon entirely on guest
loopback. It is a Linux ARM test program, separate from the production
daemon and from board-media acceptance.

Run it as root in an isolated rescue guest with the PJSIP-enabled owned
daemon installed at `/opt/aula/aula-sipd`:

```sh
/bin/busybox ifconfig lo up
/opt/aula/legacy-sip-smoke --self-test
/opt/aula/legacy-sip-smoke
```

`--daemon /absolute/path/to/aula-sipd` selects another explicitly supplied
owned daemon. The fixture creates its own `/tmp/aula-sip-smoke-*` directory,
root-readable configuration, empty fixture inputs and daemon log. It binds
one UDP SIP peer to `127.0.0.1` on an ephemeral port. All configured SIP,
control and advertised media addresses are local; media is answered inactive.

Each LSZ1 exchange forks a client that clears supplementary groups and uses
UID 10001 with GID 0. The production `SO_PEERCRED` check remains active and
the daemon's socket remains mode 0660. No account database changes are needed.
The fixture requires root's ability to set those child identities; it does
not alter the parent's identity or any other process.

Success requires an initially idle daemon, an accepted originate request,
one correlated INVITE/200/ACK and established call status, an accepted hangup,
one correlated BYE/200 and terminated status, exactly one of each request,
and a clean daemon exit after SIGTERM. Header identities, source tuple,
CSeq and LSZ1 framing/request IDs are checked. Startup, control exchanges,
dialog progress and shutdown have bounded waits. Only children created by
the fixture are signaled. Successful runs remove their temporary files;
failed runs retain their own diagnostic directory.

Exit 0 prints one bounded record containing:

```json
{"marker":"AULA_GUEST_SIP_PASS","invite":1,"ack":1,"bye":1,"initial":"idle","established":"established","final":"terminated","daemon_exit":0,"media_io":false}
```

Failures return nonzero and print `AULA_GUEST_SIP_FAIL` with the failed stage
and counters. No SIP bodies, credentials or media are printed.

`--self-test` runs bounded protocol checks without starting a daemon or
changing identity. It rejects false/malformed mutation replies, extra JSON
members, embedded NULs, wrong response flags/request IDs, invalid SIP versions,
extra request-line tokens and non-SIP request URIs. The success marker is
`AULA_GUEST_SIP_SELF_TEST_PASS`.

## Build and userspace validation

Use a compiler wrapper produced by the authenticated public legacy ABI lane
in [LEGACY.md](../../arm-userspace/LEGACY.md). For example, after that lane
has created `.work/legacy/run-001/toolchain/`:

```sh
LEGACY_CROSS_PREFIX="$PWD/.work/legacy/run-001/toolchain/arm-linux-gnueabi-"
mkdir -p .work/guest
"${LEGACY_CROSS_PREFIX}gcc" \
  -std=c99 -Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow -Wformat=2 \
  -O2 -march=armv7-a -mtune=cortex-a8 -mfloat-abi=soft \
  lab/qemu/guest/legacy_sip_smoke.c -o .work/guest/legacy-sip-smoke -lrt
```

The fixture directly needs `libc.so.6` and `librt.so.1`; the legacy runtime
also supplies their loader/pthread closure. The PJSIP daemon additionally
needs its separately verified `libm` and resolver closure. Check the actual
ELF dependencies and hashes when assembling a rescue image.

Before original-kernel execution, the same ARM fixture can run through
QEMU user mode against a read-only private legacy runtime:

```sh
qemu-arm -cpu cortex-a8 -L /private-runtime .work/guest/legacy-sip-smoke \
  --daemon /owned-build/aula-sipd \
  --qemu-arm /usr/bin/qemu-arm --runtime-root /private-runtime
```

The optional launcher arguments affect only how the owned daemon is executed;
omit them inside the ARM guest. Use a networkless disposable container with
a read-only root/runtime and only `SETUID`/`SETGID` capabilities added to the
otherwise dropped capability set. A daemon built without PJSIP must fail
this test at startup, providing a useful negative control.

The fixture proves the tested signaling/control path. Inactive media and
empty fixture inputs do not validate codecs, capture, DSP, output devices,
TLS/SRTP, registration or provider interoperability.
