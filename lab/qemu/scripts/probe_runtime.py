"""Synthetic TI8168 machine probes."""

from __future__ import annotations

from pathlib import Path
import socket
import subprocess

from probe_support import qtest_command


EXPECTED_BASE_REGISTERS = (
    ("readl 0x48200000", "OK 0x0000000000000040", "INTC revision"),
    ("readl 0x48200014", "OK 0x0000000000000001", "INTC reset complete"),
    ("readl 0x48140600", "OK 0x0000000000000000", "synthetic device ID"),
    ("readl 0x50000000", "OK 0x0000000000000050", "GPMC revision"),
    ("readl 0x47400010", "OK 0x0000000000000000", "USB OTG SYSCONFIG"),
    ("readl 0x47400ffc", "OK 0x0000000000000000", "USB OTG aperture end"),
)
SRAM = 0x4020F800
SRAM_SIZE = 0x800
WFI_BARRIER = 0x40300000
WFI_BARRIER_SIZE = 0x1000
DMM = 0x4E000000
DMM_PRIORITY_OFFSETS = (0x620, 0x624, 0x634)


def machine_help_line(qemu: Path) -> str:
    output = subprocess.run(
        [str(qemu), "-machine", "help"],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    ).stdout
    line = next(
        (value for value in output.splitlines() if value.startswith("ti8168-mediaboard ")),
        None,
    )
    if line is None:
        raise RuntimeError("ti8168-mediaboard is absent from QEMU machine list")
    return line


def verify_base_registers(qtest: socket.socket) -> None:
    for command, expected, label in EXPECTED_BASE_REGISTERS:
        try:
            qtest_command(qtest, command, expected)
        except RuntimeError as exc:
            raise RuntimeError(f"{label} failed: {exc}") from exc


def probe_sram(qtest: socket.socket) -> None:
    """Verify the synthetic SRAM aperture endpoints."""
    last_byte = SRAM + SRAM_SIZE - 1

    qtest_command(qtest, f"writeb {SRAM:#x} 0x3c")
    qtest_command(qtest, f"writeb {last_byte:#x} 0xa7")
    qtest_command(qtest, f"readb {SRAM:#x}", "OK 0x000000000000003c")
    qtest_command(qtest, f"readb {last_byte:#x}",
                  "OK 0x00000000000000a7")
    qtest_command(qtest, f"writeb {SRAM:#x} 0x0")
    qtest_command(qtest, f"writeb {last_byte:#x} 0x0")


def probe_wfi_barrier(qtest: socket.socket) -> None:
    """Verify the TI81xx SRAM page touched by the idle ordering barrier."""
    last_byte = WFI_BARRIER + WFI_BARRIER_SIZE - 1

    qtest_command(qtest, f"writeb {WFI_BARRIER:#x} 0x3c")
    qtest_command(qtest, f"writeb {last_byte:#x} 0xa7")
    qtest_command(qtest, f"readb {WFI_BARRIER:#x}",
                  "OK 0x000000000000003c")
    qtest_command(qtest, f"readb {last_byte:#x}",
                  "OK 0x00000000000000a7")


def probe_dmm_bootstrap(qtest: socket.socket) -> None:
    """Verify the synthetic DMM storage words."""
    for index, offset in enumerate(DMM_PRIORITY_OFFSETS):
        value = 0x0A + index
        qtest_command(qtest, f"writel {DMM + offset:#x} {value:#x}")
        qtest_command(qtest, f"readl {DMM + offset:#x}",
                      f"OK 0x{value:016x}")
