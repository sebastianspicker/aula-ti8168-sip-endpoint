#!/usr/bin/env python3
"""Public-contract channel-zero CPDMA packet chains and bounded failure probes.

SPRUGX8C sections 6.2.5.1-.5 and 6.2.16.1.2-.5 define fragment lengths,
SOP/EOP ownership, EOQ and completion pointers. The 64-descriptor/2048-byte
limits are emulator bounds. RX shortage filters the whole packet; overrun
statistics and RXCEFEN partial-error delivery are not modeled. No FCS, teardown,
other channel, real PHY, interrupt-handler or network acceptance claim is made.
The optional legacy alias probe checks compatibility policy, not a hardware map.
"""

from __future__ import annotations

import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

from probe_emac import (
    EMAC,
    EOP,
    EOQ,
    INTC_PENDING1,
    MAC,
    OWNER,
    REGS,
    RX_IRQ,
    SOP,
    TX_IRQ,
    emac_process,
    expect,
    read_bytes,
    receive_exact,
    wait_register,
    write_bytes,
    writel,
)

DESC = 0x80030000
BUFFER = 0x80020000
FRAME = MAC + bytes.fromhex("0200000000010800") + bytes(range(50))
HOST_ERROR = 1 << 1


def desc(
    qtest: socket.socket, address: int, next_: int, buffer: int, buflen: int, mode: int
) -> None:
    write_bytes(qtest, address, struct.pack("<IIII", next_, buffer, buflen, mode))


def reset(qtest: socket.socket) -> None:
    writel(qtest, REGS + 0x1C, 1)
    writel(qtest, REGS + 0x88, 1)
    writel(qtest, REGS + 0xA8, 1)
    writel(qtest, EMAC + 0x918, 1)
    writel(qtest, EMAC + 0x914, 1)
    writel(qtest, REGS + 0x100, 1 << 21)  # Promiscuous fixture path.
    writel(qtest, REGS + 4, 1)
    writel(qtest, REGS + 0x14, 1)


def no_frame(network: socket.socket) -> None:
    original = network.gettimeout()
    network.settimeout(0.05)
    try:
        try:
            value = network.recv(4)
        except TimeoutError:
            return
        raise RuntimeError(f"malformed TX emitted unexpected backend bytes: {value!r}")
    finally:
        network.settimeout(original)


def received(network: socket.socket, expected: bytes) -> None:
    length = struct.unpack("!I", receive_exact(network, 4))[0]
    if length != len(expected) or receive_exact(network, length) != expected:
        raise RuntimeError(
            "packet was fragmented, clipped or contained incorrect bytes"
        )


def probe_tx(qtest: socket.socket, network: socket.socket) -> None:
    reset(qtest)
    sizes = (17, 23, 24)
    cursor = 0
    for index, length in enumerate(sizes):
        offset = 8 if index == 0 else 0
        data = FRAME[cursor : cursor + length]
        write_bytes(qtest, BUFFER + index * 256, b"X" * offset + data + b"G" * 16)
        mode = SOP | OWNER | len(FRAME) if index == 0 else EOP if index == 2 else 0
        # The continuation offset is ignored by the public TX contract.
        buflen = ((offset if index == 0 else 7) << 16) | length
        desc(
            qtest,
            DESC + index * 16,
            DESC + (index + 1) * 16 if index < 2 else 0,
            BUFFER + index * 256,
            buflen,
            mode,
        )
        cursor += length
    writel(qtest, REGS + 0x600, DESC)
    wait_register(qtest, REGS + 0x640, DESC + 32, "TX packet completes at EOP")
    received(network, FRAME)
    expect(qtest, DESC + 12, SOP | len(FRAME), "TX SOP released after full packet")
    expect(qtest, DESC + 28, 0, "TX middle flags unchanged")
    expect(qtest, DESC + 44, EOP | EOQ, "TX EOQ only on EOP")
    expect(qtest, REGS + 0x600, 0, "TX chain exhausted")
    expect(qtest, INTC_PENDING1, TX_IRQ, "TX packet IRQ", TX_IRQ)
    writel(qtest, REGS + 0x640, DESC)
    expect(qtest, INTC_PENDING1, TX_IRQ, "SOP is not EOP completion ack", TX_IRQ)
    writel(qtest, REGS + 0x640, DESC + 32)
    expect(qtest, INTC_PENDING1, 0, "EOP acknowledgement clears IRQ", TX_IRQ)
    expect(qtest, REGS + 0xB0, 0, "valid TX chain has no host error")
    no_frame(network)  # Three fragments must emit exactly one packet.

    # Two whole packets linked in one queue: EOQ only at the final queue tail.
    reset(qtest)
    for index in (0, 1):
        write_bytes(qtest, BUFFER + index * 256, FRAME)
        desc(
            qtest,
            DESC + index * 16,
            DESC + 16 if index == 0 else 0,
            BUFFER + index * 256,
            len(FRAME),
            SOP | EOP | OWNER | len(FRAME),
        )
    writel(qtest, REGS + 0x600, DESC)
    wait_register(qtest, REGS + 0x640, DESC + 16, "linked TX packet queue")
    received(network, FRAME)
    received(network, FRAME)
    expect(qtest, DESC + 12, SOP | EOP | len(FRAME), "non-tail TX has no EOQ")
    expect(qtest, DESC + 28, SOP | EOP | EOQ | len(FRAME), "tail TX has EOQ")


def probe_rx(qtest: socket.socket, network: socket.socket) -> None:
    reset(qtest)
    capacities, offset = (16, 23, 64), 5
    writel(qtest, REGS + 0x110, offset)
    for index, capacity in enumerate(capacities):
        write_bytes(qtest, BUFFER + index * 256, b"G" * (capacity + 16))
        desc(
            qtest,
            DESC + index * 16,
            DESC + (index + 1) * 16 if index < 2 else 0,
            BUFFER + index * 256,
            capacity,
            OWNER,
        )
    writel(qtest, REGS + 0x620, DESC)
    network.sendall(struct.pack("!I", len(FRAME)) + FRAME)
    wait_register(qtest, REGS + 0x660, DESC + 32, "RX packet completes at EOP")
    cursor = 0
    for index, (capacity, written) in enumerate(
        zip(capacities, (11, 23, 30), strict=True)
    ):
        skip = offset if index == 0 else 0
        expected = (
            b"G" * skip
            + FRAME[cursor : cursor + written]
            + b"G" * (capacity + 16 - skip - written)
        )
        if read_bytes(qtest, BUFFER + index * 256, capacity + 16) != expected:
            raise RuntimeError("RX chain bytes, offset prefix or guard mismatch")
        expect(
            qtest,
            DESC + index * 16 + 8,
            (skip << 16) | written,
            "RX actual fragment length/offset",
        )
        cursor += written
    expect(qtest, DESC + 12, SOP | len(FRAME), "RX SOP released and full packet length")
    expect(qtest, DESC + 28, OWNER, "RX middle has no SOP/EOP/length")
    expect(
        qtest, DESC + 44, OWNER | EOP | EOQ, "RX EOP/EOQ and packet-scoped ownership"
    )
    expect(qtest, INTC_PENDING1, RX_IRQ, "RX packet IRQ", RX_IRQ)
    writel(qtest, REGS + 0x660, DESC)
    expect(qtest, INTC_PENDING1, RX_IRQ, "RX SOP ack leaves EOP pending", RX_IRQ)
    writel(qtest, REGS + 0x660, DESC + 32)
    expect(qtest, INTC_PENDING1, 0, "RX EOP ack clears IRQ", RX_IRQ)
    expect(qtest, REGS + 0xB0, 0, "valid RX chain has no host error")


def expect_tx_error(qtest: socket.socket, network: socket.socket, label: str) -> None:
    writel(qtest, REGS + 0x600, DESC)
    wait_register(qtest, REGS + 0xB0, HOST_ERROR, label, HOST_ERROR)
    expect(qtest, REGS + 0x640, 0, "bad TX has no completion")
    expect(qtest, REGS + 0x80, 0, "bad TX has no packet IRQ")
    no_frame(network)


def probe_descriptor_limit(qtest: socket.socket, network: socket.socket) -> None:
    reset(qtest)
    for index, byte in enumerate(FRAME):
        write_bytes(qtest, BUFFER + index * 4, bytes((byte,)))
        mode = SOP | OWNER | len(FRAME) if index == 0 else EOP if index == 63 else 0
        desc(
            qtest,
            DESC + index * 16,
            DESC + (index + 1) * 16 if index < 63 else 0,
            BUFFER + index * 4,
            1,
            mode,
        )
    writel(qtest, REGS + 0x600, DESC)
    wait_register(qtest, REGS + 0x640, DESC + 63 * 16, "64-descriptor TX boundary")
    received(network, FRAME)
    expect(qtest, REGS + 0xB0, 0, "maximum valid TX descriptor count")
    reset(qtest)
    for index in range(64):
        write_bytes(qtest, BUFFER + index * 4, b"GGGG")
        desc(
            qtest,
            DESC + index * 16,
            DESC + (index + 1) * 16 if index < 63 else 0,
            BUFFER + index * 4,
            1,
            OWNER,
        )
    writel(qtest, REGS + 0x620, DESC)
    network.sendall(struct.pack("!I", len(FRAME)) + FRAME)
    wait_register(qtest, REGS + 0x660, DESC + 63 * 16, "64-descriptor RX boundary")
    for index, byte in enumerate(FRAME):
        if read_bytes(qtest, BUFFER + index * 4, 4) != bytes((byte,)) + b"GGG":
            raise RuntimeError("maximum-length RX chain lost data or overwrote guards")
    expect(qtest, REGS + 0xB0, 0, "maximum valid RX descriptor count")


def probe_invalid(qtest: socket.socket, network: socket.socket) -> None:
    for label, mode, length, next_ in (
        ("TX missing SOP", OWNER | EOP | 64, 64, 0),
        ("TX missing OWNER", SOP | EOP | 64, 64, 0),
        ("TX early EOP length mismatch", SOP | OWNER | EOP | 64, 32, 0),
        ("TX missing EOP", SOP | OWNER | 64, 64, 0),
        ("TX zero length", SOP | OWNER | EOP | 64, 0, 0),
        (
            "TX offset equals fragment length",
            SOP | OWNER | EOP | 64,
            (64 << 16) | 64,
            0,
        ),
        (
            "TX offset exceeds fragment length",
            SOP | OWNER | EOP | 64,
            (65 << 16) | 64,
            0,
        ),
        ("TX cycle", SOP | OWNER | 64, 16, DESC),
        ("TX unaligned next", SOP | OWNER | 64, 16, DESC + 1),
    ):
        reset(qtest)
        write_bytes(qtest, BUFFER, FRAME)
        desc(qtest, DESC, next_, BUFFER, length, mode)
        expect_tx_error(qtest, network, label)
        expect(qtest, DESC + 12, mode, "malformed TX does not release SOP")
    reset(qtest)
    # 65 one-byte fragments exceed the model's explicit per-packet work bound.
    write_bytes(qtest, BUFFER, b"x")
    for index in range(65):
        mode = SOP | OWNER | 65 if index == 0 else EOP if index == 64 else 0
        desc(
            qtest,
            DESC + index * 16,
            DESC + (index + 1) * 16 if index < 64 else 0,
            BUFFER,
            1,
            mode,
        )
    expect_tx_error(qtest, network, "TX descriptor work bound")
    expect(qtest, DESC + 12, SOP | OWNER | 65, "overlong TX retains SOP ownership")

    reset(qtest)
    desc(qtest, DESC, 0, 0xFFFFFFF0, (32 << 16) | 64, SOP | EOP | OWNER | 64)
    expect_tx_error(qtest, network, "TX DMA address overflow")
    reset(qtest)
    desc(qtest, DESC, 0, 0, 64, SOP | EOP | OWNER | 64)
    expect_tx_error(qtest, network, "TX zero buffer")
    for label, next_, owner in (("RX cycle", DESC, OWNER), ("RX owner", 0, 0)):
        reset(qtest)
        write_bytes(qtest, BUFFER, b"G" * 96)
        desc(qtest, DESC, next_, BUFFER, 16, owner)
        writel(qtest, REGS + 0x620, DESC)
        network.sendall(struct.pack("!I", len(FRAME)) + FRAME)
        wait_register(qtest, REGS + 0xB0, HOST_ERROR, label, HOST_ERROR)
        expect(qtest, REGS + 0x660, 0, "bad RX has no completion")
        if read_bytes(qtest, BUFFER, 96) != b"G" * 96:
            raise RuntimeError("invalid RX wrote a partial packet")

    reset(qtest)
    write_bytes(qtest, BUFFER, b"G" * 96)
    for index in range(65):
        desc(
            qtest,
            DESC + index * 16,
            DESC + (index + 1) * 16 if index < 64 else 0,
            BUFFER,
            1,
            OWNER,
        )
    writel(qtest, REGS + 0x620, DESC)
    large_frame = FRAME + b"X"
    network.sendall(struct.pack("!I", len(large_frame)) + large_frame)
    wait_register(
        qtest, REGS + 0xB0, HOST_ERROR, "RX descriptor work bound", HOST_ERROR
    )
    expect(qtest, REGS + 0x660, 0, "overlong RX has no completion")
    if read_bytes(qtest, BUFFER, 96) != b"G" * 96:
        raise RuntimeError("overlong RX chain wrote partial bytes")
    reset(qtest)
    # Normal resource exhaustion is filtered, not turned into clipped success.
    write_bytes(qtest, BUFFER, b"G" * 96)
    desc(qtest, DESC, 0, BUFFER, 32, OWNER)
    writel(qtest, REGS + 0x620, DESC)
    network.sendall(struct.pack("!I", len(FRAME)) + FRAME)
    time.sleep(0.05)
    expect(qtest, REGS + 0x660, 0, "short RX queue must not complete")
    expect(qtest, REGS + 0xA0, 0, "short RX queue must not signal packet success")
    expect(qtest, REGS + 0xB0, 0, "RX shortage is not a malformed host descriptor")
    expect(qtest, DESC + 12, OWNER, "short RX queue keeps ownership")
    if read_bytes(qtest, BUFFER, 96) != b"G" * 96:
        raise RuntimeError("short RX queue published clipped packet bytes")


def probe_alias(qemu: Path) -> None:
    # Existing compatibility range maps 0x41020000 to physical DDR0x80020000.
    # It is deliberately not asserted to be part of the TI hardware contract.
    for enabled in (False, True):
        with emac_process(qemu, legacy_dma_alias=enabled) as (qtest, network):
            reset(qtest)
            write_bytes(qtest, BUFFER, FRAME)
            desc(qtest, DESC, 0, 0x41020000, len(FRAME), SOP | EOP | OWNER | len(FRAME))
            if enabled:
                writel(qtest, REGS + 0x600, DESC)
                wait_register(qtest, REGS + 0x640, DESC, "explicit legacy DMA alias")
                received(network, FRAME)
                expect(qtest, REGS + 0xB0, 0, "opt-in alias has no host error")
            else:
                expect_tx_error(qtest, network, "legacy DMA alias disabled by default")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {Path(sys.argv[0]).name} QEMU_SYSTEM_ARM")
    qemu = Path(sys.argv[1]).resolve(strict=True)
    with emac_process(qemu) as (qtest, network):
        probe_tx(qtest, network)
        probe_rx(qtest, network)
        probe_descriptor_limit(qtest, network)
        probe_invalid(qtest, network)
    probe_alias(qemu)
    print(
        "CPDMA packet chains, bounded malformed queues, nonclipping RX and opt-in legacy DMA alias passed."
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc
