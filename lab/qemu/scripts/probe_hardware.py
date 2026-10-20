#!/usr/bin/env python3
"""Check sourced TI8168 INTC/timer contracts against a built QEMU machine.

Register expectations follow TI SPRUGX8C sections 12.2.3, 12.4 and 21.2/21.3.
These qtests drive real model MMIO, input lines and the QEMU virtual clock;
they do not establish board wiring, clock/power integration or Linux boot.
"""
from __future__ import annotations

import socket
import sys
from pathlib import Path

from probe_support import qtest_only, recv_line

INTC = 0x48200000
TIMERS = (
    (0x4802E000, 67), (0x48040000, 68), (0x48042000, 69),
    (0x48044000, 92), (0x48046000, 93), (0x48048000, 94),
    (0x4804A000, 95),
)
SPURIOUS = 0xFFFFFF80


class HardwareProbe:
    def __init__(self, stream: socket.socket):
        self.stream = stream
        self.levels = {0: False, 1: False}
        self.command("irq_intercept_out /machine/intc sysbus-irq")

    def command(self, value: str) -> str:
        self.stream.sendall(value.encode() + b"\n")
        for _ in range(1024):
            reply = recv_line(self.stream)
            if reply.startswith("IRQ "):
                _, action, number = reply.split()
                if action not in ("raise", "lower"):
                    raise RuntimeError(f"invalid IRQ event: {reply}")
                self.levels[int(number)] = action == "raise"
                continue
            if reply == "OK" or reply.startswith("OK "):
                return reply
            raise RuntimeError(f"{value}: {reply}")
        raise RuntimeError(f"{value}: excessive asynchronous IRQ events")

    def write(self, address: int, value: int) -> None:
        self.command(f"writel {address:#x} {value:#x}")

    def read(self, address: int, expected: int, label: str = "register") -> None:
        actual = int(self.command(f"readl {address:#x}").split()[1], 0)
        if actual != expected:
            raise RuntimeError(
                f"{label} at {address:#x}: expected {expected:#x}, got {actual:#x}"
            )

    def outputs(self, irq: bool, fiq: bool = False) -> None:
        if self.levels != {0: irq, 1: fiq}:
            raise RuntimeError(f"INTC outputs: expected IRQ={irq}, FIQ={fiq}, "
                               f"got {self.levels}")

    def step(self, nanoseconds: int) -> None:
        self.command(f"clock_step {nanoseconds}")

    def reset(self) -> None:
        for index, (base, _) in enumerate(TIMERS):
            # Public PRCM cold state gates each timer; select CLKIN, then enable.
            self.write(0x48180390 + 4 * index, 2)
            self.write(0x48181570 + 4 * index, 2)
            self.write(base + 0x10, 1)
        self.write(INTC + 0x10, 2)
        self.outputs(False)

    def source(self, number: int, value: bool) -> None:
        self.command(f"set_irq_in /machine/intc unnamed-gpio-in {number} {int(value)}")


def bank(number: int, offset: int) -> int:
    return INTC + 0x80 + (number // 32) * 0x20 + offset


def unmask(probe: HardwareProbe, number: int) -> None:
    probe.write(bank(number, 8), 1 << (number % 32))


def software(probe: HardwareProbe, number: int, enabled: bool) -> None:
    probe.write(bank(number, 0x10 if enabled else 0x14), 1 << (number % 32))


def ilr(probe: HardwareProbe, number: int, priority: int, fiq: bool = False) -> None:
    probe.write(INTC + 0x100 + number * 4, (priority << 2) | int(fiq))


def probe_intc(probe: HardwareProbe) -> None:
    probe.reset()
    probe.read(INTC, 0, "INTC revision")
    probe.read(INTC + 0x68, 0xFF, "disabled priority threshold")
    for offset in (0x40, 0x44, 0x60, 0x64):
        probe.read(INTC + offset, SPURIOUS, "idle spurious status")
    probe.write(INTC + 0x50, 0xFFFFFFFF)
    probe.read(INTC + 0x50, 3, "IDLE field mask")
    probe.write(INTC + 0x100, 0xFFFFFFFF)
    probe.read(INTC + 0x100, 0x1FD, "seven-bit priority and route field mask")

    # All banks: software status, MIR aliases and route-filtered pending views.
    for index in range(4):
        probe.reset()
        irq, fiq = index * 32 + 3, index * 32 + 31
        bits = (1 << 3) | (1 << 31)
        ilr(probe, irq, 5)
        ilr(probe, fiq, 2, True)
        software(probe, irq, True)
        software(probe, fiq, True)
        probe.read(bank(irq, 0), bits, "raw pending with masks set")
        probe.read(bank(irq, 0x10), bits, "software ISR_SET readback")
        probe.read(bank(irq, 0x18), 0)
        probe.read(bank(irq, 0x1C), 0)
        probe.outputs(False)
        unmask(probe, irq)
        unmask(probe, fiq)
        probe.read(bank(irq, 0x18), 1 << 3, "routed IRQ pending")
        probe.read(bank(irq, 0x1C), 1 << 31, "routed FIQ pending")
        probe.read(INTC + 0x40, irq)
        probe.read(INTC + 0x44, fiq)
        probe.read(INTC + 0x60, 5)
        probe.read(INTC + 0x64, 2)
        probe.outputs(True, True)
        software(probe, irq, False)
        software(probe, fiq, False)
        probe.read(bank(irq, 0x10), 0)
        probe.read(bank(irq, 0x14), 0, "ISR_CLEAR is write-only")
        probe.read(INTC + 0x40, irq, "SIR result retained until agreement")
        probe.outputs(True, True)
        probe.write(INTC + 0x48, 1)
        probe.outputs(False, True)
        probe.write(INTC + 0x48, 2)
        probe.outputs(False)
        probe.write(bank(irq, 0xC), bits)
        probe.read(bank(irq, 4), 0xFFFFFFFF, "MIR_SET")
        probe.read(bank(irq, 8), 0, "MIR_CLEAR is write-only")
        probe.read(bank(irq, 0xC), 0, "MIR_SET is write-only")

    # One simultaneous request write must sort by priority then descending ID.
    for priorities, expected in (((7, 7), 11), ((1, 127), 4)):
        probe.reset()
        ilr(probe, 4, priorities[0])
        ilr(probe, 11, priorities[1])
        unmask(probe, 4)
        unmask(probe, 11)
        probe.write(bank(4, 0x10), (1 << 4) | (1 << 11))
        probe.read(INTC + 0x40, expected, "priority/tie sort")
        # Reading SIR does not acknowledge or select a different source.
        probe.read(INTC + 0x40, expected)
        probe.outputs(True)
        software(probe, expected, False)
        probe.read(INTC + 0x40, expected)
        probe.write(INTC + 0x48, 1)
        probe.read(INTC + 0x40, 4 if expected == 11 else 11)

    # Threshold equality blocks, 0 and 1 admit priority zero, 0xff disables.
    for priority, threshold, admitted in (
        (5, 5, False), (5, 6, True), (5, 4, False),
        (0, 0, True), (0, 1, True), (1, 1, False),
        (127, 127, False), (127, 255, True),
    ):
        probe.reset()
        ilr(probe, 9, priority)
        probe.write(INTC + 0x68, threshold)
        unmask(probe, 9)
        software(probe, 9, True)
        probe.outputs(admitted)
        probe.read(INTC + 0x40, 9 if admitted else SPURIOUS)
        # Threshold does not change pending views or invalidate a latched SIR.
        probe.read(bank(9, 0x18), 1 << 9)
        probe.write(INTC + 0x68, 255)
        probe.outputs(True)
        probe.read(INTC + 0x40, 9)
        probe.write(INTC + 0x68, priority)
        probe.read(INTC + 0x40, 9)
        probe.write(INTC + 0x48, 1)
        probe.outputs(priority == 0)

    # Softreset clears software requests but preserves an asserted input wire.
    probe.reset()
    probe.source(72, True)
    software(probe, 73, True)
    probe.write(INTC + 0x10, 2)
    probe.read(bank(72, 0), 1 << (72 % 32))
    probe.read(bank(72, 0x10), 0)
    probe.outputs(False)
    unmask(probe, 72)
    probe.outputs(True)
    probe.write(INTC + 0x48, 1)
    probe.outputs(True)
    probe.read(INTC + 0x40, 72, "asserted level retriggers after agreement")
    probe.source(72, False)
    probe.read(bank(72, 0), 0)
    probe.outputs(True)
    probe.write(INTC + 0x48, 1)
    probe.outputs(False)


def probe_timer_registers(probe: HardwareProbe) -> None:
    for base, number in TIMERS:
        probe.reset()
        probe.read(base, 0x4FFF1301, "timer revision")
        probe.read(base + 0x44, 0xFFFFFFFF, "TTGR read value")
        unmask(probe, number)
        probe.write(base + 0x24, 0xFFFFFFFF)
        probe.read(base + 0x24, 7, "RAW software events")
        probe.read(base + 0x28, 0, "disabled events hidden in masked status")
        probe.outputs(False)
        probe.write(base + 0x2C, 0xFFFFFFFA)
        probe.read(base + 0x2C, 2, "IRQENABLE_SET readback")
        probe.read(base + 0x30, 2, "IRQENABLE_CLR readback")
        probe.read(base + 0x28, 2)
        probe.read(INTC + 0x40, number, "timer interrupt wiring")
        probe.outputs(True)
        probe.write(base + 0x30, 2)
        probe.read(base + 0x24, 7, "disabling preserves raw events")
        probe.read(base + 0x28, 0)
        probe.write(INTC + 0x48, 1)
        probe.outputs(False)
        probe.write(base + 0x28, 1)
        probe.read(base + 0x24, 6, "W1C clears disabled raw event")
        probe.write(base + 0x28, 0)
        probe.read(base + 0x24, 6, "zero W1C has no effect")
        probe.write(base + 0x10, 2)
        probe.read(base + 0x10, 2, "EMUFREE is bit1, not software reset")
        probe.read(base + 0x24, 6)
        probe.write(base + 0x10, 1)
        probe.read(base + 0x10, 0)
        probe.read(base + 0x24, 0)
        probe.read(base + 0x2C, 0)
        probe.write(base + 0x24, 7)
        probe.write(base + 0x54, 6)
        probe.read(base + 0x54, 4, "TSICR reset self-clears")
        probe.read(base + 0x24, 0, "TSICR functional reset")


def probe_timer_clock(probe: HardwareProbe) -> None:
    base, number = TIMERS[0]
    probe.reset()
    # 27 input clocks per microsecond; PRE/PTV must retain the exact ratio.
    for ptv in range(8):
        probe.write(base + 0x10, 1)
        divisor = 1 << (ptv + 1)
        probe.write(base + 0x38, 1 | 0x20 | (ptv << 2))
        probe.step(1000 * divisor)
        probe.read(base + 0x3C, 27, f"prescaler divisor {divisor}")
        probe.write(base + 0x38, 0)
        probe.step(10000)
        probe.read(base + 0x3C, 27, "stopped counter frozen")
    probe.write(base + 0x10, 1)
    probe.write(base + 0x38, 1)
    probe.step(1000)
    probe.read(base + 0x3C, 27, "unprescaled counter")
    probe.write(base + 0x40, 100)
    probe.write(base + 0x44, 0)
    probe.read(base + 0x3C, 100, "TTGR reload during counting")
    probe.step(1000)
    probe.read(base + 0x3C, 127)
    # A TCRR write discards the prescaler's partly accumulated cycle.
    probe.write(base + 0x10, 1)
    probe.write(base + 0x38, 1 | 0x20 | (7 << 2))
    probe.step(9000)
    probe.read(base + 0x3C, 0)
    probe.write(base + 0x3C, 5)
    probe.step(1000)
    probe.read(base + 0x3C, 5, "TCRR resets prescaler phase")

    # Compare configuration changes must not reset a partly elapsed clock.
    probe.reset()
    probe.write(base + 0x38, 1)
    probe.step(38)
    probe.read(base + 0x3C, 1)
    probe.write(base + 0x4C, 2)
    probe.write(base + 0x38, 0x41)
    probe.step(37)
    probe.read(base + 0x3C, 2, "compare-only writes preserve source-clock phase")
    probe.read(base + 0x24, 1)

    # At 27MHz 27 clocks expire after exactly 1000ns, never 999ns.
    probe.reset()
    unmask(probe, number)
    probe.write(base + 0x2C, 2)
    probe.write(base + 0x3C, 0xFFFFFFE5)
    probe.write(base + 0x38, 1)
    probe.step(999)
    probe.read(base + 0x24, 0, "no early overflow")
    probe.step(1)
    probe.read(base + 0x24, 2)
    probe.read(base + 0x38, 0, "one-shot clears ST")
    probe.read(base + 0x3C, 0)
    probe.outputs(True)
    probe.write(base + 0x28, 2)
    probe.write(INTC + 0x48, 1)
    probe.outputs(False)
    probe.step(1000)
    probe.read(base + 0x3C, 0, "one-shot remains stopped")
    probe.read(base + 0x24, 0)

    # Repeating overflow reloads TLDR, rather than counting from zero.
    probe.reset()
    probe.write(base + 0x40, 0xFFFFFFE5)
    probe.write(base + 0x44, 0)
    probe.write(base + 0x38, 3)
    for _ in range(3):
        probe.step(1000)
        probe.read(base + 0x3C, 0xFFFFFFE5)
        probe.read(base + 0x38, 3)
        probe.read(base + 0x24, 2)
        probe.write(base + 0x28, 2)

    # Two-clock reloads retain fractional phase: 27 periods take 2000ns.
    probe.reset()
    probe.write(base + 0x40, 0xFFFFFFFE)
    probe.write(base + 0x44, 0)
    origin = int(probe.command("clock_step 1").split()[1], 0)
    probe.write(base + 0x38, 3)
    for event in range(1, 28):
        actual = int(probe.command("clock_step").split()[1], 0) - origin
        expected = (event * 2000 + 26) // 27
        if actual != expected:
            raise RuntimeError(f"fractional reload event {event}: "
                               f"expected {expected}ns, got {actual}ns")

    # Compare before overflow, with CE gating and with both events unmasked.
    probe.reset()
    unmask(probe, number)
    probe.write(base + 0x2C, 3)
    probe.write(base + 0x4C, 27)
    probe.write(base + 0x38, 0x41)
    probe.step(999)
    probe.read(base + 0x24, 0)
    probe.step(1)
    probe.read(base + 0x3C, 27)
    probe.read(base + 0x24, 1, "TMAR match")
    probe.outputs(True)
    probe.write(base + 0x28, 1)
    probe.write(INTC + 0x48, 1)
    probe.outputs(False)
    probe.write(base + 0x38, 1)  # Disable CE, preserve input clock phase.
    probe.write(base + 0x4C, 54)
    probe.step(1000)
    probe.read(base + 0x24, 0, "disabled compare")
    probe.write(base + 0x4C, 81)
    probe.write(base + 0x38, 0x41)
    probe.step(1000)
    probe.read(base + 0x24, 1, "re-enabled compare")
    # Compare at the top count occurs one tick before overflow.
    probe.reset()
    probe.write(base + 0x4C, 0xFFFFFFFF)
    probe.write(base + 0x3C, 0xFFFFFFFE)
    probe.write(base + 0x38, 0x41)
    probe.step(38)
    probe.read(base + 0x24, 1)
    probe.step(37)
    probe.read(base + 0x24, 3)
    probe.read(base + 0x38, 0x40)
    # Reload equality is a match, and compare never resets the counter itself.
    probe.reset()
    probe.write(base + 0x40, 0xFFFFFFE5)
    probe.write(base + 0x4C, 0xFFFFFFE5)
    probe.write(base + 0x3C, 0xFFFFFFFE)
    probe.write(base + 0x38, 0x43)
    probe.step(75)
    probe.read(base + 0x24, 3, "reload compare equality")
    probe.read(base + 0x3C, 0xFFFFFFE5)
    # Software reset cancels a pending timer callback.
    probe.write(base + 0x10, 1)
    probe.step(2000)
    probe.read(base + 0x24, 0)
    probe.read(base + 0x38, 0)


def run_hardware_probes(qemu: Path) -> None:
    with qtest_only(qemu, "ti8168-mediaboard", "aula-hardware-") as stream:
        probe = HardwareProbe(stream)
        probe_intc(probe)
        probe_timer_registers(probe)
        probe_timer_clock(probe)
        probe.reset()
    print("TI8168 INTC and timer hardware contract probes passed.")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} QEMU_SYSTEM_ARM")
    run_hardware_probes(Path(sys.argv[1]).resolve(strict=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
