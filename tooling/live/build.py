#!/usr/bin/env python3
"""Rebuild the live ARM payload only from a complete hash-reviewed input set."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path
from typing import Any

import receipts
from source_provenance import SourceProvenanceError
from source_provenance import source_digest as maintained_source_digest


ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work"
sys.path.insert(0, str(ROOT / "tooling/workspace"))
from protected_output import input_path, output_path  # noqa: E402

ENV_PATH_KEYS = {
    "pjsip_include": "AULA_SIPD_PJSIP_INCLUDE_DIR",
    "pjsip_libraries": "AULA_SIPD_PJSIP_LIBRARIES",
    "pjsip_ua_library": "AULA_SIPD_PJSIP_UA_LIBRARY",
    "pjmedia_library": "AULA_SIPD_PJMEDIA_LIBRARY",
    "faad2_include": "AULA_SIPD_FAAD2_INCLUDE_DIR",
    "faad2_library": "AULA_SIPD_FAAD2_LIBRARY",
    "speexdsp_include": "AULA_SIPD_SPEEXDSP_INCLUDE_DIR",
    "speexdsp_library": "AULA_SIPD_SPEEXDSP_LIBRARY",
    "srtp_include": "AULA_SIPD_SRTP_INCLUDE_DIR",
    "srtp_library": "AULA_SIPD_SRTP_LIBRARY",
}


class BuildError(RuntimeError):
    pass


def source_digest() -> str:
    try:
        return maintained_source_digest()
    except (OSError, SourceProvenanceError) as error:
        raise BuildError(f"maintained source is unavailable or unsafe: {error}") from error


def _verify_source_digest(expected: str) -> None:
    if source_digest() != expected:
        raise BuildError("maintained source changed during live build")


def _safe_root_directory(info: os.stat_result, *, private: bool) -> bool:
    mode = stat.S_IMODE(info.st_mode)
    return (
        stat.S_ISDIR(info.st_mode)
        and not stat.S_ISLNK(info.st_mode)
        and info.st_uid == os.getuid()
        and (mode == 0o700 if private else not mode & 0o022)
    )


def _private_directory(info: os.stat_result) -> bool:
    return (
        stat.S_ISDIR(info.st_mode)
        and not stat.S_ISLNK(info.st_mode)
        and info.st_uid == os.getuid()
        and stat.S_IMODE(info.st_mode) == 0o700
    )


_is_sha256 = receipts.is_sha256
digest = receipts.digest


def exact_json(raw: bytes) -> Any:
    return receipts.exact_json(raw, error=BuildError, context="build-input")


def _validate_private_manifest_parent(path: Path) -> None:
    allowed_roots = (WORK,)
    try:
        resolved_parent = path.parent.resolve(strict=True)
    except OSError as error:
        raise BuildError("build-input manifest parent is unavailable") from error
    root = next((item for item in allowed_roots if resolved_parent == item.resolve() or resolved_parent.is_relative_to(item.resolve())), None)
    if root is None:
        try:
            info = path.parent.lstat()
        except OSError as error:
            raise BuildError("build-input manifest private parent is unavailable") from error
        if not _private_directory(info):
            raise BuildError("build-input manifest private parent must be operator-owned 0700")
        return
    if not _safe_root_directory(root.lstat(), private=False):
        raise BuildError("build-input manifest private root is unsafe")
    current = root
    for component in path.parent.relative_to(root).parts:
        current /= component
        if not _private_directory(current.lstat()):
            raise BuildError("build-input manifest private parent must be operator-owned 0700")


def _write_receipt(path: Path, value: dict[str, Any]) -> None:
    descriptor = os.open(
        path,
        os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0),
        0o600,
    )
    try:
        rendered = (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("utf-8")
        view = memoryview(rendered)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                raise BuildError("build receipt write did not complete")
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    directory = os.open(path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) | getattr(os, "O_CLOEXEC", 0))
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def _validate_work_output_parent(path: Path, *, private: bool = False) -> None:
    try:
        info = path.lstat()
    except OSError as error:
        raise BuildError(f"live-build output parent is unavailable: {path.relative_to(ROOT)}") from error
    mode = stat.S_IMODE(info.st_mode)
    if (
        not stat.S_ISDIR(info.st_mode)
        or stat.S_ISLNK(info.st_mode)
        or info.st_uid != os.getuid()
        or (mode != 0o700 if private else bool(mode & 0o022))
    ):
        raise BuildError(f"live-build output parent is unsafe: {path.relative_to(ROOT)}")


def tree_receipt(path: Path) -> dict[str, Any]:
    files: dict[str, str] = {}
    for entry in sorted(path.rglob("*")):
        mode = entry.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode) or entry.is_symlink():
            raise BuildError("UI output contains a symlink or special file")
        files[entry.relative_to(path).as_posix()] = digest(entry)
    if not files:
        raise BuildError("UI build produced no files")
    return {"path": str(path.resolve()), "files": files}


def exact_file(value: Any, label: str) -> Path:
    if not isinstance(value, dict) or set(value) != {"path", "sha256"}:
        raise BuildError(f"{label} must contain exact path and sha256 fields")
    path = Path(value["path"])
    expected = value["sha256"]
    if not path.is_absolute() or not path.is_file() or path.is_symlink():
        raise BuildError(f"{label} must be an absolute regular non-symlink file")
    if not _is_sha256(expected):
        raise BuildError(f"{label} SHA-256 is invalid")
    if digest(path) != expected:
        raise BuildError(f"{label} SHA-256 mismatch")
    return path.resolve()


def verify_tree(path: Path, manifest_spec: Any, label: str) -> None:
    if not path.is_absolute() or not path.is_dir() or path.is_symlink():
        raise BuildError(f"{label} must be an absolute non-symlink directory")
    manifest = exact_file(manifest_spec, f"{label} manifest")
    rows: dict[str, str] = {}
    for number, line in enumerate(manifest.read_text(encoding="utf-8").splitlines(), 1):
        parts = line.split("\t")
        if len(parts) != 2:
            raise BuildError(f"{label} manifest line {number} is malformed")
        expected, relative = parts
        if relative in rows or relative.startswith("/") or ".." in Path(relative).parts:
            raise BuildError(f"{label} manifest contains an unsafe or duplicate path")
        if not _is_sha256(expected):
            raise BuildError(f"{label} manifest contains an invalid digest")
        rows[relative] = expected
    actual: dict[str, str] = {}
    for entry in sorted(path.rglob("*")):
        mode = entry.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode) or entry.is_symlink():
            raise BuildError(f"{label} contains a symlink or special file")
        relative = entry.relative_to(path).as_posix()
        actual[relative] = digest(entry)
    if actual != rows:
        raise BuildError(f"{label} tree does not exactly match its reviewed manifest")


def _canonical_reviewed_input(path: Path, label: str) -> Path:
    if not path.is_absolute():
        raise BuildError(f"{label} must be absolute")
    try:
        resolved = input_path(ROOT, str(path))
    except (OSError, RuntimeError, ValueError) as error:
        raise BuildError(f"{label} is not an allowed reviewed input") from error
    if path != resolved:
        raise BuildError(f"{label} path must be canonical")
    return resolved


def _tree_digests(path: Path, label: str) -> dict[str, str]:
    files: dict[str, str] = {}
    for entry in sorted(path.rglob("*")):
        mode = entry.lstat().st_mode
        if stat.S_ISDIR(mode):
            continue
        if not stat.S_ISREG(mode) or entry.is_symlink():
            raise BuildError(f"{label} contains a symlink or special file")
        files[entry.relative_to(path).as_posix()] = digest(entry)
    if not files:
        raise BuildError(f"{label} is empty")
    return files


def _stage_nginx_source(reviewed_source: Path, build: Path) -> Path:
    try:
        staged_source = output_path(ROOT, str(build / "nginx-source"))
    except (OSError, RuntimeError, ValueError) as error:
        raise BuildError("live-build nginx staging path is unsafe") from error
    before = _tree_digests(reviewed_source, "reviewed nginx source")
    try:
        shutil.copytree(reviewed_source, staged_source, copy_function=shutil.copy2)
    except OSError as error:
        raise BuildError("live-build could not stage the reviewed nginx source") from error
    if _tree_digests(staged_source, "staged nginx source") != before or \
            _tree_digests(reviewed_source, "reviewed nginx source") != before:
        raise BuildError("reviewed nginx source changed while it was staged")
    return staged_source


def run(argv: list[str], env: dict[str, str], *, cwd: Path | None = None) -> None:
    completed = subprocess.run(argv, cwd=cwd, env=env, check=False)
    if completed.returncode != 0:
        raise BuildError(f"build command failed: {Path(argv[0]).name}")


def _read_input_manifest(path: Path) -> tuple[bytes, dict[str, Any]]:
    if not path.is_absolute():
        raise SystemExit("live-build: manifest must be absolute")
    _validate_private_manifest_parent(path)
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    descriptor: int | None = None
    try:
        descriptor = os.open(path, flags)
        info = os.fstat(descriptor)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or info.st_nlink != 1
            or stat.S_IMODE(info.st_mode) & 0o022
        ):
            raise BuildError("build-input manifest must be singly linked, operator-owned, and not group/world writable")
        handle = os.fdopen(descriptor, "rb")
        descriptor = None
        with handle:
            raw = handle.read(64 * 1024 + 1)
    finally:
        if descriptor is not None:
            os.close(descriptor)
    if len(raw) > 64 * 1024:
        raise SystemExit("live-build: manifest is too large")
    document = exact_json(raw)
    required = {"schema", "toolchain", "sysroot", "nginx_source", "dependencies", "reference_binary"}
    if not isinstance(document, dict) or set(document) != required or document["schema"] != "aula-ti8168-sip-endpoint-live-build-inputs-v1":
        raise SystemExit("live-build: unsupported or inexact build-input schema")
    return raw, document


def _validate_toolchain(document: dict[str, Any]) -> tuple[dict[str, Any], str]:
    toolchain = document["toolchain"]
    required = {"cross_prefix", "gcc", "ar", "ranlib", "readelf", "strip"}
    if not isinstance(toolchain, dict) or set(toolchain) not in (required, required | {'node'}):
        raise SystemExit("live-build: toolchain schema is inexact")
    cross_prefix = toolchain["cross_prefix"]
    if not isinstance(cross_prefix, str) or not Path(cross_prefix).is_absolute():
        raise SystemExit("live-build: cross_prefix must be absolute")
    for executable in ("gcc", "ar", "ranlib", "readelf", "strip"):
        path = exact_file(toolchain[executable], f"toolchain {executable}")
        if str(path) != cross_prefix + executable or not os.access(path, os.X_OK):
            raise SystemExit(f"live-build: {executable} does not match the reviewed prefix")
    if 'node' in toolchain:
        node = exact_file(toolchain['node'], 'host Node executable')
        if node.name != 'node' or not os.access(node, os.X_OK):
            raise SystemExit('live-build: reviewed host Node must be an executable named node')
    return toolchain, cross_prefix


def _validate_inputs(
    document: dict[str, Any], cross_prefix: str,
) -> tuple[Path, Path, Path, dict[str, str]]:
    sysroot_spec = document["sysroot"]
    nginx_spec = document["nginx_source"]
    if not isinstance(sysroot_spec, dict) or set(sysroot_spec) != {"path", "tree_manifest"}:
        raise SystemExit("live-build: sysroot schema is inexact")
    if not isinstance(nginx_spec, dict) or set(nginx_spec) != {"path", "tree_manifest"}:
        raise SystemExit("live-build: nginx_source schema is inexact")
    sysroot = _canonical_reviewed_input(Path(sysroot_spec["path"]), "sysroot")
    nginx_source = _canonical_reviewed_input(
        Path(nginx_spec["path"]), "nginx source"
    )
    verify_tree(sysroot, sysroot_spec["tree_manifest"], "sysroot")
    verify_tree(nginx_source, nginx_spec["tree_manifest"], "nginx source")
    dependencies = document["dependencies"]
    if not isinstance(dependencies, dict) or set(dependencies) != set(ENV_PATH_KEYS):
        raise SystemExit("live-build: dependency path schema is inexact")
    environment = {
        "PATH": f"{Path(cross_prefix).parent}:/usr/bin:/bin:/usr/sbin:/sbin",
        "LANG": "C", "LC_ALL": "C", "AULA_CROSS_PREFIX": cross_prefix,
        "AULA_SYSROOT": str(sysroot.resolve()), "AULA_BUILD_JOBS": "1",
    }
    canonical_sysroot = sysroot.resolve()
    for key, variable in ENV_PATH_KEYS.items():
        value = dependencies[key]
        values = value.split(";") if key == "pjsip_libraries" and isinstance(value, str) else [value]
        resolved_values: list[str] = []
        for item in values:
            candidate = Path(item)
            if not candidate.is_absolute() or not candidate.exists():
                raise SystemExit(f"live-build: dependency path is unavailable: {key}")
            resolved = candidate.resolve()
            if not resolved.is_relative_to(canonical_sysroot):
                raise SystemExit(f"live-build: dependency escaped the reviewed sysroot: {key}")
            resolved_values.append(str(resolved))
        environment[variable] = ";".join(resolved_values)
    reference = exact_file(document["reference_binary"], "recovered ABI reference")
    return sysroot, nginx_source, reference, environment


def _prepare_build_directories() -> tuple[Path, Path]:
    build = WORK / "build/live"
    ui_dist = WORK / "dist/console-web"
    for parent in (WORK, WORK / "build", WORK / "dist"):
        _validate_work_output_parent(parent)
    if build.exists() or build.is_symlink():
        raise SystemExit("live-build: .work/build/live must be absent for a fresh build")
    if ui_dist.exists() or ui_dist.is_symlink():
        raise SystemExit("live-build: .work/dist/console-web must be absent for a fresh build")
    build.mkdir(parents=True, mode=0o700)
    os.chmod(build, 0o700)
    for directory in (build / "sipd", build / "gateway", build / "atomic"):
        directory.mkdir(mode=0o700)
        os.chmod(directory, 0o700)
    return build, ui_dist


def _build_artifacts(
    build: Path, ui_dist: Path, nginx_source: Path, sysroot: Path, reference: Path,
    toolchain: dict[str, Any], cross_prefix: str, environment: dict[str, str],
) -> dict[str, Path]:
    environment["AULA_ARM_BUILD_DIR"] = str(build / "sipd")
    environment["AULA_GATEWAY_ARM_BUILD_DIR"] = str(build / "gateway")
    environment["AULA_NGINX_ARM_BUILD_DIR"] = str(nginx_source)
    run(["/bin/sh", str(ROOT / "product/sipd/tools/build-arm.sh")], environment)
    run(["/bin/sh", str(ROOT / "product/console/gateway/config/build-gateway-arm.sh")], environment)
    run(["/usr/bin/make", "-C", str(ROOT / "product/console"), "device-control",
         "CC=" + cross_prefix + "gcc", "BUILD=" + str(build / "device"),
         "CPPFLAGS=-I" + str(sysroot / "usr/include"),
         "LDLIBS=" + " ".join(str(sysroot / "usr/lib" / name) for name in
                             ("libjansson.a", "libcrypto.a")) + " -lm -lpthread -lrt -ldl"], environment)
    atomic = build / "atomic/aula-atomic-replace"
    atomic_env = dict(environment)
    atomic_env["CC"] = cross_prefix + "gcc"
    run(["/bin/sh", str(ROOT / "deployment/payload/build-atomic-replace-arm.sh"), str(atomic)], atomic_env)
    run(["/bin/sh", str(ROOT / "product/console/nginx/build-nginx-1.31.4.sh")], environment, cwd=nginx_source)
    # UI assets are rebuilt locally without dependency acquisition.
    ui_environment = dict(environment, AULA_CONSOLE_REQUIRE_READY='1')
    if 'node' in toolchain:
        node = Path(toolchain['node']['path']).resolve()
        ui_environment['PATH'] = str(node.parent) + os.pathsep + environment.get('PATH', '/usr/bin:/bin')
    run(["/usr/bin/make", "-C", str(ROOT / "product/console"), "ui-build"], ui_environment)
    artifacts = {
        "sipd": build / "sipd/aula-sipd",
        "gateway": build / "gateway/aula-gateway-fcgi",
        "device": build / "device/aula-device-control",
        "nginx": nginx_source / "objs/nginx",
        "atomic_replace": atomic,
        "mime_types": nginx_source / "conf/mime.types",
        "fastcgi_params": nginx_source / "conf/fastcgi_params",
    }
    readelf = str(Path(toolchain["readelf"]["path"]).resolve())
    abi_env = dict(environment)
    abi_env["READELF"] = readelf
    for name in ("sipd", "gateway", "device", "nginx", "atomic_replace"):
        run([
            "/bin/sh", str(ROOT / "product/sipd/tools/check-arm-abi.sh"),
            "--binary", str(artifacts[name]), "--reference", str(reference), "--sysroot", str(sysroot),
        ], abi_env)
    strip = str(Path(toolchain["strip"]["path"]).resolve())
    for name in ("sipd", "gateway", "device", "nginx", "atomic_replace"):
        run([strip, "--strip-unneeded", str(artifacts[name])], environment)
        run([
            "/bin/sh", str(ROOT / "product/sipd/tools/check-arm-abi.sh"),
            "--binary", str(artifacts[name]), "--reference", str(reference), "--sysroot", str(sysroot),
        ], abi_env)
    return artifacts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    raw, document = _read_input_manifest(args.manifest)
    toolchain, cross_prefix = _validate_toolchain(document)
    sysroot, nginx_source, reference, environment = _validate_inputs(document, cross_prefix)
    initial_source = source_digest()
    build, ui_dist = _prepare_build_directories()
    staged_nginx_source = _stage_nginx_source(nginx_source, build)
    _verify_source_digest(initial_source)
    artifacts = _build_artifacts(
        build, ui_dist, staged_nginx_source, sysroot, reference, toolchain,
        cross_prefix, environment,
    )
    # Long builds must not attest reviewed inputs which changed while consumed.
    _validate_toolchain(document)
    _validate_inputs(document, cross_prefix)
    _verify_source_digest(initial_source)
    if args.manifest.read_bytes() != raw:
        raise BuildError("reviewed build manifest changed during build")
    receipt = {
        "schema": "aula-ti8168-sip-endpoint-live-build-receipt-v1",
        "artifacts": {
            **{name: {"path": str(path.resolve()), "sha256": digest(path)} for name, path in artifacts.items()},
            "ui_dist": tree_receipt(ui_dist),
        },
        "input_manifest_sha256": hashlib.sha256(raw).hexdigest(),
        "source_sha256": initial_source,
    }
    receipt_path = build / "receipt.json"
    _write_receipt(receipt_path, receipt)
    print(receipt_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
