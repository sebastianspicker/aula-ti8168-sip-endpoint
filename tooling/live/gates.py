#!/usr/bin/env python3
"""Run and bind the complete local gate set to one physical-lab payload."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

from source_provenance import EVIDENCE_GENERATOR, EVIDENCE_SOURCE_ROOTS, SOURCE_ROOTS
from source_provenance import SourceProvenanceError
from source_provenance import source_digest as maintained_source_digest


ROOT = Path(__file__).resolve().parents[2]
BUILD_RECEIPT = ROOT / ".work/build/live/receipt.json"
PAYLOAD = ROOT / ".work/dist/ls200-live/runtime"
GATE_RECEIPT = ROOT / ".work/dist/ls200-live/local-gates.json"
SAFE_ENV = {
    "LANG": "C",
    "LC_ALL": "C",
    "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
    "PYTHONDONTWRITEBYTECODE": "1",
}
REQUIRED_HOST_TOOLS = (
    "python3", "uv", "pnpm", "node", "cmake", "ctest", "cmake-format",
    "pkg-config", "shellcheck", "clang", "llvm-profdata", "llvm-cov",
)
class GateError(RuntimeError):
    pass


def is_sha256(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


def exact_json(raw: bytes) -> Any:
    def reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise GateError(f"duplicate local-gate field: {key}")
            result[key] = value
        return result

    return json.loads(raw, object_pairs_hook=reject_duplicates)


def safe_repo_directory(path: Path) -> None:
    """Reject symlinked or other-user writable paths below the repository root."""
    if not path.is_absolute():
        raise GateError("local-gate path is not absolute")
    try:
        parts = path.relative_to(ROOT).parts
    except ValueError as error:
        raise GateError("local-gate path escaped the repository") from error
    current = ROOT
    for part in parts:
        current /= part
        info = current.lstat()
        if (
            not stat.S_ISDIR(info.st_mode)
            or stat.S_ISLNK(info.st_mode)
            or info.st_uid != os.getuid()
            or stat.S_IMODE(info.st_mode) & 0o022
        ):
            raise GateError(f"unsafe local-gate directory: {current.relative_to(ROOT)}")


def read_private_json(path: Path) -> Any:
    safe_repo_directory(path.parent)
    flags = os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(path, flags)
    try:
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) != 0o600
        ):
            raise GateError(f"unsafe local-gate file: {path.relative_to(ROOT)}")
        with os.fdopen(descriptor, "rb") as handle:
            descriptor = -1
            raw = handle.read(1024 * 1024 + 1)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
    if len(raw) > 1024 * 1024:
        raise GateError(f"oversized local-gate file: {path.relative_to(ROOT)}")
    return exact_json(raw)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def _validate_host_tools(value: Any) -> None:
    if not isinstance(value, dict) or set(value) != set(REQUIRED_HOST_TOOLS):
        raise GateError("local-gate host-tool receipt is incomplete")
    for name in REQUIRED_HOST_TOOLS:
        spec = value[name]
        if not isinstance(spec, dict) or set(spec) != {"path", "sha256"}:
            raise GateError(f"local-gate host-tool receipt is malformed: {name}")
        path = Path(spec["path"])
        if (
            not path.is_absolute()
            or not path.is_file()
            or path.is_symlink()
            or not is_sha256(spec["sha256"])
            or digest(path) != spec["sha256"]
        ):
            raise GateError(f"local-gate host tool changed or is unsafe: {name}")
        info = path.stat()
        if info.st_uid not in {0, os.getuid()} or stat.S_IMODE(info.st_mode) & 0o022:
            raise GateError(f"local-gate host tool has unsafe ownership or mode: {name}")


def _host_tool_environment(search_path: str) -> tuple[dict[str, str], dict[str, dict[str, str]]]:
    if not search_path:
        raise GateError("local-gate host-tool search path is absent")
    command_directories: list[str] = []
    tools: dict[str, dict[str, str]] = {}
    expected: dict[str, Path] = {}
    for name in REQUIRED_HOST_TOOLS:
        selected = shutil.which(name, path=search_path)
        if selected is None:
            raise GateError(f"required local-gate host tool is unavailable: {name}")
        command = Path(selected)
        try:
            resolved = command.resolve(strict=True)
        except OSError as error:
            raise GateError(f"required local-gate host tool is unavailable: {name}") from error
        if not resolved.is_file() or resolved.is_symlink():
            raise GateError(f"required local-gate host tool is unsafe: {name}")
        info = resolved.stat()
        parent_info = command.parent.stat()
        if (
            info.st_uid not in {0, os.getuid()}
            or stat.S_IMODE(info.st_mode) & 0o022
            or parent_info.st_uid not in {0, os.getuid()}
            or stat.S_IMODE(parent_info.st_mode) & 0o022
        ):
            raise GateError(f"required local-gate host tool has unsafe ownership or mode: {name}")
        directory = str(command.parent)
        if directory not in command_directories:
            command_directories.append(directory)
        expected[name] = resolved
        tools[name] = {"path": str(resolved), "sha256": digest(resolved)}
    controlled_path = ":".join([*command_directories, SAFE_ENV["PATH"]])
    for name, resolved in expected.items():
        selected = shutil.which(name, path=controlled_path)
        if selected is None or Path(selected).resolve(strict=True) != resolved:
            raise GateError(f"local-gate host tool resolution is ambiguous: {name}")
    _validate_host_tools(tools)
    environment = {
        **SAFE_ENV,
        "PATH": controlled_path,
        "LS200_FUZZ_C_COMPILER": str(expected["clang"]),
        "LS200_LLVM_PROFDATA": str(expected["llvm-profdata"]),
        "LS200_LLVM_COV": str(expected["llvm-cov"]),
    }
    return environment, tools


def source_digest() -> str:
    try:
        return maintained_source_digest(
            root=ROOT, source_roots=SOURCE_ROOTS,
            evidence_source_roots=EVIDENCE_SOURCE_ROOTS,
            evidence_generator=EVIDENCE_GENERATOR,
        )
    except (OSError, SourceProvenanceError) as error:
        raise GateError(f"local gate source is unavailable or unsafe: {error}") from error


def build_receipt() -> dict[str, Any]:
    try:
        value = read_private_json(BUILD_RECEIPT)
    except FileNotFoundError as error:
        raise GateError("fresh live-build receipt is absent") from error
    if not isinstance(value, dict) or set(value) != {
        "schema", "artifacts", "input_manifest_sha256", "source_sha256",
    }:
        raise GateError("live-build receipt schema is inexact")
    if value["schema"] != "ls200-live-build-receipt-v1":
        raise GateError("live-build receipt is malformed")
    if not is_sha256(value["input_manifest_sha256"]):
        raise GateError("live-build input-manifest digest is malformed")
    if not is_sha256(value["source_sha256"]):
        raise GateError("live-build source digest is malformed")
    if value["source_sha256"] != source_digest():
        raise GateError("maintained source changed after live build")
    artifacts = value.get("artifacts")
    required = {"sipd", "gateway", "device", "nginx", "atomic_replace", "mime_types", "fastcgi_params", "ui_dist"}
    if not isinstance(artifacts, dict) or set(artifacts) != required:
        raise GateError("live-build artifact receipt is incomplete")
    return value


def package_digest(payload: Path) -> str:
    receipt = read_private_json(payload.parent / "package.json")
    if (not isinstance(receipt, dict) or set(receipt) != {"schema", "manifest_sha256"}
            or receipt["schema"] != "ls200-live-package-v1"
            or not is_sha256(receipt["manifest_sha256"])):
        raise GateError("independent package receipt is invalid")
    return receipt["manifest_sha256"]


def payload_digest() -> str:
    manifest = PAYLOAD / "payload-manifest.tsv"
    safe_repo_directory(PAYLOAD)
    if not PAYLOAD.is_dir() or PAYLOAD.is_symlink() or not manifest.is_file() or manifest.is_symlink():
        raise GateError("fresh physical payload is absent or unsafe")
    expected = package_digest(PAYLOAD)
    result = subprocess.run(
        ["/bin/sh", str(ROOT / "deployment/targets/ls200/verify-payload.sh"),
         "--payload", str(PAYLOAD), "--manifest-sha256", expected],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=SAFE_ENV, check=False,
    )
    if result.returncode != 0:
        raise GateError("trusted verifier rejected the physical payload")
    return expected


def exact_artifact_path(receipt: dict[str, Any], name: str) -> str:
    spec = receipt["artifacts"][name]
    if not isinstance(spec, dict) or set(spec) != {"path", "sha256"}:
        raise GateError(f"live-build artifact receipt is malformed: {name}")
    if not isinstance(spec["path"], str) or not is_sha256(spec["sha256"]):
        raise GateError(f"live-build artifact receipt is malformed: {name}")
    path = Path(spec["path"])
    if not path.is_absolute() or not path.is_file() or path.is_symlink() or digest(path) != spec["sha256"]:
        raise GateError(f"live-build artifact changed before local gates: {name}")
    return str(path)


def run_gates(entropy_helper: Path) -> None:
    if not entropy_helper.is_absolute() or not entropy_helper.is_file() or entropy_helper.is_symlink():
        raise GateError("QEMU entropy helper must be an absolute regular non-symlink file")
    receipt = build_receipt()
    manifest_sha256 = payload_digest()
    initial_source = source_digest()
    initial_entropy = digest(entropy_helper)
    host_environment, host_tools = _host_tool_environment(os.environ.get("PATH", ""))
    verify = subprocess.run(["/usr/bin/make", "verify"], cwd=ROOT, env=host_environment, check=False)
    if verify.returncode != 0:
        raise GateError("make verify failed; no live gate receipt was created")
    assignments = {
        "SIPD_BINARY": exact_artifact_path(receipt, "sipd"),
        "GATEWAY_BINARY": exact_artifact_path(receipt, "gateway"),
        "DEVICE_BINARY": exact_artifact_path(receipt, "device"),
        "NGINX_BINARY": exact_artifact_path(receipt, "nginx"),
        "ATOMIC_REPLACE_BINARY": exact_artifact_path(receipt, "atomic_replace"),
        "MIME_TYPES": exact_artifact_path(receipt, "mime_types"),
        "FASTCGI_PARAMS": exact_artifact_path(receipt, "fastcgi_params"),
        "QEMU_ENTROPY_HELPER": str(entropy_helper),
    }
    qemu = subprocess.run(
        ["/usr/bin/make", "qemu-acceptance", *(f"{key}={value}" for key, value in assignments.items())],
        cwd=ROOT, env=host_environment, check=False,
    )
    if qemu.returncode != 0:
        raise GateError("two-slot QEMU acceptance failed; no live gate receipt was created")
    _validate_host_tools(host_tools)
    if (source_digest() != initial_source or payload_digest() != manifest_sha256 or
            build_receipt() != receipt or digest(entropy_helper) != initial_entropy):
        raise GateError("source, payload, build receipt, or entropy helper changed during local gates")
    if GATE_RECEIPT.exists() or GATE_RECEIPT.is_symlink():
        raise GateError("local gate receipt already exists; a fresh campaign is required")
    result = {
        "schema": "ls200-live-local-gates-v1",
        "payload_manifest_sha256": manifest_sha256,
        "build_input_manifest_sha256": receipt.get("input_manifest_sha256"),
        "source_sha256": initial_source,
        "entropy_helper_sha256": initial_entropy,
        "host_tools": host_tools,
        "completed_ns": time.time_ns(),
        "make_verify": True,
        "two_slot_qemu_acceptance": True,
    }
    safe_repo_directory(GATE_RECEIPT.parent)
    directory = os.open(GATE_RECEIPT.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        descriptor = os.open(
            GATE_RECEIPT.name,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0),
            0o600,
            dir_fd=directory,
        )
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            json.dump(result, handle, sort_keys=True, indent=2)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.fsync(directory)
    finally:
        os.close(directory)


def _validate_gate_receipt(value: Any) -> None:
    expected_keys = {
        "schema", "payload_manifest_sha256", "build_input_manifest_sha256", "source_sha256",
        "entropy_helper_sha256", "host_tools", "completed_ns", "make_verify", "two_slot_qemu_acceptance",
    }
    if not isinstance(value, dict) or set(value) != expected_keys or value["schema"] != "ls200-live-local-gates-v1":
        raise GateError("local gate receipt schema is inexact")
    for key in (
        "payload_manifest_sha256", "build_input_manifest_sha256", "source_sha256",
        "entropy_helper_sha256",
    ):
        if not is_sha256(value[key]):
            raise GateError(f"local gate receipt digest is malformed: {key}")
    if isinstance(value["completed_ns"], bool) or not isinstance(value["completed_ns"], int) or value["completed_ns"] <= 0:
        raise GateError("local gate receipt completion time is malformed")
    if value["make_verify"] is not True or value["two_slot_qemu_acceptance"] is not True:
        raise GateError("local gate receipt does not prove the complete gate set")
    _validate_host_tools(value["host_tools"])


def _validate_gate_bindings(value: dict[str, Any], receipt: dict[str, Any]) -> None:
    if (
        value["payload_manifest_sha256"] != payload_digest()
        or value["build_input_manifest_sha256"] != receipt.get("input_manifest_sha256")
        or value["source_sha256"] != source_digest()
    ):
        raise GateError("payload, build inputs, or source changed after local gates")


def check_gates() -> None:
    try:
        value = read_private_json(GATE_RECEIPT)
    except FileNotFoundError as error:
        raise GateError("fresh local gate receipt is absent; run make live-gates") from error
    except OSError as error:
        raise GateError("fresh local gate receipt is unavailable") from error
    _validate_gate_receipt(value)
    _validate_gate_bindings(value, build_receipt())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--run", action="store_true")
    group.add_argument("--check", action="store_true")
    parser.add_argument("--entropy-helper", type=Path)
    args = parser.parse_args()
    try:
        if args.run:
            if args.entropy_helper is None:
                raise GateError("--run requires --entropy-helper")
            run_gates(args.entropy_helper)
        else:
            if args.entropy_helper is not None:
                raise GateError("--check does not accept --entropy-helper")
            check_gates()
        return 0
    except (GateError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"live-gates: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
