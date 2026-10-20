#!/usr/bin/env python3
"""Generated public SPRUGX8C WDT MMIO/time/IRQ/reset checks; no firmware input."""
from __future__ import annotations

import argparse
import hashlib
import json
import socket
from pathlib import Path

from probe_support import qtest_pair, recv_line, send_json

BASE = 0x480C2000
FREQUENCY = 32768


def duration(clocks: int) -> int:
    return (clocks * 1_000_000_000 + FREQUENCY - 1) // FREQUENCY


class Probe:
    def __init__(self, qtest: socket.socket, qmp: socket.socket):
        self.qtest, self.qmp = qtest, qmp
        self.irq = False
        self.assertions = 0
        self.events: list[dict] = []
        self.command("irq_intercept_out /machine/watchdog sysbus-irq")

    def command(self, value: str) -> str:
        self.qtest.sendall(value.encode() + b"\n")
        for _ in range(128):
            reply = recv_line(self.qtest)
            if reply.startswith("IRQ "):
                _, action, number = reply.split()
                if number != "0" or action not in ("raise", "lower"):
                    raise RuntimeError(f"unexpected WDT IRQ: {reply}")
                self.irq = action == "raise"
            elif reply == "OK" or reply.startswith("OK "):
                return reply
            else:
                raise RuntimeError(f"{value}: {reply}")
        raise RuntimeError("excessive asynchronous qtest events")

    def qmp_command(self, name: str, arguments: dict | None = None) -> dict:
        request: dict = {"execute": name}
        if arguments is not None:
            request["arguments"] = arguments
        send_json(self.qmp, request)
        for _ in range(128):
            reply = json.loads(recv_line(self.qmp))
            if "event" in reply:
                self.events.append(reply)
            elif "return" in reply:
                return reply
            else:
                raise RuntimeError(f"QMP {name}: {reply!r}")
        raise RuntimeError("excessive asynchronous QMP events")

    def check(self, actual: object, expected: object, label: str) -> None:
        self.assertions += 1
        if actual != expected:
            raise RuntimeError(f"{label}: expected {expected!r}, got {actual!r}")

    def read(self, offset: int, size: int = 4) -> int:
        return int(self.command(f"read{'l' if size == 4 else 'w'} "
                                f"{BASE + offset:#x}").split()[1], 0)

    def write(self, offset: int, value: int, size: int = 4) -> None:
        self.command(f"write{'l' if size == 4 else 'w'} {BASE + offset:#x} {value:#x}")

    def reg(self, offset: int, expected: int, label: str) -> None:
        self.check(self.read(offset), expected, label)

    def step(self, ns: int) -> None:
        self.command(f"clock_step {ns}")

    def completed(self, offset: int, value: int, size: int = 4) -> None:
        self.write(offset, value, size)
        self.reg(0x34, 0, "nonposted functional write completes before readback")

    def disable(self) -> None:
        self.completed(0x48, 0xCAFEAAAA)
        self.completed(0x48, 0xBEEF5555)

    def enable(self) -> None:
        self.completed(0x48, 0xBBBB)
        self.completed(0x48, 0x4444)

    def reset(self) -> None:
        self.write(0x10, 2)
        self.reg(0x14, 1, "nonposted software reset completes before readback")
        self.reg(0x10, 0, "software-reset bit self-clears")
        self.check(self.irq, False, "reset lowers IRQ")


def defaults(p: Probe) -> None:
    for offset, expected in ((0, 0), (0x10, 0), (0x14, 1), (0x18, 0),
                             (0x1C, 0), (0x24, 0x20), (0x28, 0),
                             (0x2C, 0xFFFFFFBE), (0x30, 0), (0x34, 0),
                             (0x44, 0), (0x48, 0), (0x54, 0), (0x58, 0)):
        p.reg(offset, expected, f"reset register {offset:#x}")
    p.step(1_000_000_000)
    p.reg(0x28, 0, "reset state enabled but not running (22.1.1)")
    p.completed(0x2C, 7)
    p.reg(0x2C, 0xFFFFFFBE, "configuration requires disable sequence")
    p.completed(0x48, 0x5555)
    p.completed(0x2C, 8)
    p.reg(0x2C, 0xFFFFFFBE, "second magic alone cannot disable")
    p.completed(0x48, 0xAAAA)
    p.reg(0x48, 0xAAAA, "nonposted WSPR readback is current")
    p.completed(0x48, 0x1234)
    p.completed(0x48, 0x5555)
    p.completed(0x2C, 9)
    p.reg(0x2C, 0xFFFFFFBE, "invalid intervening magic prevents disable")
    p.disable()
    p.completed(0x2C, 0x12345678)
    p.reg(0x2C, 0x12345678, "full disable sequence permits configuration")
    p.completed(0x2E, 0xABCD, 2)
    p.reg(0x2C, 0xABCD5678, "upper-half write preserves lower bits")
    p.completed(0x2C, 0xEF01, 2)
    p.reg(0x2C, 0xABCDEF01, "lower-half write preserves upper bits")
    p.reset()


def counter(p: Probe) -> None:
    p.disable()
    p.completed(0x24, 0x28)  # PRE=1, PTV=2 => divide by four (not eight).
    p.command(f"writeb {BASE + 0x24:#x} 0")
    p.reg(0x24, 0x28, "unsupported byte write cannot alter functional register")
    p.completed(0x2C, 0x200)
    p.completed(0x28, 0x123)
    p.completed(0x30, 1)
    p.reg(0x28, 0x200, "changed trigger reloads while disabled")
    p.step(duration(20))
    p.reg(0x28, 0x200, "disabled counter does not run")
    p.completed(0x48, 0xBBBB)
    p.step(duration(4))
    p.reg(0x28, 0x200, "first enable magic alone cannot start")
    p.completed(0x48, 0x4444)
    p.step(duration(12))
    p.reg(0x28, 0x203, "prescaler uses 2**PTV")
    p.enable()
    p.reg(0x28, 0x203, "re-enabling a running counter does not rewind it")
    p.completed(0x30, 1)
    p.check(p.read(0x28) >= 0x203, True, "unchanged trigger cannot reload")
    p.completed(0x30, 2)
    p.reg(0x28, 0x200, "changed trigger reloads a running counter")
    p.completed(0x2C, 1)
    p.reg(0x2C, 0x200, "running configuration write rejected")
    p.disable()
    frozen = p.read(0x28)
    p.step(duration(50))
    p.reg(0x28, frozen, "disable freezes count")
    p.reset()


def delay_irq(p: Probe) -> None:
    p.disable()
    p.completed(0x2C, 100)
    p.completed(0x44, 103)
    p.completed(0x30, 1)
    p.enable()
    p.step(duration(4) - 1)
    p.reg(0x54, 0, "delay cannot occur before documented +1 edge")
    p.step(1)
    p.reg(0x54, 2, "delay latches raw status while masked")
    p.reg(0x18, 2, "legacy WISR reports raw status")
    p.reg(0x58, 0, "masked status excludes disabled event")
    p.check(p.irq, False, "masked delay does not assert IRQ")
    p.write(0x5C, 2)
    p.reg(0x58, 2, "enabling existing status exposes pending IRQ")
    p.check(p.irq, True, "enabled pending delay asserts output")
    p.write(0x58, 0)
    p.reg(0x54, 2, "W1C zero leaves status unchanged")
    p.write(0x18, 2)
    p.reg(0x54, 0, "legacy WISR W1C clears raw status")
    p.check(p.irq, False, "acknowledge lowers IRQ")
    p.write(0x54, 1)
    p.write(0x1C, 1)
    p.reg(0x58, 1, "debug raw overflow and legacy WIER")
    p.check(p.irq, True, "debug raw overflow asserts enabled IRQ")
    p.write(0x1E, 0xFFFF, 2)
    p.reg(0x1C, 1, "upper-half WIER write preserves lower enable")
    p.write(0x60, 1)
    p.reg(0x54, 1, "masking preserves raw event")
    p.check(p.irq, False, "masking lowers IRQ")
    p.reset()


def halfword_shadow(p: Probe) -> None:
    p.disable()
    p.completed(0x2C, 0)
    p.completed(0x28, 0x89AB1234)
    p.check(p.read(0x28, 2), 0x1234, "low-half read captures disabled counter")
    p.completed(0x28, 0xFFFE)
    p.check(p.read(0x2A, 2), 0x89AB, "counter write does not refresh read shadow")
    p.reg(0x28, 0xFFFE, "live word read sees written counter")
    p.check(p.read(0x2A, 2), 0x89AB, "live word read preserves previous shadow")
    p.enable()
    p.check(p.read(0x28, 2), 0xFFFE, "LSB read captures shadow")
    p.step(duration(3))
    p.reg(0x28, 0x10001, "32-bit reads live counter")
    p.check(p.read(0x2A, 2), 0, "MSB uses shadow despite intervening 32-bit read")
    p.check(p.read(0x28, 2), 1, "next LSB refreshes shadow")
    p.check(p.read(0x2A, 2), 1, "refreshed upper-half shadow")
    p.reset()


def overflow(p: Probe) -> None:
    p.qmp_command("watchdog-set-action", {"action": "none"})
    p.disable()
    p.completed(0x2C, 0xFFFFFFF0)
    p.completed(0x28, 0xFFFFFFFC)
    p.write(0x5C, 1)
    p.enable()
    p.step(duration(4) - 1)
    p.reg(0x54, 0, "overflow cannot occur early")
    p.step(1)
    p.reg(0x28, 0xFFFFFFF0, "overflow automatically reloads WLDR")
    p.reg(0x54, 1, "overflow latches raw status")
    p.check(p.irq, True, "overflow asserts IRQ independently of reset action")
    p.qmp_command("query-status")
    p.check([e["data"]["action"] for e in p.events if e.get("event") == "WATCHDOG"],
            ["none"], "actual overflow emits WATCHDOG")
    p.reset()


def reset_expiry(p: Probe, *, special: bool) -> None:
    # A fresh process avoids QMP's documented real-time WATCHDOG rate limiting.
    p.disable()
    if special:
        p.completed(0x2C, 0xFFFFFFFF)
        p.completed(0x30, 1)
        clocks = 1
    else:
        p.completed(0x2C, 0xFFFFFFF0)
        p.completed(0x28, 0xFFFFFFFC)
        p.enable()
        clocks = 4
    p.step(duration(clocks) - 1)
    p.reg(0x54, 0, "watchdog cannot reset before the documented edge")
    p.step(1)
    # query-status drains all preceding asynchronous events, preserving them.
    p.qmp_command("query-status")
    kinds = [e.get("event") for e in p.events]
    p.check(kinds.count("WATCHDOG"), 1, "one watchdog expiry event")
    p.check(kinds.count("RESET"), 1, "actual machine RESET event")
    p.check(kinds.index("WATCHDOG") < kinds.index("RESET"), True,
            "watchdog event precedes machine reset")
    wd = next(e for e in p.events if e.get("event") == "WATCHDOG")
    reset = next(e for e in p.events if e.get("event") == "RESET")
    p.check(wd["data"], {"action": "reset"}, "reset originates from watchdog action")
    p.check(reset["data"], {"guest": True, "reason": "guest-reset"},
            "watchdog requests guest machine reset")
    p.reg(0x2C, 0xFFFFFFBE, "machine reset restores reset-context load")
    p.reg(0x28, 0, "machine reset restores initially stopped count")
    p.reg(0x34, 0, "machine reset leaves no pending writes")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("--json", type=Path, help="write machine-readable public-only receipt")
    args = parser.parse_args()
    qemu = args.qemu.resolve(strict=True)
    assertions = 0
    reset_events: list[dict] = []
    scenarios = ("register-counter-irq", "normal-overflow-reset", "disabled-max-load-reset")
    for scenario in scenarios:
        with qtest_pair(qemu, "ti8168-mediaboard", "aula-wdt-", "watchdog probe") as (qt, qmp):
            p = Probe(qt, qmp)
            if scenario == "register-counter-irq":
                for check in (defaults, counter, delay_irq, halfword_shadow, overflow):
                    check(p)
            else:
                reset_expiry(p, special=scenario == "disabled-max-load-reset")
                reset_events.extend({"scenario": scenario, **event} for event in p.events)
            assertions += p.assertions
    report = {"schema": "aula-ti8168-watchdog-probe/v1", "status": "pass",
              "reference": "TI SPRUGX8C chapters 1 and 22", "firmware_input": False,
              "clock_hz": FREQUENCY, "assertions": assertions, "scenarios": scenarios,
              "qemu_sha256": hashlib.sha256(qemu.read_bytes()).hexdigest(),
              "probe_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "reset_events": reset_events,
              "limitations": ["No WDTINT MPU routing documented in Table 1-68",
                              "No board timing, external WD_OUT, JTAG or PRCM reset-cause proof"]}
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
