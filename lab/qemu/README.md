# Synthetic TI8168 media board model

This directory maintains a partial QEMU model for TI8168 media-board experiments. The machine is named `ti8168-mediaboard`. Its default registers, NAND banks, PHY addresses, and boot identity are synthetic. They do not represent a physical board's reset state, wiring, firmware, manufacturer, or deployment support. The [hardware reference](../../docs/reference/ti8168-board.md) describes sourced TI8168 comparisons.

The model includes interrupt, timer, UART, GPMC, NAND, EMAC, EDMA, PCIESS, McSPI, and bounded register behavior. Several devices are placeholders. NAND has two equal 256 MiB synthetic banks. Each bank may receive a caller-supplied main image and OOB image, which must be regular files of exactly 256 MiB and 8 MiB. Backing files are read-only; `nand-cow=on` permits volatile guest writes. `nand-profile=synthetic-read-only` opts into the transport; the default is `none`.

The generic QEMU ARM loader may take a caller-supplied ELF or raw image through `-kernel`. No firmware image, device NAND layout, root filesystem, kernel hash, boot matrix, or two-slot acceptance workflow is part of this maintained QEMU lane. The model does not establish Linux boot or physical hardware behavior.

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
register defaults, and incomplete device behavior are project model choices.
They are not asserted to be an AREC or other physical board configuration.
The patch and probes contain no firmware image, extracted filesystem, device
tree, boot arguments, partition map, vendor library, or recovered source.

Use a reviewed, pinned local QEMU v11.0.3 checkout. Run `QEMU_SOURCE=/path/to/pinned/qemu sh lab/qemu/scripts/verify.sh --require-source` to verify patch application and structure. Run `QEMU_BASE_SOURCE=/path/to/pinned/qemu sh lab/qemu/scripts/prepare-qemu.sh` to prepare a separate worktree. With a built binary, `python3 -B lab/qemu/scripts/probe_synthetic_machine.py /path/to/qemu-system-arm` checks selected zero defaults, SRAM and DMM storage, and NAND backing isolation. Builds use local dependencies and disable downloads.

See [FIDELITY.md](FIDELITY.md) for the precise boundary.
