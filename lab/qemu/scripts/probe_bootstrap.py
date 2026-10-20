#!/usr/bin/env python3
"""Check caller-supplied bootstrap state using generated, non-firmware inputs."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from probe_support import qtest_command, qtest_pair, recv_line, send_json


def check_control(qemu: Path, directory: Path) -> None:
    seed = bytearray(4096)
    struct.pack_into("<I", seed, 0x600, 0x12345678)
    struct.pack_into("<I", seed, 0x800, 0xA1B2C3D4)
    struct.pack_into("<I", seed, 0xFFC, 0x87654321)
    path = directory / "generated-control.bin"
    path.write_bytes(seed)
    machine = f"ti8168-mediaboard,control-init-file={path}"
    with qtest_pair(qemu, machine, "aula-control-", "CONTROL probe") as (qtest, qmp):
        qtest_command(qtest, "readl 0x48140600", "OK 0x0000000012345678")
        qtest_command(qtest, "readl 0x48140ffc", "OK 0x0000000087654321")
        qtest_command(qtest, "readw 0x48140800", "OK 0x000000000000c3d4")
        qtest_command(qtest, "readw 0x48140802", "OK 0x000000000000a1b2")
        qtest_command(qtest, "writew 0x48140800 0x2468")
        qtest_command(qtest, "readl 0x48140800", "OK 0x00000000a1b22468")
        qtest_command(qtest, "writew 0x48140802 0x1357")
        qtest_command(qtest, "readl 0x48140800", "OK 0x0000000013572468")
        send_json(qmp, {"execute": "system_reset"})
        reset_seen = return_seen = False
        for _ in range(64):
            response = json.loads(recv_line(qmp))
            reset_seen |= response.get("event") == "RESET"
            return_seen |= response == {"return": {}}
            if "error" in response:
                raise RuntimeError(f"reset failed: {response!r}")
            if reset_seen and return_seen:
                break
        else:
            raise RuntimeError("CONTROL reset did not complete")
        qtest_command(qtest, "readl 0x48140800", "OK 0x00000000a1b2c3d4")
    if path.read_bytes() != seed:
        raise RuntimeError("guest writes changed the caller's CONTROL input")
    with qtest_pair(qemu, "ti8168-mediaboard", "aula-control-zero-", "default CONTROL") as (qtest, _):
        qtest_command(qtest, "readl 0x48140600", "OK 0x0000000000000000")
        qtest_command(qtest, "readl 0x48140800", "OK 0x0000000000000000")


def check_alias(qemu: Path) -> None:
    # Deliberately use a generated test address, not a board boot convention.
    machine = "ti8168-mediaboard,early-uart-base=0x47000000"
    with qtest_pair(qemu, machine, "aula-uart-alias-", "UART alias") as (qtest, _):
        qtest_command(qtest, "writeb 0x4700001c 0xa5")
        qtest_command(qtest, "readb 0x4802001c", "OK 0x00000000000000a5")
        qtest_command(qtest, "writeb 0x4802001c 0x5a")
        qtest_command(qtest, "readb 0x4700001c", "OK 0x000000000000005a")


def check_rejections(qemu: Path, directory: Path) -> None:
    short = directory / "short.bin"
    long = directory / "long.bin"
    fifo = directory / "fifo"
    short.write_bytes(bytes(4095))
    long.write_bytes(bytes(4097))
    os.mkfifo(fifo)
    properties = [
        f"control-init-file={directory / 'missing.bin'}",
        f"control-init-file={directory}",
        f"control-init-file={short}",
        f"control-init-file={long}",
        f"control-init-file={fifo}",
        "early-uart-base=0x47000001",
        "early-uart-base=0xfffffff0",
        "early-uart-base=0x100000000",
        "early-uart-base=0x80000000",
        "early-uart-base=0x48020000",
        "early-uart-base=0x48140000",
    ]
    for prop in properties:
        result = subprocess.run(
            [str(qemu), "-machine", "ti8168-mediaboard," + prop,
             "-accel", "qtest", "-display", "none", "-serial", "none", "-nic", "none"],
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5,
            check=False,
        )
        if result.returncode == 0 or not result.stderr.strip():
            raise RuntimeError(f"invalid bootstrap input accepted: {prop}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    args = parser.parse_args()
    qemu = args.qemu.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="aula-bootstrap-") as directory:
        root = Path(directory)
        check_control(qemu, root)
        check_alias(qemu)
        check_rejections(qemu, root)
    print("CONTROL seed, halfword access, reset, UART alias and invalid-input probes passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
