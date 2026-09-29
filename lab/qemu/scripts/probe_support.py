"""QEMU qtest/QMP transport and process lifecycle helpers."""

from __future__ import annotations

from contextlib import contextmanager
import json
from pathlib import Path
import socket
import subprocess
import tempfile
from typing import Iterator, TextIO


def recv_line(stream: socket.socket) -> str:
    data = bytearray()
    while not data.endswith(b"\n"):
        available = stream.recv(min(4096, 65537 - len(data)), socket.MSG_PEEK)
        newline = available.find(b"\n")
        chunk = stream.recv(newline + 1 if newline >= 0 else len(available))
        if not chunk:
            raise RuntimeError("QEMU closed a control socket unexpectedly")
        data.extend(chunk)
        if len(data) > 65536:
            raise RuntimeError("QEMU control response exceeded 64 KiB")
    return data.decode("utf-8", "strict").strip()


def qmp_response(stream: socket.socket) -> dict[str, object]:
    for _ in range(64):
        response = json.loads(recv_line(stream))
        if not isinstance(response, dict):
            raise RuntimeError("QEMU QMP response is not an object")
        if "event" not in response:
            return response
    raise RuntimeError("QEMU QMP response exceeded 64 asynchronous events")


def send_json(stream: socket.socket, value: dict[str, object]) -> None:
    stream.sendall(json.dumps(value, separators=(",", ":")).encode() + b"\n")


def qtest_command(
    stream: socket.socket, command: str, expected: str = "OK"
) -> None:
    stream.sendall(command.encode() + b"\n")
    try:
        response = recv_line(stream)
    except OSError as exc:
        raise RuntimeError(f"qtest command {command!r}: {exc}") from exc
    if response != expected:
        raise RuntimeError(
            f"qtest command {command!r}: expected {expected!r}, got {response!r}"
        )


def qtest_step(stream: socket.socket, nanoseconds: int) -> None:
    command = f"clock_step {nanoseconds}"
    stream.sendall(command.encode() + b"\n")
    response = recv_line(stream)
    if not response.startswith("OK"):
        raise RuntimeError(f"qtest command {command!r}: got {response!r}")


def qmp_hmp(qmp: socket.socket, command: str) -> str:
    send_json(
        qmp,
        {
            "execute": "human-monitor-command",
            "arguments": {"command-line": command},
        },
    )
    response = qmp_response(qmp)
    if not isinstance(response.get("return"), str):
        raise RuntimeError(f"HMP {command!r} did not return text: {response!r}")
    return response["return"]


def listener(path: Path) -> socket.socket:
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(str(path))
    server.listen(1)
    server.settimeout(5)
    return server


def terminate_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)


def qtest_arguments(
    qemu: Path, machine: str, qtest_path: Path, qmp_path: Path | None
) -> list[str]:
    arguments = [
        str(qemu), "-machine", machine, "-accel", "qtest", "-display", "none",
        "-serial", "none", "-qtest", f"unix:{qtest_path}",
    ]
    if qmp_path is not None:
        arguments.extend(
            (
                "-chardev", f"socket,path={qmp_path},id=qmp0",
                "-mon", "chardev=qmp0,mode=control",
            )
        )
    return arguments


def check_exit(
    process: subprocess.Popen[str], label: str, stderr_log: TextIO | None = None
) -> None:
    process.wait(timeout=5)
    if process.returncode != 0:
        stderr = ""
        if stderr_log is not None:
            stderr_log.seek(0)
            stderr = stderr_log.read()
        raise RuntimeError(
            f"{label} exited {process.returncode}: {stderr.strip()}"
        )


@contextmanager
def qtest_pair(
    qemu: Path, machine: str, prefix: str, exit_label: str
) -> Iterator[tuple[socket.socket, socket.socket]]:
    with tempfile.TemporaryDirectory(prefix=prefix) as temp_name:
        qtest_path = Path(temp_name) / "qtest.sock"
        qmp_path = Path(temp_name) / "qmp.sock"
        with listener(qtest_path) as qtest_server, listener(qmp_path) as qmp_server:
            with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as stderr_log:
                process = subprocess.Popen(
                    qtest_arguments(qemu, machine, qtest_path, qmp_path),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=stderr_log,
                    text=True,
                )
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
                    check_exit(process, exit_label, stderr_log)
                finally:
                    terminate_process(process)


@contextmanager
def qtest_only(
    qemu: Path, machine: str, prefix: str
) -> Iterator[socket.socket]:
    with tempfile.TemporaryDirectory(prefix=prefix) as temp_name:
        qtest_path = Path(temp_name) / "qtest.sock"
        with listener(qtest_path) as qtest_server:
            with tempfile.TemporaryFile(mode="w+", encoding="utf-8") as stderr_log:
                process = subprocess.Popen(
                    qtest_arguments(qemu, machine, qtest_path, None),
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=stderr_log,
                    text=True,
                )
                try:
                    with qtest_server.accept()[0] as qtest:
                        qtest.settimeout(5)
                        yield qtest
                finally:
                    terminate_process(process)
