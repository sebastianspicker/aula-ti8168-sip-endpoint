# Model provenance and disclosure boundary

The maintained model implements selected public TI8168 controller contracts.
Its reference is Texas Instruments **SPRUGX8C, TMS320DM816x DaVinci Digital
Media Processors Technical Reference Manual, revision C (March 2015)**:
[TI's publication](https://www.ti.com/lit/ug/sprugx8c/sprugx8c.pdf).
The reference is linked, not redistributed in the patch.

Public contracts and explicit synthetic choices are recorded separately below.
Passing these probes establishes their stated contracts, not full silicon or
appliance equivalence. All default probes use generated data and owned code.

| Model surface | Public reference and implemented contract | Synthetic choices and omissions | Executable evidence |
| --- | --- | --- | --- |
| Cortex-A8, UART, DDR and boot | QEMU's Cortex-A8/16550 models; TI memory map in chapter 1 | Caller-selected RAM size, Linux window and machine ID; direct kernel loading, no ROM/U-Boot, DDR training or CPU timing claim | `probe_boot.py`, `probe_guest.py` |
| INTC | §12.2.3 and §12.4: banks, masks, routing, priorities, threshold and agreement | Immediate arbitration; no privilege, idle or sorting latency | `probe_hardware.py` |
| General-purpose timers | §21.2–21.3: counter, prescaling, compare, reload and interrupt registers | Immediate register posting; no capture/PWM, bus-cycle timing or suspend integration | `probe_hardware.py`, `probe_guest.py`, `probe_prcm.py` |
| Timer PRCM | §18.7.4.26–32 and §18.7.17.25–31: seven clock selectors and module controls; §21.2.1.6 clock inputs | CLKIN defaults to 27 MHz, SYSCLK18 to 32768 Hz, external TCLKIN to undriven; configurable rates, no PLL implementation. Clock transitions are immediate; missing input remains nonfunctional. Reserved selections are rejected. Unmodeled PRCM offsets retain diagnostics | `probe_prcm.py` |
| GPIO0/GPIO1 | Chapter 8 register map and interrupt logic; table 1-68 IRQ96/97 and IRQ98/99 | Generic 32-pin input, output and output-enable lines per bank; no private pad routing, external devices or invented loopback. Immediate input sampling, debounce configuration storage only. Revision defaults to figure 8-7's fields (`0x50600801`); the contradictory reset summary in table 8-2 is not silently treated as verified silicon | `probe_gpio.py` |
| Watchdog | Chapter 22: counter, prescaling, nonposted completed writes, trigger, start/stop sequences and interrupt status | See [watchdog contract](WATCHDOG.md) for reset-value conflicts, timing and unmapped IRQ limitations. No guessed MPU interrupt route | `probe_watchdog.py` |
| EMAC CPDMA | §6.2.5.2–5: packet and descriptor queues, SOP/EOP, ownership, completion, offsets and buffer lengths; §6.2.16.1.5 host errors | Channel zero, at most 64 descriptors per packet and 2048 bytes per frame. RX capacity is preflighted: an undersized queue drops the whole packet without partial completion; hardware resource-overrun partial transfers/statistics are not implemented. No FCS, teardown, multi-channel scheduling or physical link timing | `probe_emac.py`, `probe_emac_chains.py` |
| MDIO/PHY | §6.3.3.11: MDIO read/write DATA fields | Two configurable synthetic PHYs; no negotiation or MDIO timing/completion interrupt. The historical DMA address alias is an unverified compatibility accommodation, disabled by default (`legacy-dma-alias=true` explicitly opts in) | `probe_phy.py`, `probe_emac_chains.py` |
| NAND/GPMC | Generic NAND commands and GPMC register interface | Generic configurable IDs and erased data; optional local backing files with volatile copy-on-write. No physical OOB, ECC, bad-block, redundant-slot or power-loss claim | `probe_synthetic_machine.py`; optional private guest lane |
| CONTROL/DMM/SRAM and bootstrap storage | Generic register/storage scaffolding, with only individually documented public fields | Historical guest-access observations informed storage compatibility. They do not establish reset, clock or engine behavior. CONTROL defaults to zero; an explicit private snapshot is execution input only. EDMA/PCIESS/McSPI storage implements no transfer engine | `probe_bootstrap.py` |
| Other peripherals and media | Named apertures only where indicated by the memory map | I2C, McASP, MMC, RTC, mailbox, second EMAC, SATA and USB remain placeholders. M3/DSP/HDVICP, capture/display/audio pipelines and private board wiring are absent | No functional acceptance |

## Private execution inputs

Recovered firmware, original root filesystems, register snapshots, PCB evidence,
boot logs and their receipts remain private. They may be supplied explicitly to
the firmware execution tools, but are not source or link inputs for the public
product. Firmware CLI outputs are confined to this checkout's ignored `.work`
directory, with private directory permissions and fresh artifact directories.
Do not copy the prepared QEMU tree or `.work` into a release archive.

`make emulator-source-check` validates the exact source inventory in
`lab/PUBLIC_SOURCES.json`, QEMU overlay target paths, and Git's index for
accidental private artifacts. New public files need a reviewed inventory entry;
the checker does not automatically approve them. This is an inventory and
artifact-confinement check. It cannot prove that authored text contains no
confidential information, and it does not replace review before publication.

No firmware blob, recovered vendor implementation, private register snapshot,
credential, private receipt or board-specific wiring table is required by the
public regression suite.
