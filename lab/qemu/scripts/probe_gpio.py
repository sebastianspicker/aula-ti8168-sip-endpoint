#!/usr/bin/env python3
"""Exercise public-manual TI8168 GPIO contracts with generated qtest inputs.

Source: TI SPRUGX8C chapter 8 and tables 1-12/1-68. GPIO0 is at 0x48032000
with MPU IRQs 96/97; GPIO1 is at 0x4804c000 with IRQs 98/99. No firmware,
physical pin assignment or PCB wiring is used. These tests do not validate pad
voltage, pinmux, debounce timing, interface-clock latency or PRCM/power behavior.
"""
from __future__ import annotations

import socket
import sys
from pathlib import Path

from probe_support import qtest_only, recv_line

INTC = 0x48200000
BANKS = ((0x48032000, "gpio0", 96), (0x4804C000, "gpio1", 98))
REVISION = 0x50600801  # SPRUGX8C figure 8-7 (table 8-2 disagrees).


class GPIOProbe:
    def __init__(self, stream: socket.socket, path: str, group: str) -> None:
        self.stream = stream
        self.levels: dict[int, bool] = {}
        self.command(f"irq_intercept_out {path} {group}")

    def command(self, value: str) -> str:
        self.stream.sendall(value.encode() + b"\n")
        for _ in range(1024):
            reply = recv_line(self.stream)
            if reply.startswith("IRQ "):
                _, action, number = reply.split()
                if action not in ("raise", "lower"):
                    raise RuntimeError(f"invalid IRQ event: {reply}")
                self.levels[int(number)] = action == "raise"
            elif reply == "OK" or reply.startswith("OK "):
                return reply
            else:
                raise RuntimeError(f"{value}: {reply}")
        raise RuntimeError(f"{value}: excessive asynchronous events")

    def write(self, address: int, value: int, width: str = "l") -> None:
        self.command(f"write{width} {address:#x} {value:#x}")

    def read(self, address: int, expected: int, label: str, width: str = "l") -> None:
        actual = int(self.command(f"read{width} {address:#x}").split()[1], 0)
        if actual != expected:
            raise RuntimeError(f"{label} at {address:#x}: expected {expected:#x}, got {actual:#x}")

    def pin(self, name: str, pin: int, value: bool) -> None:
        self.command(f"set_irq_in /machine/{name} gpio-in {pin} {int(value)}")

    def inputs(self, name: str, value: int) -> None:
        for pin in range(32):
            self.pin(name, pin, bool(value & (1 << pin)))

    def output(self, number: int, expected: bool, label: str) -> None:
        actual = self.levels.get(number, False)
        if actual != expected:
            raise RuntimeError(f"{label} {number}: expected {expected}, got {actual}")

    def reset(self, base: int, name: str) -> None:
        self.write(base + 0x10, 2)
        self.inputs(name, 0)
        self.write(INTC + 0x10, 2)


def registers(probe: GPIOProbe, base: int, name: str) -> None:
    probe.reset(base, name)
    for offset, expected in ((0, REVISION), (0x10, 0), (0x20, 0), (0x114, 1),
                             (0x130, 2), (0x134, 0xFFFFFFFF), (0x138, 0), (0x13C, 0)):
        probe.read(base + offset, expected, "reset register")
    for offset in (0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C, 0x40,
                   0x140, 0x144, 0x148, 0x14C, 0x150, 0x154, 0x190, 0x194):
        probe.read(base + offset, 0, "reset register")
    for offset, expected in ((0, REVISION), (0x114, 1), (0x138, 0)):
        probe.write(base + offset, 0xFFFFFFFF)
        probe.read(base + offset, expected, "read-only register")
    probe.write(base + 0x10, 0xFFFFFFFD)
    probe.read(base + 0x10, 0x19, "SYSCONFIG writable fields")
    probe.write(base + 0x130, 0xFFFFFFFE)
    probe.read(base + 0x130, 6, "CTRL writable fields")
    probe.write(base + 0x154, 0xFFFFFFFF)
    probe.read(base + 0x154, 0xFF, "debounce time field (timing not tested)")
    probe.write(base + 0x150, 0x80000001)
    probe.read(base + 0x150, 0x80000001, "debounce enable storage (filter not modeled)")

    # Generated lanes prove adjacent direct-write data is preserved.
    probe.write(base + 0x13C, 0)
    for lane in range(4):
        probe.write(base + 0x13C + lane, 0x11 * (lane + 1), "b")
    probe.read(base + 0x13C, 0x44332211, "byte lane writes")
    probe.write(base + 0x13E, 0xA5C3, "w")
    probe.read(base + 0x13C, 0xA5C32211, "halfword preserves lower lanes")
    probe.read(base + 0x13D, 0x22, "byte lane read", "b")
    probe.read(base + 0x13E, 0xA5C3, "halfword lane read", "w")
    probe.write(base + 0x190, 0xA5000011)
    probe.write(base + 0x197, 0x80, "b")
    for offset in (0x13C, 0x190, 0x194):
        probe.read(base + offset, 0x80C32200, "set/clear read aliases")
    probe.write(base + 0x192, 0xC3, "b")
    probe.read(base + 0x13C, 0x80002200, "partial clear alias")
    probe.write(base + 0x134, 0)
    probe.inputs(name, 0x80000001)
    probe.read(base + 0x138, 0x80000001, "pad input independent of OE/DATAOUT")
    probe.write(base + 0x134, 0xFFFFFFFF)
    probe.read(base + 0x138, 0x80000001, "OE changes do not invent input feedback")


def events(probe: GPIOProbe, base: int, name: str) -> None:
    probe.reset(base, name)
    probe.write(base + 0x148, 0xFFFFFFFF)
    # Every pin, including sign-bit 31, must latch in both independent lines.
    for pin in range(32):
        bit = 1 << pin
        probe.pin(name, pin, True)
        probe.read(base + 0x138, bit, "generated input capture")
        for offset in (0x24, 0x28):
            probe.read(base + offset, bit, "masked edge raw status")
        for offset in (0x2C, 0x30):
            probe.read(base + offset, 0, "disabled event masked view")
        # W1C clears a raw latch even while the interrupt is masked.
        probe.write(base + 0x2C, bit)
        probe.read(base + 0x24, 0, "independent line-0 W1C")
        probe.read(base + 0x28, bit, "line-1 latch survives line-0 ack")
        probe.write(base + 0x30, bit)
        probe.pin(name, pin, False)
        probe.read(base + 0x24, 0, "falling not configured")
    probe.write(base + 0x148, 0)
    probe.write(base + 0x14C, 0x80000001)
    probe.inputs(name, 0x80000001)
    probe.read(base + 0x24, 0, "rising not configured")
    probe.inputs(name, 0)
    probe.read(base + 0x24, 0x80000001, "falling detector")
    probe.write(base + 0x2C, 0x80000001)
    probe.write(base + 0x30, 0x80000001)
    probe.write(base + 0x14C, 0)

    # Input capture/event detection pauses while DISABLEMODULE is set.
    probe.write(base + 0x148, 0x80)
    probe.write(base + 0x130, 3)
    probe.pin(name, 7, True)
    probe.read(base + 0x138, 0, "disabled capture holds sample")
    probe.read(base + 0x24, 0, "disabled event path")
    probe.write(base + 0x130, 2)
    probe.read(base + 0x138, 0x80, "capture resumes")
    probe.read(base + 0x24, 0x80, "resumed edge detector")


def interrupt_routes(probe: GPIOProbe, base: int, name: str, first_irq: int) -> None:
    for line in range(2):
        probe.reset(base, name)
        irq = first_irq + line
        raw, status = base + 0x24 + 4 * line, base + 0x2C + 4 * line
        enable, disable = base + 0x34 + 4 * line, base + 0x3C + 4 * line
        probe.write(raw + 3, 0x80, "b")
        probe.read(raw, 0x80000000, "software raw W1S byte lane")
        probe.read(status, 0, "software event initially masked")
        probe.read(INTC + 0xE0, 0, "disabled GPIO does not drive INTC")
        probe.write(enable + 3, 0x80, "b")
        probe.read(disable, 0x80000000, "enable aliases")
        probe.read(INTC + 0xE0, 1 << (irq - 96), "public MPU IRQ mapping")
        probe.write(INTC + 0xE8, 1 << (irq - 96))
        probe.output(0, True, "MPU IRQ")
        probe.read(INTC + 0x40, irq, "MPU source arbitration")
        probe.write(base + 0x20, line)
        probe.read(raw, 0x80000000, "EOI does not clear event source")
        probe.read(base + 0x20, 0, "EOI has no memory")
        probe.write(disable + 3, 0x80, "b")
        probe.read(raw, 0x80000000, "disable preserves raw event")
        probe.read(status, 0, "disable masks pending event")
        probe.read(INTC + 0xE0, 0, "mask deasserts mapped source")
        probe.write(INTC + 0x48, 1)
        probe.output(0, False, "MPU IRQ after controller acknowledgement")
        probe.write(enable, 0x80000000)
        probe.output(0, True, "re-enabled raw event")
        probe.write(status + 3, 0x80, "b")
        probe.read(raw, 0, "W1C source ack")
        probe.read(INTC + 0xE0, 0, "acked GPIO source drops")
        probe.write(INTC + 0x48, 1)
        probe.output(0, False, "MPU IRQ after source ack")
        probe.read(INTC + 0x40, 0xFFFFFF80, "no pending source")
        probe.write(raw, 0)
        probe.read(raw, 0, "raw W1S zero has no effect")


def levels(probe: GPIOProbe, base: int, name: str, first_irq: int) -> None:
    probe.reset(base, name)
    bit = 1 << 5
    probe.write(base + 0x140, bit)
    for offset in (0x24, 0x28):
        probe.read(base + offset, bit, "low level latches while masked")
    for offset in (0x34, 0x38):
        probe.write(base + offset, bit)
    probe.read(INTC + 0xE0, 3 << (first_irq - 96), "two independent enabled output lines")
    for offset in (0x2C, 0x30):
        probe.write(base + offset, bit)
        probe.read(base + offset, bit, "active level reasserts after ack")
    probe.pin(name, 5, True)
    for offset in (0x2C, 0x30):
        probe.read(base + offset, bit, "level event remains latched until ack")
        probe.write(base + offset, bit)
        probe.read(base + offset, 0, "deasserted level ack releases")
    probe.read(INTC + 0xE0, 0, "both mapped sources released")
    probe.write(base + 0x140, 0)
    probe.write(base + 0x144, bit)
    probe.read(base + 0x24, bit, "high level detection")
    probe.write(base + 0x140, bit)
    probe.pin(name, 5, False)
    for offset in (0x2C, 0x30):
        probe.write(base + offset, bit)
        probe.read(base + offset, bit, "both level detectors produce constant request")
    probe.write(base + 0x140, 0)
    probe.write(base + 0x144, 0)
    for offset in (0x2C, 0x30):
        probe.write(base + offset, bit)
        probe.read(base + offset, 0, "clear disabled level detector")
    # Full software reset clears configuration, source latches and IRQs.
    probe.write(base + 0x13C, 0xFFFFFFFF)
    probe.write(base + 0x134, 0)
    probe.write(base + 0x24, 0xFFFFFFFF)
    probe.write(base + 0x10, 2)
    for offset in (0x24, 0x28, 0x34, 0x38, 0x13C, 0x140, 0x144, 0x148, 0x14C):
        probe.read(base + offset, 0, "software reset clears state")
    probe.read(base + 0x134, 0xFFFFFFFF, "reset restores all inputs")
    probe.read(INTC + 0xE0, 0, "reset deasserts GPIO interrupt outputs")


def outputs(qemu: Path, base: int, name: str, group: str) -> None:
    with qtest_only(qemu, "ti8168-mediaboard", "aula-gpio-output-") as stream:
        probe = GPIOProbe(stream, f"/machine/{name}", group)
        probe.write(base + 0x10, 2)
        for pin in range(32):
            bit = 1 << pin
            if group == "gpio-out":
                probe.write(base + 0x194, bit)
                probe.output(pin, True, "output latch")
                probe.write(base + 0x190, bit)
                probe.output(pin, False, "cleared output latch")
            else:
                probe.write(base + 0x134, 0xFFFFFFFF ^ bit)
                probe.output(pin, True, "enabled output driver")
                probe.write(base + 0x134, 0xFFFFFFFF)
                probe.output(pin, False, "input has output driver disabled")
        probe.write(base + (0x13C if group == "gpio-out" else 0x134),
                    0xFFFFFFFF if group == "gpio-out" else 0)
        probe.write(base + 0x10, 2)
        for pin in range(32):
            probe.output(pin, False, "reset output signal")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: probe_gpio.py /path/to/qemu-system-arm")
    qemu = Path(sys.argv[1]).resolve(strict=True)
    with qtest_only(qemu, "ti8168-mediaboard", "aula-gpio-") as stream:
        probe = GPIOProbe(stream, "/machine/intc", "sysbus-irq")
        for base, name, first_irq in BANKS:
            registers(probe, base, name)
            events(probe, base, name)
            interrupt_routes(probe, base, name, first_irq)
            levels(probe, base, name, first_irq)
    for base, name, _ in BANKS:
        for group in ("gpio-out", "gpio-oe"):
            outputs(qemu, base, name, group)
    print("TI8168 GPIO: both banks, 64 inputs/outputs/directions, byte/halfword lanes, "
          "raw/masked status, all four MPU IRQ routes, edges/levels, ack/reset passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
