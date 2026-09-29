# Synthetic TI8168 media board model

This directory maintains a partial QEMU model for TI8168 media-board experiments. The machine is named `ti8168-mediaboard`. Its default registers, NAND banks, PHY addresses, and boot identity are synthetic. They do not represent a physical board's reset state, wiring, firmware, manufacturer, or deployment support. The [hardware reference](../../docs/reference/ti8168-board.md) describes sourced TI8168 comparisons.

The model includes interrupt, timer, UART, GPMC, NAND, EMAC, EDMA, PCIESS, McSPI, and bounded register behavior. Several devices are placeholders. NAND has two equal 256 MiB synthetic banks. Each bank may receive a caller-supplied main image and OOB image, which must be regular files of exactly 256 MiB and 8 MiB. Backing files are read-only; `nand-cow=on` permits volatile guest writes. `nand-profile=synthetic-read-only` opts into the transport; the default is `none`.

The generic QEMU ARM loader may take a caller-supplied ELF or raw image through `-kernel`. No firmware image, device NAND layout, root filesystem, kernel hash, boot matrix, or two-slot acceptance workflow is part of this maintained QEMU lane. The model does not establish Linux boot or physical hardware behavior.

Use a reviewed, pinned local QEMU v11.0.3 checkout. Run `QEMU_SOURCE=/path/to/pinned/qemu sh lab/qemu/scripts/verify.sh --require-source` to verify patch application and structure. Run `QEMU_BASE_SOURCE=/path/to/pinned/qemu sh lab/qemu/scripts/prepare-qemu.sh` to prepare a separate worktree. With a built binary, `python3 -B lab/qemu/scripts/probe_synthetic_machine.py /path/to/qemu-system-arm` checks selected zero defaults, SRAM and DMM storage, and NAND backing isolation. Builds use local dependencies and disable downloads.

See [FIDELITY.md](FIDELITY.md) for the precise boundary.
