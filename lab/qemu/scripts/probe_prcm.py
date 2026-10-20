#!/usr/bin/env python3
"""Exercise the public TI8168 timer-clock PRCM subset with generated inputs.

SPRUGX8C sections 18.7.4.26-.32 and 18.7.17.25-.31 define the register map.
The source-rate overrides below are test inputs, not a board clock snapshot.
"""
from __future__ import annotations

import json
import socket
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

from probe_hardware import INTC, TIMERS, HardwareProbe, unmask
from probe_support import (
    check_exit,
    listener,
    qmp_response,
    qtest_arguments,
    recv_line,
    send_json,
    terminate_process,
)

CLKSEL = 0x48180390
CLKCTRL = 0x48181570


@contextmanager
def process(qemu: Path, extra: tuple[str, ...] = ()) -> Iterator[tuple[HardwareProbe, socket.socket]]:
    with tempfile.TemporaryDirectory(prefix="aula-prcm-") as temporary:
        qtest_path = Path(temporary) / "qtest.sock"
        qmp_path = Path(temporary) / "qmp.sock"
        with (listener(qtest_path) as qs, listener(qmp_path) as ms,
              tempfile.TemporaryFile(mode="w+") as log):
            child = subprocess.Popen(
                qtest_arguments(qemu, "ti8168-mediaboard", qtest_path, qmp_path) + list(extra),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log, text=True,
            )
            try:
                with qs.accept()[0] as qt, ms.accept()[0] as qm:
                    qt.settimeout(5)
                    qm.settimeout(5)
                    json.loads(recv_line(qm))
                    send_json(qm, {"execute": "qmp_capabilities"})
                    if "return" not in qmp_response(qm):
                        raise RuntimeError("QMP setup failed")
                    yield HardwareProbe(qt), qm
                    send_json(qm, {"execute": "quit"})
                    if "return" not in qmp_response(qm):
                        raise RuntimeError("QMP quit failed")
                    check_exit(child, "PRCM probe", log)
            except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
                log.seek(0)
                raise RuntimeError(f"{error}\nQEMU stderr: {log.read()}") from error
            finally:
                terminate_process(child)


def cold_reset(qmp: socket.socket) -> None:
    send_json(qmp, {"execute": "system_reset"})
    returned = False
    reset_seen = False
    for _ in range(64):
        response = json.loads(recv_line(qmp))
        returned |= "return" in response
        reset_seen |= response.get("event") == "RESET"
        if "error" in response:
            raise RuntimeError(f"cold reset failed: {response}")
        if returned and reset_seen:
            return
    raise RuntimeError("missing QMP reset return/event")


def configure(probe: HardwareProbe, index: int, source: int = 2) -> int:
    probe.write(CLKSEL + index * 4, source)
    probe.write(CLKCTRL + index * 4, 2)
    return TIMERS[index][0]


def probe_registers_and_gates(probe: HardwareProbe) -> None:
    for index, (base, _) in enumerate(TIMERS):
        select = CLKSEL + index * 4
        control = CLKCTRL + index * 4
        probe.read(select, 1, "public cold-reset timer source")
        probe.read(control, 3 << 16, "disabled module and IDLEST")
        probe.read(base, 0, "disabled timer OCP access is rejected")
        probe.write(base + 0x3C, 0x12345678)
        probe.write(control, 2)
        probe.read(control, 2, "clock-enabled functional module")
        probe.read(base, 0x4FFF1301)
        probe.read(base + 0x3C, 0, "disabled write had no effect")
        # Selector1 is SYSCLK18, not the former fixed27MHz source.
        probe.write(base + 0x38, 1)
        probe.step(1_000_000_000)
        probe.read(base + 0x3C, 32768, "SYSCLK18 counter rate")
        probe.write(base + 0x38, 0)
        probe.write(select, 2)
        probe.write(select, 0xFFFFFFFF)  # Reserved selector3 must not become a source.
        probe.read(select, 2, "reserved source write ignored")
        probe.write(select, 0xFFFFFFFE)
        probe.read(select, 2, "reserved register bits read zero")
        probe.write(control, 1)
        probe.read(control, 2, "reserved MODULEMODE1 ignored")
        probe.write(control, 3)
        probe.read(control, 2, "reserved MODULEMODE3 ignored")
        probe.write(control, 0xFFFF0002)
        probe.read(control, 2, "IDLEST remains read-only")
        probe.write(base + 0x3C, 0)
        probe.write(base + 0x38, 1)
        probe.step(1000)
        probe.read(base + 0x3C, 27, "CLKIN rate")
        probe.write(control, 0)
        probe.read(control, 3 << 16)
        probe.read(base + 0x3C, 0, "OCP blocked while disabled")
        probe.write(base + 0x3C, 0xABCDEF)
        probe.step(1_000_000)
        probe.write(control, 2)
        probe.read(base + 0x3C, 27, "gating freezes counter and rejects writes")
        probe.step(1000)
        probe.read(base + 0x3C, 54)
        # Unconnected TCLKIN does not acknowledge a functional module.
        probe.write(select, 0)
        probe.read(control, (1 << 16) | 2, "missing source remains in transition")
        probe.step(1_000_000)
        probe.read(base + 0x3C, 54, "no clock means no counter progress")
        probe.write(select, 2)
        probe.read(control, 2)
        probe.step(1000)
        probe.read(base + 0x3C, 81, "returning source resumes frozen count")
        probe.write(base + 0x38, 0)


def probe_phase_and_interrupts(probe: HardwareProbe) -> None:
    base = configure(probe, 0)
    control = CLKCTRL
    probe.write(base + 0x10, 1)
    # Gate after a fractional source clock, then resume the same phase.
    probe.write(base + 0x38, 1)
    probe.step(10)
    probe.read(base + 0x3C, 0)
    probe.write(control, 0)
    probe.step(2000)
    probe.write(control, 2)
    probe.step(27)
    probe.read(base + 0x3C, 0, "fractional input phase retained across gating")
    probe.step(1)
    probe.read(base + 0x3C, 1)
    probe.write(base + 0x10, 1)
    # 243 source clocks have entered a 256-clock prescaler before gating.
    probe.write(base + 0x38, 1 | 0x20 | (7 << 2))
    probe.step(9000)
    probe.write(control, 0)
    probe.step(1000)
    probe.write(control, 2)
    probe.step(482)
    probe.read(base + 0x3C, 1, "whole prescaler progress retained across gating")
    probe.write(base + 0x10, 1)
    # Source switches preserve the count, then apply the selected frequency.
    probe.write(base + 0x38, 1)
    probe.step(1000)
    probe.write(CLKSEL, 1)
    probe.step(1_000_000_000)
    probe.read(base + 0x3C, 27 + 32768, "live source switch")
    probe.write(CLKSEL, 2)
    probe.step(1000)
    probe.read(base + 0x3C, 54 + 32768)
    probe.write(base + 0x10, 1)
    probe.write(INTC + 0x10, 2)
    unmask(probe, TIMERS[0][1])
    probe.write(base + 0x2C, 2)
    probe.write(base + 0x3C, 0xFFFFFFE5)
    probe.write(base + 0x38, 1)
    probe.step(500)
    probe.write(control, 0)
    probe.step(1_000_000_000)
    probe.outputs(False)
    probe.write(control, 2)
    probe.step(499)
    probe.outputs(False)
    probe.step(1)
    probe.read(base + 0x24, 2, "overflow waits for enabled source clocks")
    probe.outputs(True)
    # Gating does not fabricate a reset or clear a pending raw interrupt.
    probe.write(control, 0)
    probe.outputs(True)
    probe.write(control, 2)
    probe.read(base + 0x24, 2)
    probe.write(base + 0x28, 2)
    probe.write(INTC + 0x48, 1)
    probe.outputs(False)


def run_prcm_probes(qemu: Path) -> None:
    with process(qemu) as (probe, qmp):
        probe_registers_and_gates(probe)
        probe_phase_and_interrupts(probe)
        cold_reset(qmp)
        for index, (base, _) in enumerate(TIMERS):
            probe.read(CLKSEL + index * 4, 1, "cold reset restores selector")
            probe.read(CLKCTRL + index * 4, 3 << 16, "cold reset disables module")
            probe.read(base, 0)
            probe.write(CLKCTRL + index * 4, 2)
            probe.read(base + 0x3C, 0, "cold reset restores timer registers")
        # Unimplemented domains retain the unknown-register behavior.
        probe.write(0x48181000, 0xFFFFFFFF)
        probe.read(0x48181000, 0, "unimplemented PRCM domain does not become ready")
    extra = (
        "-global", "ti8168-prcm.tclkin-frequency=1000000",
        "-global", "ti8168-prcm.sysclk18-frequency=4096",
        "-global", "ti8168-prcm.clkin-frequency=12000000",
    )
    with process(qemu, extra) as (probe, _):
        for source, frequency in ((0, 1000000), (1, 4096), (2, 12000000)):
            base = configure(probe, 0, source)
            probe.write(base + 0x10, 1)
            probe.read(CLKCTRL, 2)
            probe.write(base + 0x38, 1)
            probe.step(1_000_000_000)
            probe.read(base + 0x3C, frequency, "caller-selected source input rate")
            probe.write(base + 0x38, 0)
    print("TI8168 PRCM timer source, reset, gating, phase and IRQ probes passed.")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise SystemExit(f"usage: {Path(sys.argv[0]).name} QEMU_SYSTEM_ARM")
        run_prcm_probes(Path(sys.argv[1]).resolve(strict=True))
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
