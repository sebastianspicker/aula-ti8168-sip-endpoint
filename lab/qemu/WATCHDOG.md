# Public TI8168 watchdog model

`hw/arm/ti8168_watchdog.c` implements the public WDT register interface at
`0x480c2000` (4 KiB) for the synthetic TI8168 machine. It uses no firmware,
private board configuration or vendor driver implementation.

Source: Texas Instruments **SPRUGX8C**, March 2015,
[TI816x Technical Reference Manual](https://www.ti.com/lit/ug/sprugx8c/sprugx8c.pdf).
Relevant sections are §1.10.3.1.4 / Figure 1-73 (printed p226, watchdog clock),
Table 1-12 (register window), Table 1-68 (MPU interrupt mapping), and chapter 22
(printed pp2029–2049).

The implemented contract includes:

- 32-/16-bit little-endian register access; byte accesses are rejected.
- WIDR, WDSC/WDST software-reset handshake, legacy WISR/WIER, WCLR, WCRR,
  WLDR, WTGR, WWPS, WDLY, WSPR, raw/masked interrupt status and enable SET/CLR.
- A 32-bit upward counter, nominal 32768 Hz functional input and optional
  `2**PTV` prescaler when PRE is set. This differs from the dual-mode timer's
  `2**(PTV+1)` division. A nonzero device `frequency` property can select another
  caller-supplied clock; it is not coupled to a modeled PRCM clock tree.
- Low-16-bit WSPR disable (`AAAA`, `5555`) / enable (`BBBB`, `4444`) sequences;
  isolated second words, invalid intervening words and uncompleted sequences
  do not change the enable state. The upper 16 bits do not participate.
- Changed WTGR values reload WCRR from WLDR and reset the prescaler; unchanged
  values do not reload. Prescaler writes become active on reload/overflow.
- Frozen disabled counters, protected configuration writes and coherent
  halfword counter reads: reading the low half captures a shadow for the next
  high-half read; a 32-bit read remains live and does not refresh that shadow.
- Raw overflow/delay events, W1C acknowledgement, W1S debug events, masked
  status, legacy enable replacement and SET/CLR aliases; a pending raw event
  asserts the IRQ if its enable is later set. Delay scheduling uses the
  explicit `(WDLY - count + 1)` period formula in §22.2.11; values below WLDR do
  not cause delay interrupts.
- Overflow reload and an independent QEMU watchdog action. The default action
  requests machine reset, emitting QMP `WATCHDOG(action=reset)` followed by
  `RESET(guest=true, reason=guest-reset)`. `-watchdog-action` / QMP action
  selection remains available for controlled inspection. A changed WTGR with
  `WLDR=0xffffffff` also expires after one *unprescaled* functional cycle when
  disabled, as required by the special-case warning in §22.2.6.

## Explicit interpretations and limits

The manual is not internally consistent about boot state. The model uses
**enabled but not running** from §22.1.1, `WCRR=0` from Table 22-18,
`WLDR=0xffffffbe` from device reset-context Table 22-2, and `PRE=1, PTV=0` from
Table 22-17. A changed trigger starts an enabled timer; an explicit enable
sequence starts it as well. Generic Table 22-19 instead lists a zero WLDR reset
value, and Table 22-5's claimed two-second default does not follow from Table
22-2 and the documented clock. This model does not claim to resolve those
silicon reset-context discrepancies. The nominal **32768 Hz** comes from the
public clock integration in §1.10.3.1.4, which explicitly supplies the watchdog;
chapter 22's “32 kHz” examples are rounded descriptions.

Table 22-25's explicit masked WIRQSTAT register semantics take precedence over
the generic note claiming RAW and masked registers give equal read information.
WISR reads raw status, with its documented W1C behavior.

Device-specific §22.1.3 says this integration supports **nonposted writes
only**. All functional writes therefore have their completed effects visible
when the modeled MMIO transaction returns, and WWPS reads zero. The two-write
WSPR sequence remains mandatory. QEMU does not reproduce the L4 bus stalls,
the documented 1.5–2.5 functional-cycle WSPR synchronization latency, or reset
handshake cycle timing: software-reset completion is also immediate at the
completed MMIO boundary. The generic posted WDT2/WDT3 note in §22.2.13 does not
override this integration-specific nonposted contract.

Configuration writes while enabled are rejected and logged, following the
programming requirement in §22.2.9; undocumented effects of violating that
requirement are not modeled.

The SysBus IRQ output is implemented and tested directly. No MPU INTC input is connected: chapter 22 describes WDTINT, but the complete
Table 1-68 mapping provides no WDT source. External WD_OUT pin electrical pulse
shape, PRCM reset-status latching/retention, cold-vs-warm reset domains, JTAG
emulation/EMUFREE behavior, bus cycles and clock-domain metastability are not
modeled. QEMU virtual time stops when the VM is paused. For nonintegral clock
periods, deadlines round up to the next nanosecond; this is not a cycle-accuracy
claim. The surrounding machine already prohibits migration; this device does
not add independent migration support.

## Reproduce the generated probe

After integrating and building the overlay:

```sh
python3 lab/qemu/scripts/probe_watchdog.py /path/to/qemu-system-arm \
  --json .work/qemu-watchdog/probe.json
```

The probe uses generated MMIO values, the qtest virtual clock, direct SysBus IRQ
interception and QMP events. It checks reset defaults, completed nonposted writes, incomplete/full magic sequences, halfword writes and
counter shadowing, prescaler arithmetic, unchanged/changed trigger behavior,
configuration protection, freeze/reload, delay timing, IRQ masks/acknowledgement,
overflow reload and an actual watchdog-originated machine reset. The JSON
receipt identifies the executable hash, public source, nominal clock,
assertion count, reset events and limitations. No network, kernel, rootfs,
firmware or private state is used. It validates this model's documented public
contract; passing it does not establish whole-board or electrical equivalence.
