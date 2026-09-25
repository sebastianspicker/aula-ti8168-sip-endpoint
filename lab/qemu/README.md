# LS-200 QEMU model

This directory builds QEMU's `ti8168-ls200` machine and starts recovered
AREC LS-200 firmware from the full recovered NAND layout. It is a bounded
recovered-Linux development environment, not a complete appliance model.

## Build, verify, and boot

```sh
make qemu-build
make qemu-verify
make qemu-run
```

The default machine binary is
`.work/build/qemu/build-ls200-v11.0.3/qemu-system-arm`. Kernel, entry-state,
first-fault, boot-matrix, and rescue launchers use that same build by default.
`scripts/verify-zimage.py` reads retained kernel inputs below
`evidence/firmware-analysis/` and the current reconstructed ELF below
`.work/firmware-analysis/reverse_engineering/work/kernel_elf_reconstruction/`.

The default command boots recovered slot 1, runs Linux 2.6.37 with the
recovered MTD4 UBIFS root, and opens an interactive BusyBox shell. Use
`LS200_FIRMWARE_SLOT=2` for the recovered slot-2 kernel and MTD6 root, or
`LS200_BOOT_MODE=vendor` to run the vendor init path. `LS200_DUMP_DIR` can
select another directory containing the same eighteen MTD main/OOB files.

Useful launch choices:

```sh
# Start the recovered web path on the loopback-only default port 8080.
LS200_BOOT_MODE=web make qemu-run

# Start recovered slot 2 in web mode.
LS200_FIRMWARE_SLOT=2 LS200_BOOT_MODE=web make qemu-run

# Choose a different local HTTP port.
LS200_BOOT_MODE=web LS200_WEB_PORT=18080 make qemu-run

# Capture the guest console without attaching it to the terminal.
LS200_CONSOLE_LOG="$PWD/.work/console.log" make qemu-run

# Add debugger and QMP endpoints when diagnosing the guest.
LS200_GDB_PORT=12345 LS200_QMP_SOCKET="$PWD/.work/ls200.qmp" make qemu-run
```

## Recovered NAND map

All nine MTD regions are present at their recovered offsets. Each requires a
main-area dump and OOB sidecar; together they cover the recovered 512 MiB NAND
address space.

| MTD | Offset | Size | Main image | OOB image |
| --- | ---: | ---: | --- | --- |
| 0 | `0x00000000` | 1 MiB | `mtd0-U-Boot.bin` | `mtd0.oob` |
| 1 | `0x00100000` | 256 KiB | `mtd1-U-Boot-Env.bin` | `mtd1.oob` |
| 2 | `0x00140000` | 1 MiB | `mtd2-U-Boot-Logo.bin` | `mtd2.oob` |
| 3 | `0x00240000` | 4 MiB | `mtd3-Kernel-1.bin` | `mtd3.oob` |
| 4 | `0x00640000` | 220 MiB | `mtd4-File-System-1.bin` | `mtd4.oob` |
| 5 | `0x0e240000` | 4 MiB | `mtd5-Kernel-2.bin` | `mtd5.oob` |
| 6 | `0x0e640000` | 220 MiB | `mtd6-File-System-2.bin` | `mtd6.oob` |
| 7 | `0x1c240000` | 20 MiB | `mtd7-System-Data.bin` | `mtd7.oob` |
| 8 | `0x1d640000` | 41.75 MiB | `mtd8-User-Data.bin` | `mtd8.oob` |

Slot 1 loads MTD3 and mounts MTD4 with the recovered UBI layout. Slot 2 loads
MTD5 and mounts MTD6. The current fidelity statement is in
[FIDELITY.md](FIDELITY.md).

## Web and Linux development

Web mode boots the selected recovered kernel and UBIFS root, enables a volatile
copy-on-write view of the full NAND mapping, starts the vendor filesystem and
patch stages, configures the native TI8168 DaVinci EMAC through QEMU user-mode
networking, and starts recovered nginx. The host forwards guest port 80 to
`http://127.0.0.1:8080/` by default.

The default `LS200_WEB_STORAGE=nand` path mounts recovered MTD7 and MTD8 JFFS2
filesystems read-write. Their verified create, `sync`, read, and delete cycle
remains volatile. `LS200_WEB_STORAGE=tmpfs` is available when only `/var/log`
and `/var/lib/cbox` need temporary storage.

Create an editable copy of the recovered web tree and deploy it as guest
`/var/www` on each web boot:

```sh
sh scripts/stage-web-root.sh
LS200_BOOT_MODE=web \
LS200_WEB_ROOT="$PWD/.work/web-root" \
  sh scripts/run-ls200.sh
```

`LS200_WEB_ROOT` is the contents of guest `/var/www`; edit that host directory,
restart the prototype, and reload the browser. The launcher archives it and the
guest obtains it through the private TFTP service before nginx starts.

For Linux-side changes, provide a sparse tree with paths relative to guest `/`:

```sh
mkdir -p .work/root-overlay/etc .work/root-overlay/usr/bin
cp path/to/example.conf .work/root-overlay/etc/
cp path/to/example-tool .work/root-overlay/usr/bin/
LS200_BOOT_MODE=web \
LS200_ROOT_OVERLAY="$PWD/.work/root-overlay" \
  sh scripts/run-ls200.sh
```

`LS200_ROOT_OVERLAY` is likewise archived and transferred through TFTP. The
guest remounts its UBIFS root read-write, extracts the archive at `/`, and then
starts nginx. Both root and web overlay markers have been read successfully in
the guest; the root mount was reported as `ubifs (rw)`.

Slot 1 has a verified native EMAC/slirp ping to `10.0.2.2` (1/1), with RX 10,
TX 9, and zero errors. Its recovered nginx 1.8.0 service returned a
host-forwarded HTTP 200 response with `Content-Length: 2862`. Slot 2 web mode
also mounts MTD7/8, starts recovered nginx, and returns HTTP/1.1 200 (3250
response bytes).

The recovered `webapi-server`, API, and media services still require unavailable
M3, DSP, media, and `pycbox` components. Recovered `/api` may therefore return
`502`. The isolated maintained overlay does not depend on that path: its
explicit QEMU profile starts the HTTPS console, FastCGI gateway, SIP daemon,
private SIP/RTSP fixtures, and fake audio/HDMI renderer. Both recovered slots
have passed the bootstrap/login/call/DTMF/hangup flow with bidirectional RTP
and outbound RTCP. This mode does not claim vendor-media or physical-device
parity.

## Machine and launcher structure

The ordered QEMU patch stack produces focused source modules:

- `ti8168_ls200.c` composes the board and boot configuration.
- `ti8168_intc.c`, `ti8168_timer.c`, `ti8168_nand.c`, and `ti8168_gpmc.c`
  implement the interrupt, timer, NAND, and GPMC contracts.
- `ti8168_emac.c` provides the bounded DaVinci EMAC, MDIO, CPDMA, and PHY path.
- `ti8168_bootstrap.c` and `ti8168_probe_regs.c` contain narrow boot and
  compatibility-register contracts.
- `ti8168_ls200.h` is the internal module interface.

The final quality refactor shares private declarations through
`ti8168_internal.h` and keeps the larger EMAC and NAND register/device sections
in owned `.inc` units. Volatile NAND COW pages materialize their main and OOB
bytes from immutable backing on ordinary program operations. A newly erased
page starts entirely at `0xff`; erasing one block leaves adjacent backing
blocks unchanged.

The EMAC model resets RXMAXLEN to 1518 and enforces it against packet bytes
supplied by QEMU's network backend. With RXCEFEN clear, oversized frames leave
the owned RX descriptor and head pointer unchanged. With RXCEFEN set, transfer
is capped at RXMAXLEN and descriptor capacity, and completion reports OVERSIZE.
Synthetic socket tests cover both modes and the exact-size boundary. Backend
packets omit physical Ethernet FCS, so these checks do not establish wire-level
or physical-device parity.

`scripts/run-ls200.sh` selects shell, vendor, or web mode. Its
`scripts/lib/web-boot.sh` helper builds web-mode networking, storage, and
overlay commands; `scripts/run_web_console.py` injects that command after the
guest finishes booting; and `scripts/stage-web-root.sh` creates an editable web
tree. These modules support the verified path above without representing all
LS-200 hardware.

## Current boundary and verification

Boot continues after expected warnings from incomplete clocks and unmodelled
AHCI/SATA, I2C, USB, audio, and other peripherals. Those warnings are expected;
they are not a claim that the affected hardware is supported.

Run the local gate after source or launcher changes:

```sh
make qemu-verify
```

It checks the ordered modular patch stack, shell/Python syntax, source quality,
boot-matrix checks, deployment and fixture contracts, all MTD main/OOB
mappings, and built-machine qtests when the QEMU binary is supplied. The
maintained `scripts/probe_synthetic_machine.py` lane creates sparse MTD7
main/OOB fixtures below `.work/build`, runs base-register and NAND COW qtests,
restarts the machine to prove that mutations are volatile, and never loads a
kernel or recovered image. The recovered-data NAND oracle separately pins
owned main/OOB file descriptors, verifies exact geometry and identity around
each bounded offset read, rejects replacement, mutation, short, and out-of-range
reads, and closes both descriptors deterministically.

Preparation and builds compare the complete source tree against the pinned base
plus every ordered patch using an isolated Git index under `.work`. Unrelated
edits, missing patch changes, and unexpected untracked source files fail before
configuration. The checkout's real index is preserved. Ninja and QEMU's build
dependencies must already be prepared; configuration disables downloads.

The per-slot full-service harness is `tests/test_qemu_zoom_stack.py`; it
requires
a reviewed ARM payload staged through the deployment kit. The reusable
acceptance entry point is `tests/test_qemu_zoom_two_slots.py`. It runs slot 1
and slot 2 serially with disjoint host ports, rejects partial or mismatched
evidence, and emits one `ls200-qemu-two-slot-v1` JSON result only after both
pass:

```sh
make qemu-acceptance \
  SIPD_BINARY=/approved/ls200-sipd \
  GATEWAY_BINARY=/approved/ls200-gateway-fcgi \
  NGINX_BINARY=/approved/ls200-nginx \
  ATOMIC_REPLACE_BINARY=/approved/ls200-atomic-replace \
  MIME_TYPES=/approved/nginx-mime.types \
  FASTCGI_PARAMS=/approved/nginx-fastcgi_params \
  QEMU_ENTROPY_HELPER=/approved/ls200-qemu-entropy-seed
```
