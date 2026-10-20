#!/usr/bin/env python3
"""Boundedly boot a pinned original kernel with newly generated rescue userspace."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import secrets
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

from private_output import prepare_output
from rescue_initramfs import (
    build,
    checked,
    load_inputs,
    output_allowed,
    regular,
    relative,
    sha256,
    strict_json,
    verification_script,
)

MAX_LOG = 16 * 1024 * 1024
BANK_BYTES = 256 * 1024 * 1024


def stop(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


def safe_option(value: str) -> str:
    key, separator, data = value.partition("=")
    allowed = {"boot-machine-id", "boot-ram-start", "boot-ram-size", "early-uart-base", "nand-profile"}
    if (not separator or key not in allowed or not data or
            any(character in data for character in ",\r\n\0")):
        raise ValueError("machine options must select explicit boot metadata, UART alias, or NAND profile")
    if key == "nand-profile":
        if data not in ("none", "synthetic-read-only"):
            raise ValueError("unsupported NAND profile")
    elif not re.fullmatch(r"(?:0x[0-9a-fA-F]+|[0-9]+)", data):
        raise ValueError("boot metadata and UART alias must be numeric")
    return value


def file_receipt(path: Path, expected: object, size: int | None = None) -> dict[str, Any]:
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode) or (size is not None and before.st_size != size):
            raise ValueError(f"wrong file type or pinned size: {path}")
        digest = hashlib.sha256()
        with os.fdopen(fd, "rb", closefd=False) as source:
            while chunk := source.read(1024 * 1024):
                digest.update(chunk)
        after = os.fstat(fd)
        if (digest.hexdigest() != expected or
                (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns) !=
                (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)):
            raise ValueError(f"file hash mismatch or changed input: {path}")
        return {"path": str(path), "sha256": digest.hexdigest(), "bytes": after.st_size}
    finally:
        os.close(fd)


def erased_sha256(size: int) -> str:
    digest = hashlib.sha256()
    chunk = b"\xff" * min(size, 1024 * 1024)
    while size:
        count = min(size, len(chunk))
        digest.update(chunk[:count])
        size -= count
    return digest.hexdigest()


def expected_banks(segments: list[dict[str, Any]]) -> list[str]:
    """Recompute exact bank contents from pinned sources plus erased gaps."""
    for item in segments:
        if (type(item.get("offset")) is not int or type(item.get("size")) is not int or
                item["offset"] < 0 or item["size"] <= 0 or item["offset"] + item["size"] > 2 * BANK_BYTES):
            raise ValueError("composition segment is outside the two-bank address space")
    ordered = sorted(segments, key=lambda item: item["offset"])
    digests = [hashlib.sha256(), hashlib.sha256()]
    position = 0

    def feed(data: bytes) -> None:
        nonlocal position
        start = 0
        while start < len(data):
            bank, offset = divmod(position, BANK_BYTES)
            count = min(len(data) - start, BANK_BYTES - offset)
            digests[bank].update(data[start:start + count])
            start += count
            position += count

    def fill(size: int) -> None:
        chunk = b"\xff" * min(size, 1024 * 1024)
        while size:
            count = min(size, len(chunk))
            feed(chunk[:count])
            size -= count

    for item in ordered:
        if item["offset"] < position:
            raise ValueError("composition segments overlap")
        fill(item["offset"] - position)
        fd = os.open(item["path"], os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(fd, "rb") as source:
            info = os.fstat(source.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size != item["size"]:
                raise ValueError("composition source size/type changed")
            digest = hashlib.sha256()
            remaining = item["size"]
            while remaining:
                chunk = source.read(min(remaining, 1024 * 1024))
                if not chunk:
                    raise ValueError("composition source was truncated")
                digest.update(chunk)
                feed(chunk)
                remaining -= len(chunk)
            if source.read(1) or digest.hexdigest() != item["sha256"]:
                raise ValueError("composition source hash changed")
    fill(2 * BANK_BYTES - position)
    return [digest.hexdigest() for digest in digests]


def composition(path: Path, kernel: dict[str, Any], rootfs: dict[str, Any] | None = None) -> tuple[dict[str, Any], list[str]]:
    raw = regular(path)
    if len(raw) > 1024 * 1024:
        raise ValueError("composition manifest exceeds 1 MiB")
    document = strict_json(raw)
    if (not isinstance(document, dict) or document.get("kind") != "synthetic-ti8168-nand-composition-v1"
            or type(document.get("schema_version")) is not int or document.get("schema_version") != 1 or
            type(document.get("bank_count")) is not int or document.get("bank_count") != 2
            or document.get("bank_bytes") != BANK_BYTES):
        raise ValueError("unsupported maintained-machine NAND composition")
    files, segments = document.get("files"), document.get("segments")
    if not isinstance(files, list) or len(files) != 4 or not isinstance(segments, list) or not 1 <= len(segments) <= 1024:
        raise ValueError("composition must pin four bank/OOB files and source segments")
    verified = []
    kernel_found = False
    rootfs_found = rootfs is None
    for item in segments:
        source = Path(item["path"])
        if not source.is_absolute() or type(item["size"]) is not int or item["size"] <= 0:
            raise ValueError("composition source paths and sizes must be explicit")
        verified.append(file_receipt(source, item["sha256"], item["size"]))
        kernel_found |= source.resolve() == Path(kernel["path"]).resolve() and item["sha256"] == kernel["sha256"]
        if rootfs is not None:
            rootfs_found |= source.resolve() == Path(rootfs["path"]).resolve() and item["sha256"] == rootfs["sha256"]
    if not kernel_found or not rootfs_found:
        raise ValueError("composition does not contain the explicitly pinned kernel/rootfs sources")
    expected = expected_banks(segments)
    options = []
    seen = set()
    for item in files:
        name, bank, kind = relative(item["path"]), item["bank"], item["kind"]
        if "/" in name or type(bank) is not int or bank not in (0, 1) or kind not in ("main", "synthetic-erased-oob"):
            raise ValueError("composition file identity is invalid")
        identity = (bank, kind)
        if identity in seen:
            raise ValueError("duplicate composition bank identity")
        seen.add(identity)
        size = BANK_BYTES if kind == "main" else BANK_BYTES // 2048 * 64
        if item["size"] != size:
            raise ValueError("composition bank size does not match the maintained machine")
        backing = path.resolve().parent / name
        receipt = file_receipt(backing, item["sha256"], size)
        if receipt["sha256"] != (expected[bank] if kind == "main" else erased_sha256(size)):
            raise ValueError("backing bytes do not match pinned segment composition or erased OOB")
        verified.append(receipt)
        if any(character in str(backing) for character in ",\r\n\0"):
            raise ValueError("backing path cannot be represented safely as a machine property")
        suffix = "image" if kind == "main" else "oob-image"
        options.append(f"nand-mtd{bank}-{suffix}={backing}")
    return {"manifest": str(path.resolve()), "sha256": sha256(raw), "verified": verified}, options


def evidence(log: bytes, busybox_digest: str, nonce: str = "", rootfs: bool = False, check_count: int = 0) -> dict[str, str] | None:
    prefix = "AULA:RESCUE:" + (nonce + ":" if nonce else "")
    lines = log.decode("utf-8", "replace").replace("\r", "").splitlines()
    if any(line == prefix + "FAIL" or "Kernel panic" in line for line in lines):
        raise RuntimeError("kernel or generated init reported failure")
    try:
        start = lines.index(prefix + "INIT")
        uname = next(index for index in range(start + 1, len(lines))
                     if lines[index].startswith(prefix + "UNAME:") and lines[index] != prefix + "UNAME:")
        busybox = lines.index(prefix + "BUSYBOX:" + busybox_digest, uname + 1)
        ready_after = busybox + 1
        if rootfs:
            ready_after = lines.index(prefix + "ROOT:ubifs:ro", ready_after) + 1
        for index in range(check_count):
            ready_after = lines.index(prefix + f"CHECK:{index}:EXIT:0", ready_after) + 1
        lines.index(prefix + "READY", ready_after)
    except (ValueError, StopIteration):
        return None
    return {"uname_release": lines[uname].removeprefix(prefix + "UNAME:"),
            "busybox_sha256": busybox_digest}


def run(args: argparse.Namespace) -> dict[str, Any]:
    qemu = args.qemu.resolve(strict=True)
    if not qemu.is_file() or not os.access(qemu, os.X_OK):
        raise ValueError("QEMU must be an executable regular file")
    inputs = load_inputs(args.inputs)
    output_allowed(args.output_dir, Path(inputs["runtime_root"]), Path(inputs["kernel"]["path"]))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="original-rescue-", dir=args.output_dir.resolve()))
    record: dict[str, Any] = {"kind": "aula-original-rescue-run-v1", "result": "failed", "directory": str(output)}
    try:
        if args.rootfs_boot and (inputs.get("supplemental") or inputs.get("checks")):
            raise ValueError("rootfs mode cannot load supplemental bytes/checks; use rescue initramfs mode")
        generated = build(args.inputs, output / "initramfs")
        record["build"] = generated
        kernel = Path(generated["kernel"]["path"])
        options = [safe_option(option) for option in args.machine_option]
        if len({option.partition("=")[0] for option in options}) != len(options):
            raise ValueError("duplicate machine options are not permitted")
        machine = ["ti8168-mediaboard", *options]
        if args.rootfs_boot and (args.nand_composition is None or "rootfs" not in inputs):
            raise ValueError("--rootfs-boot requires --nand-composition and explicit input-manifest rootfs pin")
        rootfs_pin = inputs.get("rootfs")
        if args.nand_composition is not None:
            receipt, bank_options = composition(args.nand_composition, generated["kernel"], rootfs_pin)
            record["nand_composition"] = receipt
            if "nand-profile=none" in options:
                raise ValueError("composition conflicts with disabled NAND profile")
            if "nand-profile=synthetic-read-only" not in options:
                machine.append("nand-profile=synthetic-read-only")
            machine.extend(["nand-cow=" + ("on" if args.nand_volatile_writes else "off"), *bank_options])
            record["nand_volatile_writes"] = args.nand_volatile_writes
        if args.seed is not None:
            seed = checked(regular(args.seed), args.seed_sha256, "machine seed")
            seed_copy = output / "machine-seed.bin"
            seed_copy.write_bytes(seed)
            seed_copy.chmod(0o400)
            machine.append("control-init-file=" + str(seed_copy))
            record["machine_seed"] = {"source": str(args.seed.resolve()), "sha256": sha256(seed), "copy": str(seed_copy)}
        command_line = args.append.strip()
        if any(word.startswith(("init=", "rdinit=")) for word in command_line.split()):
            raise ValueError("--append cannot override the generated rescue init")
        if args.rootfs_boot:
            if "ro" not in command_line.split() or any(word == "rw" for word in command_line.split()):
                raise ValueError("original rootfs mode requires an explicit read-only root command line")
            command_line += " init=/bin/sh"
        else:
            command_line += " rdinit=/init"
        command = [str(qemu), "-machine", ",".join(machine), "-accel", "tcg,thread=single",
                   "-nodefaults", "-nic", "none", "-display", "none", "-monitor", "none",
                   "-serial", "stdio", "-semihosting-config", "enable=off", "-no-reboot",
                   "-kernel", str(kernel), "-append", command_line]
        script = b""
        nonce = secrets.token_hex(16) if args.rootfs_boot else ""
        record["nonce"] = nonce
        if args.rootfs_boot:
            script = verification_script(generated["busybox_path"], generated["busybox_sha256"], generated["checks"],
                                         nonce=nonce, require_rootfs=True)
            if len(script) > 4096:
                raise ValueError("interactive verification script exceeds the 4096-byte pipe limit")
            (output / "guest-check.sh").write_bytes(script)
            record["guest_check_sha256"] = sha256(script)
            record["initramfs_loaded"] = False
        else:
            command.extend(("-initrd", generated["archive"]["path"]))
            record["initramfs_loaded"] = True
        if args.memory is not None:
            command.extend(("-m", args.memory))
        if args.nand_id is not None:
            command.extend(("-global", "ti8168-synthetic-nand.id=" + hex(args.nand_id)))
        record["argv"] = command
        version = subprocess.run([str(qemu), "--version"], check=True, capture_output=True, text=True, timeout=10)
        record["qemu"] = {"path": str(qemu), "sha256": sha256(regular(qemu)), "version": version.stdout.strip()}
        record["timeout_seconds"] = args.timeout
        log = bytearray()
        found: dict[str, str] | None = None
        with (output / "uart.log").open("wb") as uart, (output / "qemu.stderr.log").open("wb") as stderr:
            process = subprocess.Popen(command, stdin=subprocess.PIPE if args.rootfs_boot else subprocess.DEVNULL,
                                       stdout=uart, stderr=stderr)
            submitted = False
            try:
                deadline = time.monotonic() + args.timeout
                with (output / "uart.log").open("rb") as reader:
                    while time.monotonic() < deadline:
                        log.extend(reader.read(65536))
                        if len(log) > MAX_LOG or (output / "qemu.stderr.log").stat().st_size > MAX_LOG:
                            raise RuntimeError("QEMU output exceeded the 16 MiB per-stream limit")
                        if args.rootfs_boot and not submitted and re.search(rb"(?:^|[\r\n])/ # ?$", bytes(log)):
                            if process.stdin is None:
                                raise RuntimeError("rootfs shell input pipe is unavailable")
                            process.stdin.write(script)
                            process.stdin.flush()
                            submitted = True
                            record["guest_check_submitted_after_prompt"] = True
                        found = evidence(bytes(log), generated["busybox_sha256"], nonce, args.rootfs_boot, len(generated["checks"]))
                        if process.poll() is not None:
                            log.extend(reader.read(MAX_LOG + 1 - len(log)))
                            if len(log) > MAX_LOG:
                                raise RuntimeError("QEMU UART exceeded 16 MiB")
                            found = evidence(bytes(log), generated["busybox_sha256"], nonce, args.rootfs_boot, len(generated["checks"]))
                            if process.returncode != 0:
                                raise RuntimeError(f"QEMU exited with status {process.returncode}")
                            break
                        if found is not None:
                            break
                        time.sleep(0.05)
            finally:
                record["stopped_by_host"] = process.poll() is None
                stop(process)
                record["returncode"] = process.returncode
                if process.stdin is not None:
                    process.stdin.close()
        checked(regular(kernel), generated["kernel"]["sha256"], "kernel after execution")
        if args.nand_composition is not None:
            after, _ = composition(args.nand_composition, generated["kernel"], rootfs_pin)
            if after != record["nand_composition"]:
                raise RuntimeError("NAND composition changed during execution")
            record["nand_unchanged_after_run"] = True
        if found is None:
            raise RuntimeError("bounded boot ended without verified rescue userspace markers")
        if args.rootfs_boot:
            found["root_filesystem"] = "ubifs"
            found["root_mount"] = "ro"
        record["userspace"] = found
        record["verified_checks"] = [{"index": index, "argv": command, "exit_status": 0}
                                     for index, command in enumerate(generated["checks"])]
        record["result"] = "userspace-reached"
        record["claim"] = ("original kernel mounted pinned-composition UBIFS read-only and ran hash-verified BusyBox shell; vendor startup bypassed"
                           if args.rootfs_boot else
                           "original kernel ran new rescue init and hash-verified selected BusyBox; vendor startup bypassed")
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        record["error"] = str(error)
        raise RuntimeError(f"original-kernel probe failed; evidence: {output}: {error}") from error
    finally:
        for filename in ("uart.log", "qemu.stderr.log"):
            path = output / filename
            if path.is_file():
                digest = hashlib.sha256()
                with path.open("rb") as stream:
                    while chunk := stream.read(1024 * 1024):
                        digest.update(chunk)
                record[filename + "_sha256"] = digest.hexdigest()
        (output / "run.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", type=Path, required=True)
    parser.add_argument("--inputs", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--append", required=True, help="explicit caller-selected kernel command line")
    parser.add_argument("--machine-option", action="append", default=[])
    parser.add_argument("--memory", help="explicit QEMU RAM size")
    parser.add_argument("--rootfs-boot", action="store_true")
    parser.add_argument("--nand-composition", type=Path)
    parser.add_argument("--nand-volatile-writes", action="store_true",
                        help="allow only in-memory NAND writes; host backing hashes must remain unchanged")
    parser.add_argument("--nand-id", type=lambda value: int(value, 0),
                        help="explicit synthetic NAND identity word, e.g. a hexadecimal integer")
    parser.add_argument("--seed", type=Path)
    parser.add_argument("--seed-sha256")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    if args.nand_volatile_writes and args.nand_composition is None:
        parser.error("--nand-volatile-writes requires a pinned composition")
    if args.memory is not None and not re.fullmatch(r"[1-9][0-9]*[MG]", args.memory):
        parser.error("--memory must be an explicit positive size with M or G suffix")
    if not 0 < args.timeout <= 120:
        parser.error("--timeout must be greater than zero and at most 120 seconds")
    if args.nand_id is not None and not 0 <= args.nand_id <= 0xffffffff:
        parser.error("--nand-id must fit an unsigned 32-bit word")
    if (args.seed is None) != (args.seed_sha256 is None):
        parser.error("--seed and --seed-sha256 must be supplied together")
    args.output_dir = prepare_output(args.output_dir, container=True)
    result = run(args)
    print(f"verified userspace reached; evidence: {result['directory']}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1) from error
