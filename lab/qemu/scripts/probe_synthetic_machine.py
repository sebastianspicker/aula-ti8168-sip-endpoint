#!/usr/bin/env python3
"""Run the synthetic TI8168 machine against generated sparse NAND fixtures."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from probe_runtime import (
    machine_help_line,
    probe_dmm_bootstrap,
    probe_sram,
    probe_wfi_barrier,
    verify_base_registers,
)
from probe_support import qtest_command, qtest_only


GPMC = 0x50000000
GPMC_NAND_COMMAND = GPMC + 0x7C
GPMC_NAND_ADDRESS = GPMC + 0x80
GPMC_NAND_DATA = GPMC + 0x84
NAND_PAGE_SIZE = 0x800
NAND_OOB_SIZE = 0x40
NAND_PAGES_PER_BLOCK = 0x40
BANK0_OFFSET = 0
BANK0_SIZE = 0x10000000
BANK0_OOB_SIZE = 0x00800000
BACKING_MAIN = bytes((0xF3, 0xA5))
BACKING_OOB = bytes((0xB7,))
REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
FIXTURE_PARENT = REPOSITORY_ROOT / ".work" / "build"


def create_sparse_fixture(path: Path, size: int, values: tuple[tuple[int, bytes], ...]) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0)
    descriptor = os.open(path, flags, 0o600)
    try:
        os.ftruncate(descriptor, size)
        for offset, value in values:
            if os.pwrite(descriptor, value, offset) != len(value):
                raise RuntimeError(f"short fixture write: {path}")
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def command(qtest: object, value: int) -> None:
    qtest_command(qtest, f"writeb {GPMC_NAND_COMMAND:#x} {value:#x}")


def address(qtest: object, value: int) -> None:
    qtest_command(qtest, f"writeb {GPMC_NAND_ADDRESS:#x} {value & 0xFF:#x}")


def select_page(qtest: object, row: int, column: int = 0) -> None:
    command(qtest, 0x00)
    for value in (column, column >> 8, row, row >> 8, row >> 16):
        address(qtest, value)
    command(qtest, 0x30)


def expect_byte(qtest: object, row: int, column: int, expected: int) -> None:
    select_page(qtest, row, column)
    qtest_command(
        qtest,
        f"readb {GPMC_NAND_DATA:#x}",
        f"OK 0x{expected:016x}",
    )


def status(qtest: object) -> None:
    command(qtest, 0x70)
    qtest_command(qtest, f"readb {GPMC_NAND_DATA:#x}", "OK 0x00000000000000c0")


def program_byte(qtest: object, row: int, column: int, value: int) -> None:
    command(qtest, 0x80)
    for address_byte in (column, column >> 8, row, row >> 8, row >> 16):
        address(qtest, address_byte)
    qtest_command(qtest, f"writeb {GPMC_NAND_DATA:#x} {value:#x}")
    command(qtest, 0x10)
    status(qtest)


def erase_block(qtest: object, row: int) -> None:
    command(qtest, 0x60)
    for address_byte in (row, row >> 8, row >> 16):
        address(qtest, address_byte)
    command(qtest, 0xD0)
    status(qtest)


def machine_properties(main_path: Path, oob_path: Path) -> str:
    return (
        "ti8168-mediaboard,nand-profile=synthetic-read-only,nand-cow=on,"
        f"nand-mtd0-image={main_path},nand-mtd0-oob-image={oob_path}"
    )


def exercise_process(qemu: Path, machine: str, mutate: bool) -> None:
    row = BANK0_OFFSET // NAND_PAGE_SIZE
    adjacent_row = row + NAND_PAGES_PER_BLOCK
    with qtest_only(qemu, machine, "aula-synthetic-qtest-") as qtest:
        verify_base_registers(qtest)
        probe_sram(qtest)
        probe_wfi_barrier(qtest)
        probe_dmm_bootstrap(qtest)
        expect_byte(qtest, row, 0, BACKING_MAIN[0])
        expect_byte(qtest, row, 1, BACKING_MAIN[1])
        expect_byte(qtest, row, NAND_PAGE_SIZE, BACKING_OOB[0])
        expect_byte(qtest, adjacent_row, 0, 0x5A)
        expect_byte(qtest, adjacent_row, NAND_PAGE_SIZE, 0x6B)
        if mutate:
            program_byte(qtest, row, 0, 0x3F)
            expect_byte(qtest, row, 0, 0x33)
            expect_byte(qtest, row, 1, BACKING_MAIN[1])
            expect_byte(qtest, row, NAND_PAGE_SIZE, BACKING_OOB[0])
            erase_block(qtest, row)
            expect_byte(qtest, row, 0, 0xFF)
            expect_byte(qtest, row, NAND_PAGE_SIZE, 0xFF)
            expect_byte(qtest, adjacent_row, 0, 0x5A)
            expect_byte(qtest, adjacent_row, NAND_PAGE_SIZE, 0x6B)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} QEMU_SYSTEM_ARM")
    qemu = Path(sys.argv[1]).resolve(strict=True)
    FIXTURE_PARENT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="aula-synthetic-nand-", dir=FIXTURE_PARENT
    ) as name:
        fixture_dir = Path(name)
        main_path = fixture_dir / "bank0.bin"
        oob_path = fixture_dir / "bank0.oob.bin"
        create_sparse_fixture(
            main_path,
            BANK0_SIZE,
            ((0, BACKING_MAIN), (NAND_PAGES_PER_BLOCK * NAND_PAGE_SIZE, b"\x5a")),
        )
        create_sparse_fixture(
            oob_path,
            BANK0_OOB_SIZE,
            ((0, BACKING_OOB), (NAND_PAGES_PER_BLOCK * NAND_OOB_SIZE, b"\x6b")),
        )
        machine = machine_properties(main_path, oob_path)
        exercise_process(qemu, machine, mutate=True)
        exercise_process(qemu, machine, mutate=False)
        adjacent_main_offset = NAND_PAGES_PER_BLOCK * NAND_PAGE_SIZE
        adjacent_oob_offset = NAND_PAGES_PER_BLOCK * NAND_OOB_SIZE
        main_descriptor = os.open(main_path, os.O_RDONLY)
        try:
            actual_main = os.pread(main_descriptor, len(BACKING_MAIN), 0)
            actual_adjacent_main = os.pread(main_descriptor, 1, adjacent_main_offset)
        finally:
            os.close(main_descriptor)
        if actual_main != BACKING_MAIN or actual_adjacent_main != b"\x5a":
            raise RuntimeError("QEMU modified the immutable synthetic main backing")
        oob_descriptor = os.open(oob_path, os.O_RDONLY)
        try:
            actual_oob = os.pread(oob_descriptor, len(BACKING_OOB), 0)
            actual_adjacent_oob = os.pread(oob_descriptor, 1, adjacent_oob_offset)
        finally:
            os.close(oob_descriptor)
        if actual_oob != BACKING_OOB or actual_adjacent_oob != b"\x6b":
            raise RuntimeError("QEMU modified the immutable synthetic OOB backing")
    print(machine_help_line(qemu))
    print("synthetic-only machine and NAND COW qtests passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
