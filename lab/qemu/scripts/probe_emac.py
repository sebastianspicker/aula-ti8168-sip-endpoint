#!/usr/bin/env python3
"""Exercise synthetic TI8168 MDIO and channel-zero CPDMA on a local socketpair.

No guest image or external Ethernet connection is used. Buffers use actual
synthetic DDR physical addresses; this does not validate the model's existing
optional legacy DMA address heuristic, real PHY wiring, or real network acceptance.
The exercised path is channel zero; probe_emac_chains.py adds packet chains, with
no CRC/FCS, fragmented-packet, teardown, timing, PHY-negotiation or MDIO-completion
interrupt acceptance claim. IRQ checks observe INTC input levels, not CPU code
executing an interrupt handler. The synthetic PHY slots and IDs are fixtures.
"""

from __future__ import annotations

import json
import socket
import struct
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path

from probe_support import (
    check_exit,
    listener,
    qmp_response,
    qtest_arguments,
    qtest_command,
    recv_line,
    send_json,
    terminate_process,
)

EMAC = 0x4A100000
MDIO = EMAC + 0x800
REGS = EMAC + 0x1000
INTC_PENDING1 = 0x482000A0
RX_IRQ, TX_IRQ = 1 << (41 - 32), 1 << (42 - 32)
GO, WRITE, ACK = 1 << 31, 1 << 30, 1 << 29
SOP, EOP, OWNER, EOQ = 1 << 31, 1 << 30, 1 << 29, 1 << 28
TX_DESC, RX_DESC = EMAC + 0x2000, EMAC + 0x2020
TX_BUFFER, RX_BUFFER = 0x80010000, 0x80011000
MAC = bytes.fromhex("025449383136")


@contextmanager
def emac_process(
    qemu: Path, *, legacy_dma_alias: bool = False
) -> Iterator[tuple[socket.socket, socket.socket]]:
    with tempfile.TemporaryDirectory(prefix="aula-emac-") as name:
        directory = Path(name)
        qtest_path, qmp_path = directory / "qtest.sock", directory / "qmp.sock"
        with listener(qtest_path) as qtest_server, listener(qmp_path) as qmp_server:
            # A connected stream backend uses a network-order length prefix.
            # Only this inherited socketpair transports Ethernet bytes.
            host, backend = socket.socketpair()
            try:
                host.settimeout(5)
                arguments = qtest_arguments(
                    qemu,
                    "ti8168-mediaboard",
                    qtest_path,
                    qmp_path,
                    nic=f"socket,fd={backend.fileno()},mac=02:54:49:38:31:36",
                )
                if legacy_dma_alias:
                    arguments.extend(("-global", "ti8168-emac.legacy-dma-alias=on"))
                with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as log:
                    process = subprocess.Popen(
                        arguments,
                        pass_fds=(backend.fileno(),),
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                        stderr=log,
                        text=True,
                    )
                    backend.close()
                    try:
                        with (
                            qtest_server.accept()[0] as qtest,
                            qmp_server.accept()[0] as qmp,
                        ):
                            qtest.settimeout(5)
                            qmp.settimeout(5)
                            json.loads(recv_line(qmp))
                            send_json(qmp, {"execute": "qmp_capabilities"})
                            response = qmp_response(qmp)
                            if "return" not in response:
                                raise RuntimeError(f"QMP setup failed: {response!r}")
                            yield qtest, host
                            send_json(qmp, {"execute": "quit"})
                            qmp_response(qmp)
                        check_exit(process, "EMAC qtest", log)
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.SubprocessError,
                        ValueError,
                    ) as exc:
                        log.flush()
                        log.seek(0)
                        details = log.read().strip()
                        raise RuntimeError(f"{exc}\nQEMU stderr: {details}") from exc
                    finally:
                        terminate_process(process)
            finally:
                host.close()
                backend.close()


def readl(qtest: socket.socket, address: int) -> int:
    qtest.sendall(f"readl {address:#x}\n".encode())
    reply = recv_line(qtest).split()
    if len(reply) != 2 or reply[0] != "OK":
        raise RuntimeError(f"readl {address:#x}: {reply!r}")
    return int(reply[1], 16)


def writel(qtest: socket.socket, address: int, value: int) -> None:
    qtest_command(qtest, f"writel {address:#x} {value:#x}")


def expect(
    qtest: socket.socket, address: int, value: int, label: str, mask: int = 0xFFFFFFFF
) -> None:
    actual = readl(qtest, address)
    if actual & mask != value:
        raise RuntimeError(
            f"{label}: register {address:#x} expected {value:#x} mask {mask:#x}, got {actual:#x}"
        )


def write_bytes(qtest: socket.socket, address: int, value: bytes) -> None:
    qtest_command(qtest, f"write {address:#x} {len(value):#x} 0x{value.hex()}")


def read_bytes(qtest: socket.socket, address: int, length: int) -> bytes:
    qtest.sendall(f"read {address:#x} {length:#x}\n".encode())
    reply = recv_line(qtest)
    if not reply.startswith("OK 0x"):
        raise RuntimeError(f"read memory: {reply!r}")
    value = bytes.fromhex(reply[5:])
    if len(value) != length:
        raise RuntimeError("short qtest memory read")
    return value


def wait_register(
    qtest: socket.socket,
    address: int,
    expected: int,
    label: str,
    mask: int = 0xFFFFFFFF,
) -> None:
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        if readl(qtest, address) & mask == expected:
            return
        time.sleep(0.005)
    expect(qtest, address, expected, label, mask)


def receive_exact(stream: socket.socket, length: int) -> bytes:
    result = bytearray()
    while len(result) < length:
        data = stream.recv(length - len(result))
        if not data:
            raise RuntimeError("network backend closed before the expected frame")
        result.extend(data)
    return bytes(result)


def mdio(
    qtest: socket.socket, phy: int, reg: int, data: int = 0, write: bool = False
) -> int:
    command = GO | (WRITE if write else 0) | (reg << 21) | (phy << 16) | data
    writel(qtest, MDIO + 0x80, command)
    result = readl(qtest, MDIO + 0x80)
    if result & GO:
        raise RuntimeError("MDIO GO did not complete")
    if result & 0x03FF0000 != command & 0x03FF0000:
        raise RuntimeError("MDIO changed PHY/register fields")
    return result


def probe_mdio(qtest: socket.socket) -> None:
    writel(qtest, MDIO + 4, 1 << 30)
    expect(qtest, MDIO + 8, 3, "synthetic PHY alive slots")
    for phy in (0, 1):
        # DATA is ignored for read commands. Nonzero DATA must not leak into
        # the completed read, especially after a previous write transaction.
        for reg, value in ((2, 0x2000), (3, 0x5C90)):
            result = mdio(qtest, phy, reg, 0xFFFF)
            if not result & ACK or result & 0xFFFF != value:
                raise RuntimeError(
                    f"MDIO PHY {phy} reg {reg}: expected ACK/data {value:#x}, got {result:#x}"
                )
        mdio(qtest, phy, 7, 0x00A5, write=True)
        result = mdio(qtest, phy, 7, 0xF0F0)
        if not result & ACK or result & 0xFFFF != 0x00A5:
            raise RuntimeError(f"MDIO read retained stale command DATA: {result:#x}")
        result = mdio(qtest, phy, 0, 0x8000, write=True)
        if result & 0xFFFF != 0x8000:
            raise RuntimeError(f"MDIO write DATA changed after PHY reset: {result:#x}")
        result = mdio(qtest, phy, 0, 0xFFFF)
        if not result & ACK or result & 0xFFFF != 0:
            raise RuntimeError(
                f"synthetic PHY reset bit did not self-clear: {result:#x}"
            )
    result = mdio(qtest, 7, 2)
    if result & ACK or result & 0xFFFF != 0xFFFF:
        raise RuntimeError(
            f"absent synthetic PHY unexpectedly acknowledged: {result:#x}"
        )


def descriptor(
    qtest: socket.socket, address: int, buffer: int, length: int, mode: int
) -> None:
    write_bytes(qtest, address, struct.pack("<IIII", 0, buffer, length, mode))


def probe_cpdma(qtest: socket.socket, network: socket.socket) -> None:
    frame = MAC + bytes.fromhex("0200000000010800") + bytes(range(50))
    if len(frame) != 64:
        raise RuntimeError("invalid Ethernet fixture")
    # TX/RX data use real synthetic DDR physical addresses. No direct-map
    # address remapping or translation heuristic is used by this test.
    writel(qtest, REGS + 0x1C, 1)
    expect(qtest, REGS + 0x600, 0, "TX reset head")
    expect(qtest, REGS + 0x620, 0, "RX reset head")
    tx_offset = 8
    write_bytes(qtest, TX_BUFFER, bytes([0x5A]) * tx_offset + frame)
    descriptor(
        qtest,
        TX_DESC,
        TX_BUFFER,
        (tx_offset << 16) | len(frame),
        SOP | EOP | OWNER | len(frame),
    )
    writel(qtest, REGS + 0x88, 1)
    writel(qtest, REGS + 4, 1)
    writel(qtest, REGS + 0x600, TX_DESC)
    wait_register(qtest, REGS + 0x640, TX_DESC, "TX completion pointer")
    length = struct.unpack("!I", receive_exact(network, 4))[0]
    if length != len(frame) or receive_exact(network, length) != frame:
        raise RuntimeError("CPDMA TX bytes differ from guest buffer")
    expect(
        qtest, TX_DESC + 12, SOP | EOP | EOQ | len(frame), "TX ownership/EOQ completion"
    )
    expect(qtest, REGS + 0x600, 0, "TX consumed descriptor")
    expect(qtest, REGS + 0x80, 1, "TX raw completion")
    expect(qtest, REGS + 0x84, 1, "TX masked completion")
    expect(qtest, INTC_PENDING1, 0, "TX wrapper initially disabled", TX_IRQ)
    writel(qtest, EMAC + 0x918, 1)
    expect(qtest, INTC_PENDING1, TX_IRQ, "TX routed SoC IRQ42", TX_IRQ)
    writel(qtest, REGS + 0x8C, 1)
    expect(qtest, REGS + 0x80, 1, "TX raw remains pending with channel masked")
    expect(qtest, REGS + 0x84, 0, "TX masked status cleared")
    expect(qtest, INTC_PENDING1, 0, "TX channel mask gates IRQ", TX_IRQ)
    writel(qtest, REGS + 0x88, 1)
    expect(qtest, INTC_PENDING1, TX_IRQ, "TX unmask reasserts IRQ", TX_IRQ)
    # Advance to a real newer completion, then acknowledge the actual older
    # descriptor. The older pointer must not retire the newer interrupt.
    second_frame = frame[:14] + bytes(reversed(frame[14:]))
    second_tx_desc, second_tx_buffer = TX_DESC + 16, TX_BUFFER + 0x100
    write_bytes(qtest, second_tx_buffer, second_frame)
    descriptor(
        qtest,
        second_tx_desc,
        second_tx_buffer,
        len(second_frame),
        SOP | EOP | OWNER | len(second_frame),
    )
    writel(qtest, REGS + 0x600, second_tx_desc)
    wait_register(qtest, REGS + 0x640, second_tx_desc, "newer TX completion pointer")
    length = struct.unpack("!I", receive_exact(network, 4))[0]
    if length != len(second_frame) or receive_exact(network, length) != second_frame:
        raise RuntimeError("second CPDMA TX frame differs from guest buffer")
    expect(
        qtest,
        second_tx_desc + 12,
        SOP | EOP | EOQ | len(second_frame),
        "newer TX ownership/EOQ completion",
    )
    writel(qtest, REGS + 0x640, TX_DESC)
    expect(qtest, REGS + 0x80, 1, "stale TX acknowledgement preserves pending")
    expect(
        qtest, INTC_PENDING1, TX_IRQ, "stale TX acknowledgement preserves IRQ", TX_IRQ
    )
    expect(qtest, REGS + 0x640, second_tx_desc, "older TX ack leaves newer CP")
    writel(qtest, REGS + 0x640, second_tx_desc)
    expect(qtest, REGS + 0x80, 0, "TX completion acknowledgement")
    expect(qtest, INTC_PENDING1, 0, "TX routed IRQ cleared", TX_IRQ)

    descriptor(qtest, RX_DESC, RX_BUFFER, 256, OWNER)
    write_bytes(qtest, RX_BUFFER, bytes([0xA5]) * 256)
    # Program source address for the model's unicast receive filter.
    writel(qtest, REGS + 0x1D4, int.from_bytes(MAC[:4], "little"))
    writel(qtest, REGS + 0x1D0, int.from_bytes(MAC[4:], "little"))
    writel(qtest, REGS + 0x100, 1 << 13)  # Enable receive/broadcast path.
    writel(qtest, REGS + 0x104, 1)  # Accept channel-zero unicast.
    writel(qtest, REGS + 0xA8, 1)
    writel(qtest, EMAC + 0x914, 1)
    rx_offset = 16
    writel(qtest, REGS + 0x110, rx_offset)
    writel(qtest, REGS + 0x14, 1)
    writel(qtest, REGS + 0x620, RX_DESC)
    network.sendall(struct.pack("!I", len(frame)) + frame)
    wait_register(qtest, REGS + 0x660, RX_DESC, "RX completion pointer")
    expected = (
        bytes([0xA5]) * rx_offset
        + frame
        + bytes([0xA5]) * (256 - rx_offset - len(frame))
    )
    if read_bytes(qtest, RX_BUFFER, 256) != expected:
        raise RuntimeError(
            "CPDMA RX bytes, offset prefix or untouched buffer tail differ"
        )
    expect(
        qtest,
        RX_DESC + 8,
        (rx_offset << 16) | len(frame),
        "RX returned buffer offset/length",
    )
    expect(
        qtest,
        RX_DESC + 12,
        SOP | EOP | EOQ | len(frame),
        "RX ownership/length/EOQ completion",
    )
    expect(qtest, REGS + 0x620, 0, "RX consumed descriptor")
    expect(qtest, REGS + 0xA0, 1, "RX raw completion")
    expect(qtest, REGS + 0xA4, 1, "RX masked completion")
    expect(qtest, INTC_PENDING1, RX_IRQ, "RX routed SoC IRQ41", RX_IRQ)
    writel(qtest, REGS + 0xAC, 1)
    expect(qtest, REGS + 0xA0, 1, "RX raw remains pending with channel masked")
    expect(qtest, REGS + 0xA4, 0, "RX masked status cleared")
    expect(qtest, INTC_PENDING1, 0, "RX channel mask gates IRQ", RX_IRQ)
    writel(qtest, REGS + 0xA8, 1)
    expect(qtest, INTC_PENDING1, RX_IRQ, "RX unmask reasserts IRQ", RX_IRQ)
    # An exactly sized physical buffer proves offset bytes consume capacity.
    # Incomplete packets are filtered, never published as clipped success.
    second_rx_desc, second_rx_buffer = RX_DESC + 16, RX_BUFFER + 0x200
    capacity, guard = len(second_frame) + rx_offset, 16
    written = len(second_frame)
    write_bytes(qtest, second_rx_buffer, bytes([0xA5]) * (capacity + guard))
    descriptor(qtest, second_rx_desc, second_rx_buffer, capacity, OWNER)
    writel(qtest, REGS + 0x620, second_rx_desc)
    network.sendall(struct.pack("!I", len(second_frame)) + second_frame)
    wait_register(qtest, REGS + 0x660, second_rx_desc, "newer RX completion pointer")
    expected = (
        bytes([0xA5]) * rx_offset + second_frame[:written] + bytes([0xA5]) * guard
    )
    if read_bytes(qtest, second_rx_buffer, capacity + guard) != expected:
        raise RuntimeError("tight RX capacity overwrote offset prefix or guard bytes")
    expect(
        qtest,
        second_rx_desc + 8,
        (rx_offset << 16) | written,
        "tight RX actual buffer offset/length",
    )
    expect(
        qtest,
        second_rx_desc + 12,
        SOP | EOP | EOQ | written,
        "tight RX bounded completion",
    )
    writel(qtest, REGS + 0x660, RX_DESC)
    expect(qtest, REGS + 0xA0, 1, "stale RX acknowledgement preserves pending")
    expect(
        qtest, INTC_PENDING1, RX_IRQ, "stale RX acknowledgement preserves IRQ", RX_IRQ
    )
    expect(qtest, REGS + 0x660, second_rx_desc, "older RX ack leaves newer CP")
    writel(qtest, REGS + 0x660, second_rx_desc)
    expect(qtest, REGS + 0xA0, 0, "RX completion acknowledgement")
    expect(qtest, INTC_PENDING1, 0, "RX routed IRQ cleared", RX_IRQ)
    expect(qtest, REGS + 0xB0, 0, "no CPDMA host error")
    writel(qtest, REGS + 0x1C, 1)
    for offset in (4, 0x14, 0x600, 0x620, 0x640, 0x660, 0x80, 0xA0):
        expect(qtest, REGS + offset, 0, "CPDMA softreset")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} QEMU_SYSTEM_ARM")
    qemu = Path(sys.argv[1]).resolve(strict=True)
    with emac_process(qemu) as (qtest, network):
        probe_mdio(qtest)
        probe_cpdma(qtest, network)
    print(
        "synthetic MDIO read/write and channel-zero CPDMA TX/RX/IRQ/completion probes passed."
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
