#!/usr/bin/env python3
"""Prove actual ARM CPU, UART, and twice-acknowledged timer IRQ execution."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any

from probe_support import terminate_process

MARKERS = (
    "AULA:GUEST:CPU_UART",
    "AULA:GUEST:TIMER_IRQ_1_CLEARED_ACKED",
    "AULA:GUEST:TIMER_IRQ_2_CLEARED_ACKED",
    "AULA:GUEST:PASS",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def probe(qemu: Path, output: Path, assembler: str, linker: str, timeout: float) -> dict[str, Any]:
    qemu = qemu.resolve(strict=True)
    if not qemu.is_file() or not os.access(qemu, os.X_OK):
        raise ValueError(f"QEMU is not an executable regular file: {qemu}")
    output.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="guest-", dir=output.resolve(strict=True)))
    record: dict[str, Any] = {"kind": "aula-arm-guest-probe-v1", "result": "failed", "directory": str(run)}
    try:
        builder = Path(__file__).resolve().parents[1] / "guest" / "build_guest.py"
        build = subprocess.run([sys.executable, str(builder), "--output-dir", str(run),
                                "--assembler", assembler, "--linker", linker],
                               check=True, capture_output=True, text=True, timeout=45)
        (run / "build.stdout.log").write_text(build.stdout, encoding="utf-8")
        (run / "build.stderr.log").write_text(build.stderr, encoding="utf-8")
        record["build"] = json.loads((run / "build.json").read_text(encoding="utf-8"))
        record["qemu"] = {"path": str(qemu), "sha256": digest(qemu)}
        version = subprocess.run([str(qemu), "--version"], check=True, capture_output=True, text=True, timeout=10)
        record["qemu"]["version"] = version.stdout.strip()
        command = [str(qemu), "-machine", "ti8168-mediaboard", "-cpu", "cortex-a8", "-accel", "tcg,thread=single",
                   "-icount", "shift=0,align=off,sleep=off", "-display", "none", "-monitor", "none", "-nic", "none",
                   "-serial", "stdio", "-semihosting-config", "enable=on,target=native",
                   "-kernel", str(run / "guest.elf")]
        record["argv"] = command
        record["timeout_seconds"] = timeout
        with (run / "uart.log").open("w", encoding="utf-8") as uart, (run / "qemu.stderr.log").open("w", encoding="utf-8") as stderr:
            process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=uart, stderr=stderr, text=True)
            try:
                try:
                    process.wait(timeout=timeout)
                except subprocess.TimeoutExpired as error:
                    raise RuntimeError(f"guest did not exit through semihosting within {timeout:g} seconds") from error
            finally:
                terminate_process(process)
                record["returncode"] = process.returncode
        uart_output = (run / "uart.log").read_text(encoding="utf-8")
        record["uart_sha256"] = digest(run / "uart.log")
        record["stderr_sha256"] = digest(run / "qemu.stderr.log")
        if process.returncode != 0:
            raise RuntimeError(f"QEMU/guest exited with status {process.returncode}; inspect {run}")
        lines = tuple(uart_output.splitlines())
        if lines != MARKERS:
            raise RuntimeError(f"guest UART evidence differs from the four required ordered markers: {lines!r}")
        record["markers"] = list(lines)
        record["result"] = "passed"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        record["error"] = str(error)
        if isinstance(error, subprocess.CalledProcessError):
            record["command_stdout"] = error.stdout
            record["command_stderr"] = error.stderr
        raise RuntimeError(f"guest probe failed; evidence: {run}: {error}") from error
    finally:
        for filename, field in (("uart.log", "uart_sha256"), ("qemu.stderr.log", "stderr_sha256")):
            if (run / filename).is_file():
                record[field] = digest(run / filename)
        (run / "probe.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("qemu", type=Path)
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).resolve().parents[3] / ".work/build/qemu/guest-probes")
    parser.add_argument("--assembler", default="arm-none-eabi-as")
    parser.add_argument("--linker", default="arm-none-eabi-ld")
    parser.add_argument("--timeout", type=float, default=15)
    arguments = parser.parse_args()
    if not 0 < arguments.timeout <= 60:
        parser.error("--timeout must be greater than zero and at most 60 seconds")
    result = probe(arguments.qemu, arguments.output_dir, arguments.assembler, arguments.linker, arguments.timeout)
    print(f"actual ARM CPU/UART/timer IRQ probe passed; evidence: {result['directory']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
