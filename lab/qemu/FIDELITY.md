# QEMU model fidelity

`ti8168-mediaboard` is a partial functional model. Selected INTC, timer, PRCM,
GPIO, watchdog and EMAC behavior follows TI's public SPRUGX8C manual. Other peripherals and board
defaults remain synthetic or unimplemented. The tests below establish specific
software-visible contracts; they do not establish complete board equivalence.

| Surface | Maintained behavior and executable check | Remaining limit |
| --- | --- | --- |
| CPU/UART/IRQ | Generated Cortex-A8 ELF executes under TCG, writes UART, services two timer IRQs, clears status, acknowledges INTC and exits through semihosting (`probe_guest.py`); private Linux execution described below | No instruction timing or cache/coherency validation |
| DDR and boot | 512 MiB, 1 GiB or 2 GiB mapped from `0x80000000`; caller-selected Linux window and machine ID. Generated raw/uImage/ELF guests check registers, ATAG_MEM and entry behavior; endpoint writes test aliasing (`probe_boot.py`) | No DDR training, physical bank wiring, boot ROM/U-Boot, reserved-memory discovery or arbitrary kernel/device-tree acceptance |
| INTC | Four banks, IRQ/FIQ route-filtered pending bits, software/level requests, masks, seven-bit priority, descending-ID ties, threshold including priority zero, latched SIR/output until agreement (`probe_hardware.py`) | No sorting-cycle latency, privilege/protection enforcement or complete idle/power behavior |
| Seven timers and PRCM | Sourced reset/status fields, raw/masked interrupts, enables, one-shot stopping, prescale ratios, autoreload and TMAR comparison; seven source selectors and module gates, cold reset, retained fractional/prescaler progress and IRQ behavior (`probe_hardware.py`, `probe_prcm.py`) | Configurable input rates, no PLL or complete power/clock domains. No capture pins, PWM or cycle-accurate bus/posting behavior. Gated MMIO returns a transaction error; actual ARM abort handling is not tested |
| GPIO | Two 32-pin controllers, input/output/direction lines, byte/halfword/word accesses, raw/masked status, set/clear aliases, edge/level detection, acknowledgement and all four MPU interrupt routes (`probe_gpio.py`) | No private PCB wiring, pad mux or input feedback; no debounce timing or PRCM coupling. Revision-field conflict is recorded in `PROVENANCE.md` |
| Watchdog | Functional counter, prescaler, reload/trigger, start/stop sequence, status and reset behavior (`probe_watchdog.py`) | No CPU bus-stall timing or guessed MPU IRQ route; documented manual reset/synchronization ambiguities in `WATCHDOG.md` |
| NAND/GPMC | Two generic backing regions, configurable ID, main/OOB reads, volatile program/erase COW and backing-file isolation; private update-derived fixture boots the original UBIFS root | No physical MTD/OOB capture, BCH/ECC or bad-block fidelity, redundant-slot or power-loss acceptance |
| EMAC/MDIO | Register DATA semantics; channel-zero packet TX gather/RX scatter, buffer offsets, SOP ownership, EOP completion/EOQ and channel/wrapper masks (`probe_emac.py`, `probe_emac_chains.py`); original Linux driver passes isolated gateway ICMP | Explicit bounds of 64 descriptors and 2048 packet bytes; insufficient RX capacity drops the whole frame. No partial-error transfer/overrun statistics, FCS, teardown, other channels, full DMASTATUS error encoding, MDIO completion interrupt/timing, PHY negotiation or physical network validation |
| Bootstrap storage | Bounded SRAM/DMM/CONTROL, caller-supplied CONTROL snapshot, halfword access and optional UART alias; configurable PHY addressing (`probe_bootstrap.py`, `probe_phy.py`) | Snapshots do not establish reset values, pin routing or clock behavior; EDMA/PCIESS/McSPI storage does not implement those engines |
| Other peripherals/media | Named placeholders for I2C, McASP, MMC, RTC, mailbox, second EMAC, SATA and USB | PRCM outside the timer controls remains incomplete; M3/DSP/HDVICP, capture, display and audio pipelines remain absent |

EMAC's old buffer-address translation for `0x41000000..0x60ffffff` is disabled
by default. Its physical hardware justification is unverified; explicit
`-global ti8168-emac.legacy-dma-alias=on` retains it only for compatibility.
The packet probes use DDR physical addresses and separately check this opt-in.
Malformed or resource-starved descriptor queues cannot publish a successful
packet completion.

`make qemu-verify` requires all runtime checks. `verify.sh` without a binary is
explicitly source-only; it cannot award hardware fidelity. Generated guest
receipts retain source/tool/ELF/QEMU hashes and UART evidence under `.work`.
The [provenance record](PROVENANCE.md) identifies public references, synthetic
choices and unresolved assumptions per model surface. The default regression
suite uses no vendor image and makes no external network
connection. The separate [private firmware lane](firmware/README.md) requires
explicit hashed local inputs.

The [ARM application lane](../arm-userspace/README.md) separately runs the
maintained daemon/C tests and optionally a real PJSIP UDP call under Cortex-A8
user-mode QEMU. This checks application instructions and protocol behavior.
It uses the host Linux kernel and a supplied ARM libc, so it does not prove the
board's Linux 2.6.37/glibc 2.12 ABI or hardware media behavior.

The [legacy build lane](../arm-userspace/LEGACY.md) adds authenticated public
older headers/startup objects and a private glibc 2.12.2 execution oracle.
Both default and locked PJSIP builds pass its tests and ELF ABI gate. The same
owned binaries also pass ten selected software checks inside the original
Linux 2.6.37 rescue guest: seven C test programs, configuration validation,
daemon self-test and fixture call. These checks do not exercise hardware media.
The owned [SIP fixture](guest/LEGACY_SIP_SMOKE.md) also passes on that same
original-kernel guest: an actual peer-UID-authorized LSZ1 request produces one
INVITE/ACK/BYE dialog, idle/established/terminated states and clean daemon exit.
Its protocol rejection self-test runs first. All sockets are guest loopback;
media is inactive, so this is signaling/control evidence only.

Private checks boot an explicitly supplied Linux 2.6.37 kernel and glibc 2.12.2
runtime, reach BusyBox through a generated rescue initramfs, and separately
mount the original UBIFS root read-only. The guest reports its kernel release
and verifies the selected runtime bytes. These tests bypass vendor startup.

Caller-supplied register snapshots do not establish hardware reset values,
pin routing or clock behavior. PRCM outside the timer controls remains
incomplete. Update-derived NAND uses generated FF spare data, not captured
OOB. The full physical NAND backup, both deployed-slot kernels, hardware media
and a current physical trace comparison remain missing. Boot warnings remain
visible.

Register references: TI [SPRUGX8C](https://www.ti.com/lit/ug/sprugx8c/sprugx8c.pdf),
§12.2.3/12.4 (INTC), §21.2/21.3 (timers), §18.7 (PRCM), chapter 8 (GPIO),
chapter 22 (watchdog), §6.2.5 (CPDMA) and §6.3.3.11 (MDIO).
