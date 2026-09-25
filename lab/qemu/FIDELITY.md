# QEMU model fidelity and evidence matrix

This is the current statement of what the `ti8168-ls200` QEMU model boots and
emulates, and what it does not. Each claim names the script or test that
supports it.

| Surface | Current coverage | Evidence | Confidence | Remaining gap |
| --- | --- | --- | --- | --- |
| Recovered Linux boot, both slots | The machine boots the recovered Linux 2.6.37 kernel from MTD3 (slot 1) or MTD5 (slot 2), attaches the full nine-partition recovered NAND map, and mounts the recovered UBIFS root from MTD4 or MTD6 as `ubi0:rootfs` to an interactive BusyBox shell | `scripts/run-ls200.sh`, `scripts/verify.sh --require-source`, local boot runs | High for the recorded boot path | AHCI/SATA, I2C, USB, audio, and other peripherals stay unmodelled; boot continues past their expected warnings without claiming support for them |
| NAND and storage | The recovered nine-MTD map (see [README.md](README.md#recovered-nand-map)) is backed by real main+OOB dump pairs; web mode adds a volatile copy-on-write view and a verified JFFS2 create/sync/read/delete cycle on MTD7/8 | `hw/arm/ti8168_nand.c`, the NAND COW qtests, local web-mode boot runs | High for the recorded geometry and COW semantics | The opt-in single-file `nand-profile=synthetic-read-only` GPMC/NAND transport still advertises an obsolete ONFI-style compatibility ID (`0xe1 0xf0 0xa5 0x00`) and a synthesized Linux-software-ECC OOB shape, not the live device's Micron ID (`0x2c 0xdc`) or its BCH8-style OOB layout; that transport has never itself reached MTD registration, UBI attach, or a UBIFS mount |
| Kernel image reconstruction | The recovered uImage/zImage's embedded LZMA stream decompresses byte-identical to both the independently extracted kernel and the reconstructed ELF's `.kernel` section | `scripts/verify-zimage.py`, SHA-256 comparison against the retained kernel inputs | High for placement, consumed-input range, and decompressed-output identity | The decompressor's exact stack low-water mark is a static bound, not an instruction-level trace |
| Boot-loader and placement mechanics | Seven bounded, independently launched experiments (uImage control, QEMU raw zImage load, direct raw placement, board-owned stub, cache/TLB-maintenance stub, split-RAM aliasing, and a relocation-breakpoint observation) all reach the identical entry state and first-fault signature | `fixtures/boot-matrix.json`, `scripts/run-boot-matrix.py`, `tests/test_boot_matrix.py`, `tests/test_boot_matrix_qemu.py` | High that QEMU's loader, relocation, and decompression path is not the source of any remaining boundary | First-fault capture needs a diagnostic-instrumented QEMU build that is no longer part of the maintained tree; matrix runs against the maintained binary currently prove entry state only (`--entry-only`) |
| TI8168 compatibility surface (mux/FAPLL, EDMA, PCIe, McSPI, ELM, CONTROL pads, SRAM, GPTIMER, device ID) | Modelled in the maintained patch; device ID, the proven SRAM slice, the ELM aperture, GPTIMER frequency, and the CONTROL pad allowlist are each qtested at their exact reached offsets | `hw/arm/ti8168_*.c`, `tests/qtest/ti8168-*-test.c` | High for the exact reached registers and values | No broader register map, reset-default, DMA/IRQ, or electrical claim; blocks and offsets outside the recovered boot path are not modelled |
| Maintained Zoom/SIP overlay | The recovered vendor filesystem startup is followed by the maintained nginx, FastCGI gateway, SIP daemon, console, private SIP/RTSP fixtures, and fake renderer; both slots independently pass the bootstrap/login/call/DTMF/hangup flow with bidirectional RTP and outbound RTCP, and prove settings-revision persistence across a service restart | `tests/test_qemu_zoom_stack.py`, `tests/test_qemu_zoom_two_slots.py` | High for the recorded end-to-end flow | Development QEMU certificate, private fixtures, and deterministic synthetic media only; no trusted production TLS, public Zoom CRC interoperability, vendor audio/HDMI, echo-cancellation calibration, or live soak/resource behavior |
| Full-system appliance parity | Not claimed | n/a | n/a | M3/DSP, SysLink, capture/display, audio, USB, SATA, and all vendor-media behavior stay outside this model; `lab/emulator/FIDELITY.md` documents the separate logical-emulation lane that covers some of that surface without hardware parity either |

## Tool status

Maintained tools, exercised by tests or `make qemu-verify`:

- `scripts/verify-zimage.py` (kernel-image reconstruction check)
- `scripts/run-boot-matrix.py` and `fixtures/boot-matrix.json` (loader/placement
  matrix)
- `scripts/extract-ti816x-mux.py` (recovered CONTROL pad/mux inventory)
- `storage/ubi_data_area.py` (disposable synthetic UBI data-area copies)
- `initramfs/pack_rescue_initramfs.py` and `initramfs/run_rescue_initramfs.py`
  (synthetic rescue archive)

## Acceptance gates

`make qemu-verify` is the deterministic local gate for the patch stack,
launchers, and built-machine qtests. `make qemu-acceptance` additionally
builds and runs the two-slot maintained-overlay acceptance harness. Both
require a locally built QEMU binary; see [README.md](README.md) for the build
prerequisites.
