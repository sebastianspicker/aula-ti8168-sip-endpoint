# Generated Cortex-A8 execution probe

This project-owned assembly program is a synthetic bare-metal guest for the
maintained `ti8168-mediaboard` QEMU machine. It contains no firmware, vendor
code, operating system, filesystem, or NAND image. A successful run proves the
specific CPU/UART/timer/IRQ path below. It does not establish Linux boot or
physical TI8168 board compatibility.

The guest starts at RAM address `0x80000000`, installs an eight-entry ARM vector
table through VBAR, creates supervisor and IRQ stacks, and initializes UART1
(`0x48020000`, four-byte register spacing). It writes the first marker through
UART MMIO. It selects the 27 MHz CLKIN source through PRCM's TIMER1 clock
selector and enables that module before accessing its registers. It then
routes DMTIMER1 (`0x4802e000`) overflow to INTC IRQ67, enables IRQ exceptions,
and starts a one-shot counter near overflow.

The actual CPU IRQ handler checks SIR_IRQ=67, checks overflow status, verifies
that one-shot overflow cleared TCLR.ST, clears the timer's status through its
write-one-to-clear register, verifies the clear, and writes INTC.CONTROL's
NEWIRQAGR acknowledgement. A second timer start must enter the same handler
again. The main path then checks the timer is stopped and both the timer status
and INTC pending source are clear. The guest never uses an INTC software
interrupt to simulate timer delivery.

UART output must consist of exactly these four lines in order:

```text
AULA:GUEST:CPU_UART
AULA:GUEST:TIMER_IRQ_1_CLEARED_ACKED
AULA:GUEST:TIMER_IRQ_2_CLEARED_ACKED
AULA:GUEST:PASS
```

Only after these checks does the guest issue ARM semihosting
`SYS_EXIT_EXTENDED` with status zero. Failed guest assertions write
`AULA:GUEST:FAIL` and exit with status one. Missing interrupts or unavailable
semihosting cause the host timeout to fail the probe. A UART marker alone is
insufficient: the runner requires all four ordered markers and process exit
zero. Execution uses TCG instruction counting, never the qtest accelerator. The
runner disables networking with `-nic none`.

## Requirements and execution

Use Python 3.11+, the locally prepared/pinned QEMU binary with this repository's
current model patch, and GNU ARM binutils (`arm-none-eabi-as` and
`arm-none-eabi-ld`). The host's native Apple assembler/linker are unsuitable.
No tool is installed or downloaded. Missing tools fail with a nonzero status;
there is no skipped or simulated success.

From the repository root:

```sh
python3 lab/qemu/scripts/probe_guest.py \
  .work/build/qemu/build-aula-v11.0.3/qemu-system-arm
```

Supply absolute `--assembler /path/to/arm-none-eabi-as` and
`--linker /path/to/arm-none-eabi-ld` when the tools are absent from PATH.
`--timeout SECONDS` sets the execution deadline (default 15; maximum 60).
`--output-dir DIRECTORY` selects the parent evidence directory; the default is
`.work/build/qemu/guest-probes`. Each invocation creates and retains a unique
`guest-*` directory, including on a build or execution failure.

To build and inspect the ELF without running QEMU:

```sh
python3 lab/qemu/guest/build_guest.py --output-dir .work/build/qemu/guest-inspect
arm-none-eabi-readelf -h -l .work/build/qemu/guest-inspect/guest.elf
arm-none-eabi-objdump -d .work/build/qemu/guest-inspect/guest.elf
```

The ELF is little-endian ARM32 EABI, with `_start` at `0x80000000`. A linker
assertion limits the guest, its stacks, and its BSS to the first 64 KiB of RAM.
The RAM segment is intentionally executable and writable for this MMU-disabled
probe. The runtime does not require a C library or a cross C compiler.

## Evidence and limits

The retained artifacts are `guest.elf`, `guest.o`, `guest.map`, `build.json`,
`build.stdout.log`, `build.stderr.log`, `uart.log`, `qemu.stderr.log`, and
`probe.json` (the last is written for failed probes too). `build.json` records
source hashes, tool binary hashes, exact commands, and tool version output.
`probe.json` records the QEMU binary path/hash/version, exact invocation,
return status, output hashes, and the checked markers. A failure before a
particular build/run step may leave only the artifacts produced so far.

These records identify the binaries exercised; they do not independently attest
how a caller-supplied QEMU binary was built. Pair them with the maintained QEMU
source verification and build logs. The probe validates TIMER1 clock selection
and module enable, one-shot operation, repeated interrupt delivery and software
clear/acknowledgement. It does not validate the full clock tree, timing accuracy, cache coherency,
DMA, networking, storage, media, or a physical interrupt controller.

The separate [legacy SIP fixture](LEGACY_SIP_SMOKE.md) is a Linux ARM program
for the original-kernel rescue lane. It exercises the production daemon's
authorized LSZ1 control path and a real loopback SIP dialog.
