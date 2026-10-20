#!/usr/bin/env python3
"""Verify caller-selected DDR and ARM boot parameters with generated code only."""

from __future__ import annotations

import argparse
import json
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

from probe_support import (
    check_exit,
    listener,
    qmp_hmp,
    qmp_response,
    qtest_arguments,
    qtest_command,
    recv_line,
    send_json,
    terminate_process,
)

DDR_BASE = 0x80000000
MIB = 1 << 20
GIB = 1 << 30


@contextmanager
def machine(qemu: Path, ram: str, properties: str = "", kernel: Path | None = None,
            *, execute: bool = False) -> Iterator[tuple[socket.socket, socket.socket]]:
    with tempfile.TemporaryDirectory(prefix="aula-boot-probe-") as directory:
        root = Path(directory)
        qtest_path, qmp_path = root / "qtest.sock", root / "qmp.sock"
        with listener(qtest_path) as qtest_server, listener(qmp_path) as qmp_server:
            arguments = qtest_arguments(qemu, "ti8168-mediaboard" + properties, qtest_path, qmp_path)
            arguments.extend(("-m", ram))
            if execute:
                arguments[arguments.index("qtest")] = "tcg"
                arguments.append("-S")
            if kernel is not None:
                arguments.extend(("-kernel", str(kernel), "-append", "aula_boot_probe=1"))
            with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as errors:
                process = subprocess.Popen(arguments, stdin=subprocess.DEVNULL,
                                           stdout=subprocess.DEVNULL, stderr=errors, text=True)
                try:
                    with qtest_server.accept()[0] as qtest, qmp_server.accept()[0] as qmp:
                        qtest.settimeout(5)
                        qmp.settimeout(5)
                        json.loads(recv_line(qmp))
                        send_json(qmp, {"execute": "qmp_capabilities"})
                        qmp_response(qmp)
                        yield qtest, qmp
                        send_json(qmp, {"execute": "quit"})
                        qmp_response(qmp)
                    check_exit(process, "boot probe QEMU", errors)
                finally:
                    terminate_process(process)


def read_word(qtest: socket.socket, address: int) -> int:
    qtest.sendall(f"readl {address:#x}\n".encode())
    result = recv_line(qtest)
    if not re.fullmatch(r"OK 0x[0-9a-fA-F]+", result):
        raise RuntimeError(f"unexpected memory response: {result}")
    return int(result.split()[1], 16)


def check_ram(qemu: Path) -> None:
    for ram, size in (("512M", 512 * MIB), ("1G", GIB), ("2G", 2 * GIB)):
        addresses = [DDR_BASE, DDR_BASE + size - 1]
        if size == 2 * GIB:
            addresses.insert(1, DDR_BASE + GIB)
        with machine(qemu, ram) as (qtest, _):
            for index, address in enumerate(addresses):
                qtest_command(qtest, f"writeb {address:#x} {0x31 + index:#x}")
            for index, address in enumerate(addresses):
                qtest_command(qtest, f"readb {address:#x}", f"OK 0x{0x31 + index:016x}")


def guest_code(report: int) -> bytes:
    # ARM32: load report address, save incoming r0/r1/r2, publish completion,
    # then spin. All instructions and images are generated, without a toolchain.
    return struct.pack("<7I", 0xE59F3010, 0xE8830007, 0xE3A040A5,
                       0xE583400C, 0xEAFFFFFE, 0xE1A00000, report)


def make_image(path: Path, kind: str, entry: int, report: int) -> None:
    payload = guest_code(report)
    if kind == "raw":
        image = payload
    elif kind == "uimage":
        header = struct.pack("!7I4B32s", 0x27051956, 0, 0, len(payload), entry,
                             entry, zlib.crc32(payload), 5, 2, 2, 0, b"Aula generated boot probe")
        header = header[:4] + struct.pack("!I", zlib.crc32(header)) + header[8:]
        image = header + payload
    elif kind == "elf":
        ident = b"\x7fELF\x01\x01\x01" + bytes(9)
        header = struct.pack("<16sHH5I6H", ident, 2, 40, 1, entry, 52, 0,
                             0x05000000, 52, 32, 1, 0, 0, 0)
        program = struct.pack("<8I", 1, 0x100, entry, entry, len(payload), len(payload), 7, 4)
        image = (header + program).ljust(0x100, b"\0") + payload
    else:
        raise ValueError(f"unsupported generated image: {kind}")
    path.write_bytes(image)


def check_boot(qemu: Path, directory: Path, ram: str, boot_start: int,
               boot_size: int, machine_id: int, *, custom: bool) -> None:
    properties = (f",boot-ram-start={boot_start:#x},boot-ram-size={boot_size:#x},"
                  f"boot-machine-id={machine_id:#x}") if custom else ""
    report = boot_start + 0x4000
    for kind in ("raw", "uimage", "elf"):
        # ELF remains an independent caller-defined entry, even when it lies
        # outside a shifted Linux boot window but within mapped physical DDR.
        entry = DDR_BASE + 0x30000 if kind == "elf" else boot_start + 0x10000
        kernel = directory / f"{ram}-{kind}-{machine_id:x}.bin"
        make_image(kernel, kind, entry, report)
        with machine(qemu, ram, properties, kernel, execute=True) as (qtest, qmp):
            initial = qmp_hmp(qmp, "info registers")
            expected_pc = entry if kind == "elf" else boot_start
            if not re.search(rf"R15={expected_pc:08x}\b", initial, re.IGNORECASE):
                raise RuntimeError(f"{kind} initial PC does not match {expected_pc:#x}: {initial}")
            send_json(qmp, {"execute": "cont"})
            qmp_response(qmp)
            deadline = time.monotonic() + 3
            while read_word(qtest, report + 12) != 0xA5:
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"generated {kind} guest did not execute")
                time.sleep(0.005)
            send_json(qmp, {"execute": "stop"})
            qmp_response(qmp)
            if kind == "elf":
                continue
            values = [read_word(qtest, report + index * 4) for index in range(3)]
            if values != [0, machine_id, boot_start + 0x100]:
                raise RuntimeError(f"{kind} boot registers mismatch: {values!r}")
            cursor = values[2]
            memory_tags: list[tuple[int, int]] = []
            for _ in range(32):
                words, tag = read_word(qtest, cursor), read_word(qtest, cursor + 4)
                if words == 0:
                    break
                if words < 2 or words > 1024:
                    raise RuntimeError("invalid generated ATAG extent")
                if tag == 0x54410002:
                    memory_tags.append((read_word(qtest, cursor + 12), read_word(qtest, cursor + 8)))
                cursor += words * 4
            else:
                raise RuntimeError("generated ATAG list did not terminate")
            if memory_tags != [(boot_start, boot_size)]:
                raise RuntimeError(f"{kind} ATAG_MEM mismatch: {memory_tags!r}")


def check_rejections(qemu: Path) -> None:
    cases = [(ram, "") for ram in ("256M", "768M", "3G")]
    cases.extend(("2G", properties) for properties in (
        ",boot-ram-start=0x7ffff000,boot-ram-size=0x1000",
        ",boot-ram-start=0x100000000,boot-ram-size=0x1000",
        ",boot-ram-start=0xfffff000,boot-ram-size=0x2000",
        ",boot-ram-start=0x80000001,boot-ram-size=0x1000",
        ",boot-ram-size=0xffffffffffffffff",
        ",boot-ram-size=0x1001",
        ",boot-machine-id=0x100000000",
    ))
    for ram, properties in cases:
        result = subprocess.run([str(qemu), "-machine", "ti8168-mediaboard" + properties,
                                 "-m", ram, "-accel", "qtest", "-display", "none",
                                 "-serial", "none", "-nic", "none"],
                                check=False, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
        if result.returncode == 0 or not result.stderr.strip():
            raise RuntimeError(f"invalid boot/RAM configuration accepted: {ram}{properties}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    args = parser.parse_args()
    qemu = args.qemu.resolve(strict=True)
    check_ram(qemu)
    with tempfile.TemporaryDirectory(prefix="aula-generated-boot-") as directory:
        root = Path(directory)
        check_boot(qemu, root, "512M", DDR_BASE, 512 * MIB, 0, custom=False)
        check_boot(qemu, root, "1G", DDR_BASE, GIB, 0, custom=False)
        check_boot(qemu, root, "2G", DDR_BASE, 2 * GIB, 0, custom=False)
        check_boot(qemu, root, "2G", DDR_BASE + GIB, 512 * MIB, 0x1A2B3C4D, custom=True)
        check_boot(qemu, root, "2G", DDR_BASE + GIB, 512 * MIB, 0xFFFFFFFF, custom=True)
    check_rejections(qemu)
    print("DDR capacity/endpoints, raw/uImage ARM parameters, ATAG_MEM, and ELF entry probes passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
