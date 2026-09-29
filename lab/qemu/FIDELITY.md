# QEMU model fidelity

The `ti8168-mediaboard` machine is a synthetic TI8168 media-board experiment. Its current defaults are model choices, not measured hardware reset values. The [README](README.md) describes the inputs and local checks.

| Surface | Maintained behavior | Check | Limit |
| --- | --- | --- | --- |
| ARM boot | Standard QEMU loader accepts caller-supplied ELF or raw input | Patch application and optional machine probe | No device firmware or Linux boot claim |
| NAND | Two equal synthetic banks with exact-size read-only main/OOB backing and optional volatile COW | NAND qtest and synthetic machine probe | No recovered MTD map, UBI mount, or physical NAND identity claim |
| EMAC/MDIO | Partial one-channel descriptor, IRQ, and two-PHY state machine using synthetic addresses 0 and 1 | EMAC qtest | No board wiring or network acceptance claim |
| Interrupts, timers, UART, GPMC, and bootstrap registers | Partial device state machines and zero-initialized synthetic CONTROL storage | Synthetic probe and patch quality check | Unimplemented offsets and peripherals remain |
| Physical board and vendor media | No validation | None | Requires separate authorized physical and provenance work |

The former recovered kernel and NAND launchers, exact pad snapshot, firmware hashes, boot matrix, web bootstrap, and two-slot corpus tests were removed from the maintained source candidate. Their prior results are not evidence for this synthetic model.
