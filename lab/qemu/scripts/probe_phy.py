#!/usr/bin/env python3
"""Check caller-configured MDIO PHY addresses without guest or network input."""
from __future__ import annotations

import socket
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

from probe_support import (
    listener,
    qtest_arguments,
    qtest_command,
    recv_line,
    terminate_process,
)

MDIO = 0x4A100800
GO, WRITE, ACK = 1 << 31, 1 << 30, 1 << 29


def options(addresses: tuple[int, int] | None) -> list[str]:
    if addresses is None:
        return []
    return [item for index, address in enumerate(addresses)
            for item in ("-global", f"ti8168-emac.phy{index}-address={address}")]


@contextmanager
def phy_process(qemu: Path, addresses: tuple[int, int] | None) -> Iterator[socket.socket]:
    with tempfile.TemporaryDirectory(prefix="aula-phy-") as temporary:
        path = Path(temporary) / "qtest.sock"
        with listener(path) as server, tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen(
                qtest_arguments(qemu, "ti8168-mediaboard", path, None) + options(addresses),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=log, text=True,
            )
            try:
                with server.accept()[0] as stream:
                    stream.settimeout(5)
                    yield stream
            except (OSError, RuntimeError, ValueError) as error:
                log.seek(0)
                raise RuntimeError(f"{error}\nQEMU stderr: {log.read()}") from error
            finally:
                terminate_process(process)


def read(stream: socket.socket, address: int) -> int:
    stream.sendall(f"readl {address:#x}\n".encode())
    reply = recv_line(stream).split()
    if len(reply) != 2 or reply[0] != "OK":
        raise RuntimeError(f"invalid register read response: {reply}")
    return int(reply[1], 16)


def access(stream: socket.socket, phy: int, register: int, value: int = 0,
           *, write: bool = False) -> int:
    command = GO | (WRITE if write else 0) | phy << 16 | register << 21 | value
    qtest_command(stream, f"writel {MDIO + 0x80:#x} {command:#x}")
    result = read(stream, MDIO + 0x80)
    if result & GO:
        raise RuntimeError("MDIO GO did not clear")
    return result


def probe_addresses(qemu: Path, addresses: tuple[int, int] | None) -> None:
    expected = addresses if addresses is not None else (0, 1)
    with phy_process(qemu, addresses) as stream:
        mask = (1 << expected[0]) | (1 << expected[1])
        for offset in (8, 12):
            if read(stream, MDIO + offset) != mask:
                raise RuntimeError(f"MDIO ALIVE/LINK does not match {expected}")
        for index, phy in enumerate(expected):
            result = access(stream, phy, 2, 0xFFFF)
            if result & (ACK | 0xFFFF) != ACK | 0x2000:
                raise RuntimeError(f"configured PHY {phy} did not acknowledge: {result:#x}")
            access(stream, phy, 7, 0xAB00 + index, write=True)
        for index, phy in enumerate(expected):
            result = access(stream, phy, 7)
            if result & (ACK | 0xFFFF) != ACK | (0xAB00 + index):
                raise RuntimeError("configured PHY register files are not independent")
        for phy in range(32):
            if phy in expected:
                continue
            result = access(stream, phy, 2)
            if result & ACK or result & 0xFFFF != 0xFFFF:
                raise RuntimeError(f"absent PHY {phy} unexpectedly acknowledged: {result:#x}")
            result = access(stream, phy, 7, 0x1234, write=True)
            if result & ACK:
                raise RuntimeError(f"absent PHY {phy} acknowledged a write")
        # EMAC software reset preserves configured addressing.
        qtest_command(stream, "writel 0x4a101174 1")
        if read(stream, MDIO + 8) != mask or read(stream, MDIO + 12) != mask:
            raise RuntimeError("EMAC reset changed configured PHY addresses")
        for phy in expected:
            if access(stream, phy, 7) & (ACK | 0xFFFF) != ACK:
                raise RuntimeError("EMAC reset did not reset configured PHY state")


def run_phy_probes(qemu: Path) -> None:
    for addresses in (None, (5, 23), (23, 5), (0, 31)):
        probe_addresses(qemu, addresses)
    for addresses in ((0, 0), (31, 31), (32, 1), (0, 32), (0xFFFFFFFF, 1)):
        result = subprocess.run(
            [str(qemu), "-machine", "ti8168-mediaboard", "-accel", "qtest",
             "-display", "none", "-serial", "none", "-nic", "none"] + options(addresses),
            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5, check=False,
        )
        if result.returncode == 0 or "distinct values from 0 to 31" not in result.stderr:
            raise RuntimeError(f"invalid PHY addresses {addresses} not rejected: {result.stderr}")
    print("Configured PHY addresses, MDIO masks/ACK/reset and invalid-input probes passed.")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise SystemExit(f"usage: {Path(sys.argv[0]).name} QEMU_SYSTEM_ARM")
        run_phy_probes(Path(sys.argv[1]).resolve(strict=True))
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
