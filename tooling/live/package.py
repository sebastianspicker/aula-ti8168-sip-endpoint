#!/usr/bin/env python3
"""Package one fresh, manifest-verified TI8168 media-board runtime."""

from __future__ import annotations

import json
import os
import stat
import subprocess
from pathlib import Path
from typing import Any

import receipts
from payload_receipt import read_manifest_digest
from source_provenance import SourceProvenanceError
from source_provenance import source_digest as maintained_source_digest


ROOT = Path(__file__).resolve().parents[2]
RECEIPT = ROOT / ".work/build/live/receipt.json"
OUTPUT = ROOT / ".work/dist/aula-ti8168-sip-endpoint-live/runtime"

digest = receipts.digest


def source_digest() -> str:
    try:
        return maintained_source_digest()
    except (OSError, SourceProvenanceError) as error:
        raise SystemExit(
            f"live-package: maintained source is unavailable or unsafe: {error}"
        ) from error


def _verify_receipt_source(receipt: dict[str, Any]) -> None:
    expected = receipt.get("source_sha256")
    if not isinstance(expected, str) or len(expected) != 64 or any(
        character not in "0123456789abcdef" for character in expected
    ):
        raise SystemExit("live-package: build receipt source digest is missing or malformed")
    if source_digest() != expected:
        raise SystemExit("live-package: maintained source changed after live build")


def _exact_json(raw: bytes) -> Any:
    return receipts.exact_json(raw, context="package receipt")


def _safe_operator_directory(path: Path, *, create: bool, label: str) -> None:
    if create:
        try:
            path.mkdir(mode=0o700)
        except FileExistsError:
            pass
    try:
        info = path.lstat()
    except OSError as error:
        raise SystemExit(f"live-package: {label} is unavailable") from error
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.getuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise SystemExit(f"live-package: {label} must be an operator-owned private 0700 directory")


def _read_receipt() -> dict[str, Any]:
    for path, private in ((ROOT / ".work", False), (ROOT / ".work/build", False), (ROOT / ".work/build/live", True)):
        try:
            parent_info = path.lstat()
        except OSError as error:
            raise SystemExit("live-package: live-build receipt parent is unavailable") from error
        if (
            not stat.S_ISDIR(parent_info.st_mode)
            or stat.S_ISLNK(parent_info.st_mode)
            or parent_info.st_uid != os.getuid()
            or (stat.S_IMODE(parent_info.st_mode) != 0o700 if private else stat.S_IMODE(parent_info.st_mode) & 0o022)
        ):
            raise SystemExit("live-package: live-build receipt parent is unsafe")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(RECEIPT, flags)
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) != 0o600
        ):
            raise SystemExit("live-package: fresh live-build receipt is unsafe")
        with os.fdopen(descriptor, "rb") as handle:
            raw = handle.read(64 * 1024 + 1)
        if len(raw) > 64 * 1024:
            raise SystemExit("live-package: fresh live-build receipt is too large")
        value = _exact_json(raw)
    except (OSError, ValueError, UnicodeDecodeError) as error:
        raise SystemExit("live-package: fresh live-build receipt is absent or malformed") from error
    if not isinstance(value, dict):
        raise SystemExit("live-package: build receipt schema is inexact")
    return value


def _write_package_receipt(path: Path, value: dict[str, str]) -> None:
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0),
        0o600,
    )
    try:
        rendered = (json.dumps(value, sort_keys=True) + "\n").encode("utf-8")
        view = memoryview(rendered)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise SystemExit("live-package: package receipt write did not complete")
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_CLOEXEC", 0))
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def _verified_ui_artifact(spec: Any) -> Path:
    if not isinstance(spec, dict) or set(spec) != {"path", "files"} or not isinstance(spec["files"], dict):
        raise SystemExit("live-package: malformed UI receipt")
    directory = Path(spec["path"])
    if not directory.is_absolute() or not directory.is_dir() or directory.is_symlink():
        raise SystemExit("live-package: UI output is absent or unsafe")
    actual: dict[str, str] = {}
    for entry in sorted(directory.rglob("*")):
        if entry.is_dir() and not entry.is_symlink():
            continue
        if not entry.is_file() or entry.is_symlink():
            raise SystemExit("live-package: UI output contains a symlink or special file")
        actual[entry.relative_to(directory).as_posix()] = digest(entry)
    if not actual or actual != spec["files"]:
        raise SystemExit("live-package: UI output changed after build")
    return directory


def _verified_file_artifact(name: str, spec: Any) -> Path:
    if not isinstance(spec, dict) or set(spec) != {"path", "sha256"}:
        raise SystemExit(f"live-package: malformed receipt entry: {name}")
    path = Path(spec["path"])
    if not path.is_absolute() or not path.is_file() or path.is_symlink() or digest(path) != spec["sha256"]:
        raise SystemExit(f"live-package: artifact changed after build: {name}")
    return path


def _verified_artifacts(receipt: Any) -> dict[str, Path]:
    if not isinstance(receipt, dict) or set(receipt) != {
        "schema", "artifacts", "input_manifest_sha256", "source_sha256",
    }:
        raise SystemExit("live-package: build receipt schema is inexact")
    if receipt["schema"] != "aula-ti8168-sip-endpoint-live-build-receipt-v1":
        raise SystemExit("live-package: build receipt schema is unsupported")
    _verify_receipt_source(receipt)
    artifacts = receipt["artifacts"]
    required = {"sipd", "gateway", "device", "nginx", "atomic_replace", "mime_types", "fastcgi_params", "ui_dist"}
    if not isinstance(artifacts, dict) or set(artifacts) != required:
        raise SystemExit("live-package: artifact receipt is incomplete")
    paths: dict[str, Path] = {}
    for name, spec in artifacts.items():
        if name == "ui_dist":
            paths[name] = _verified_ui_artifact(spec)
        else:
            paths[name] = _verified_file_artifact(name, spec)
    return paths


def _verify_packaged_artifacts(receipt: dict[str, Any]) -> None:
    leaves = {
        "sipd": "bin/aula-sipd", "gateway": "bin/aula-gateway-fcgi", "device": "bin/aula-device-control",
        "nginx": "bin/nginx", "atomic_replace": "bin/aula-atomic-replace",
        "mime_types": "etc/nginx/mime.types", "fastcgi_params": "etc/nginx/fastcgi_params",
    }
    for name, leaf in leaves.items():
        spec = {"path": str(OUTPUT / leaf), "sha256": receipt["artifacts"][name]["sha256"]}
        _verified_file_artifact(name, spec)
    _verified_ui_artifact({**receipt["artifacts"]["ui_dist"], "path": str(OUTPUT / "ui/zoom")})


def _validate_output_roots() -> None:
    work = ROOT / ".work"
    work_info = work.lstat()
    if not stat.S_ISDIR(work_info.st_mode) or stat.S_ISLNK(work_info.st_mode) or work_info.st_uid != os.getuid() or stat.S_IMODE(work_info.st_mode) & 0o022:
        raise SystemExit("live-package: .work output root is unsafe")
    dist = work / "dist"
    dist_info = dist.lstat()
    if not stat.S_ISDIR(dist_info.st_mode) or stat.S_ISLNK(dist_info.st_mode) or dist_info.st_uid != os.getuid() or stat.S_IMODE(dist_info.st_mode) & 0o022:
        raise SystemExit("live-package: .work/dist output root is unsafe")
    _safe_operator_directory(OUTPUT.parent, create=True, label="live package output parent")
    if OUTPUT.exists() or OUTPUT.is_symlink():
        raise SystemExit("live-package: output already exists; a fresh package is required")


def _build_payload(paths: dict[str, Path]) -> None:
    command = [
        "python3", "-B", str(ROOT / "deployment/payload/build-payload.py"),
        "--sipd", str(paths["sipd"]), "--gateway", str(paths["gateway"]), "--device", str(paths["device"]),
        "--nginx", str(paths["nginx"]), "--atomic-replace", str(paths["atomic_replace"]),
        "--ui-dist", str(paths["ui_dist"]),
        "--nginx-config", str(ROOT / "product/console/nginx/nginx.conf"),
        "--mime-types", str(paths["mime_types"]), "--fastcgi-params", str(paths["fastcgi_params"]),
        "--sip-config", str(ROOT / "product/sipd/config/aula-sipd.example.conf"),
        "--gateway-config", str(ROOT / "product/console/gateway/config/aula-console.example.conf"),
        "--install-prefix", "/run/aula-state", "--output", str(OUTPUT),
    ]
    completed = subprocess.run(command, env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C", "LC_ALL": "C"}, check=False)
    if completed.returncode != 0:
        raise SystemExit("live-package: payload builder failed")


def _verify_payload() -> str:
    try:
        expected = read_manifest_digest(OUTPUT)
    except (OSError, ValueError) as error:
        raise SystemExit(f"live-package: independent digest receipt is unavailable: {error}") from error
    verified = subprocess.run(
        ["/bin/sh", str(ROOT / "deployment/targets/ti8168/verify-payload.sh"),
         "--payload", str(OUTPUT), "--manifest-sha256", expected],
        env={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "LANG": "C", "LC_ALL": "C"}, check=False,
    )
    if verified.returncode != 0:
        raise SystemExit("live-package: trusted live verifier rejected the package")
    return expected


def main() -> int:
    receipt = _read_receipt()
    paths = _verified_artifacts(receipt)
    _validate_output_roots()
    _build_payload(paths)
    _verify_receipt_source(receipt)
    _verify_packaged_artifacts(receipt)
    expected = _verify_payload()
    metadata = OUTPUT.parent / "package.json"
    _write_package_receipt(metadata, {"schema": "aula-ti8168-sip-endpoint-live-package-v1", "manifest_sha256": expected})
    print(OUTPUT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
