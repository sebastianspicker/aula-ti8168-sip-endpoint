# Synthetic TI8168 media board model

`ti8168-mediaboard` is a partial QEMU model of TI8168 hardware. It implements
selected interrupt, timer, PRCM, GPIO, watchdog and Ethernet contracts from
TI's public manual. NAND geometry, PHY addresses, bootstrap storage and boot
identity use configurable synthetic defaults. The [fidelity matrix](FIDELITY.md)
lists tested behavior and missing hardware; the
[provenance record](PROVENANCE.md) identifies sources and assumptions.

UART and channel-zero Ethernet can carry guest traffic. EDMA, PCIESS and
McSPI provide bootstrap register storage only. Several other peripherals
are placeholders.

NAND has two 256 MiB synthetic banks. Each accepts a caller-supplied main
image and OOB image of exactly 256 MiB and 8 MiB. Backing files are read-only;
`nand-cow=on` permits volatile guest writes. Set
`nand-profile=synthetic-read-only` to enable this interface; its default is
`none`.

The generic QEMU ARM loader accepts caller-supplied ELF, raw or uImage input
through `-kernel`. Project-owned generated guests exercise that loader and
actual Cortex-A8 UART/timer/interrupt execution. A separate
[private firmware lane](firmware/README.md) accepts explicitly hashed local
inputs. It has booted a recovered Linux 2.6.37 kernel and UBIFS root filesystem.
No firmware image, device NAND layout or root filesystem is distributed here;
two-slot and full hardware acceptance remain open.

## Build and run

`make qemu-build` fetches the exact QEMU revision, prepares the overlay and
builds it. The configure/build phase disables downloads. Prepare its native
dependencies first: a compiler and archiver appropriate to the host, Ninja,
GLib, libfdt, libslirp, and QEMU's Python/Meson dependencies.

Create the Python build environment below `.work`. These versions were used
for the verified native build and satisfy the pinned QEMU source's
`pythondeps.toml`:

```sh
python3 -m venv .work/tools/qemu-python
.work/tools/qemu-python/bin/python -m pip install \
  meson==1.10.0 qemu.qmp==0.0.5 pycotap==1.3.1 \
  setuptools==84.2.0 wheel==0.48.0 packaging==26.3
export PATH="$PWD/.work/tools/qemu-python/bin:$PATH"
```

That preparation downloads public Python packages. Fetch QEMU and its pinned
public subprojects before the offline configure/build step:

```sh
sh lab/qemu/scripts/fetch-qemu.sh
sh lab/qemu/scripts/prepare-qemu.sh
(
  cd .work/build/qemu/qemu-aula-v11.0.3
  meson subprojects download keycodemapdb berkeley-softfloat-3 berkeley-testfloat-3
)
```

On macOS with Homebrew libraries, the tested build invocation from the
repository root was:

```sh
PATH="$PWD/.work/tools/qemu-python/bin:$PATH" \
AR=/usr/bin/ar RANLIB=/usr/bin/ranlib \
CFLAGS="$(pkg-config --cflags libfdt)" \
LDFLAGS="$(pkg-config --libs-only-L libfdt)" \
QEMU_JOBS=8 sh lab/qemu/scripts/build.sh
```

Native Apple archives are required for the native Apple linker; a GNU `ar`
earlier on PATH can produce incompatible archives. `QEMU_RECONFIGURE=1`
recreates the configuration when changing toolchain/library settings.

With GNU ARM binutils available:

```sh
make qemu-run       # generated Cortex-A8 UART and two timer IRQs
make qemu-verify    # source + controller/DMA probes + boot + actual CPU guest
make emulator-source-check  # public source inventory and private-artifact guard
```

`QEMU_ARM_AS` and `QEMU_ARM_LD` select explicit `arm-none-eabi-as` and
`arm-none-eabi-ld` executables. The generated
[guest documentation](guest/README.md) explains its assertions and receipts.
Probes use local Unix sockets and a socketpair; no external Ethernet connection
or vendor image is required.

For an already built binary, run
`QEMU_BINARY=/absolute/path/to/qemu-system-arm sh lab/qemu/scripts/verify.sh --require-runtime`.
This fails on missing inputs or a failing probe. Without `QEMU_BINARY`, the
script reports a source-only result. Product payload packaging and acceptance
remain unavailable; `make package-qemu` and `make qemu-acceptance` fail closed.

## Explicit DDR and Linux boot configuration

`-m 512M` is the default; `-m 1G` and `-m 2G` are also supported. DDR begins at
`0x80000000`. These capacities describe mapped address space, not DDR training
or a physical chip arrangement.

Machine properties `boot-machine-id`, `boot-ram-start` and `boot-ram-size`
select the Linux machine ID and boot-memory window. Defaults are ID zero,
start `0x80000000`, and size zero meaning all configured DDR. An explicit
window must be 4 KiB aligned and lie within DDR; a shifted start generally
requires an explicit size.

The Linux window controls the ARM loader's boot stub, raw-image placement and
ATAG_MEM. It does not remove the remaining DDR mapping or model firmware
reservations. uImage retains its header load/entry addresses; ELF retains its
own entry point. Supplied images and initrds must fit the intended layout,
including the loader's raw-kernel offset. No board-specific machine ID,
partition map, boot arguments or pad snapshot is hardcoded.

`control-init-file` optionally supplies exactly 4096 little-endian bytes for
CONTROL storage. Guest writes preserve neighboring halfwords; reset restores
the supplied snapshot. With no file, storage starts at zero. A snapshot does
not establish physical reset values or implement clocks/pin routing.

`early-uart-base` optionally aliases UART1's 32-byte legacy window at an
unmapped, 32-byte-aligned ARM32 address. Zero disables it. Overlapping aliases
are rejected.

Device properties can select NAND identity and two distinct MDIO addresses:

```sh
-global ti8168-synthetic-nand.id=0x11223344
-global ti8168-emac.phy0-address=5
-global ti8168-emac.phy1-address=23
```

These are illustrative values. The defaults remain the synthetic NAND ID and
PHY addresses 0/1. Selecting an ID does not implement that chip's ECC or timing.

Timers start gated. Guest code must select their PRCM source and enable each
module before accessing timer registers. The default input frequencies are
27 MHz CLKIN and 32768 Hz SYSCLK18; TCLKIN is undriven. These inputs do not
implement upstream PLLs or the board's clock tree.
Override them with the device properties `ti8168-prcm.clkin-frequency`,
`ti8168-prcm.sysclk18-frequency` and `ti8168-prcm.tclkin-frequency`, in Hz.
For example, `-global ti8168-prcm.tclkin-frequency=1000000` supplies a
synthetic 1 MHz external timer input.

GPIO banks expose generic input, output and output-enable lines without private
pad wiring. EMAC gathers/scatters channel-zero packets across bounded descriptor
chains. Its unverified legacy DMA address translation is disabled by default;
`-global ti8168-emac.legacy-dma-alias=on` explicitly retains that accommodation.

## Source and provenance boundary

The documented peripheral ranges, register layouts, and interrupt numbers used
by the model are public DM8168 hardware facts documented in Texas Instruments'
[TMS320DM816x Technical Reference Manual](https://www.ti.com/lit/ug/sprugx8c/sprugx8c.pdf)
(notably memory-map Tables 1-11 and 1-12 and interrupt Table 1-68) and
[DM816x data sheet](https://www.ti.com/lit/ds/symlink/tms320dm8168.pdf).
The QEMU integration points and the small amount of unified-diff context come
from the separately obtained, pinned, publicly licensed QEMU source identified
by `scripts/lib/common.sh`. New TI8168 model units use `GPL-2.0-or-later`;
changes to QEMU's `serial-mm` units retain their MIT-style upstream terms and
copyright notices, as recorded in the repository's third-party notices.

The 512 MiB RAM default, reserved-range synthetic SRAM aperture and bounded
bootstrap storage, two equal 256 MiB
NAND banks and OOB sizes, synthetic NAND identifier, PHY addresses, zeroed
CONTROL defaults, and incomplete device behavior are project model choices.
They are not asserted to be an AREC or other physical board configuration.
The patch and probes contain no firmware image, extracted filesystem, device
tree, vendor boot arguments, partition map, vendor library, or recovered source.

Firmware CLI outputs are restricted to private directories below `.work`.
`make emulator-source-check` checks the reviewed file inventory, overlay targets
and Git index for accidental artifacts; it runs within `make verify`. This does
not prove that arbitrary source text contains no confidential information.
Private evidence and generated receipts must remain outside published source.

Use a reviewed, pinned local QEMU v11.0.3 checkout. Run `QEMU_SOURCE=/path/to/pinned/qemu sh lab/qemu/scripts/verify.sh --require-source` to verify patch application and structure. Run `QEMU_BASE_SOURCE=/path/to/pinned/qemu sh lab/qemu/scripts/prepare-qemu.sh` to prepare a separate worktree. The individual `probe_*.py` entry points can also run against an explicit built binary.

See [FIDELITY.md](FIDELITY.md) for the precise boundary.
