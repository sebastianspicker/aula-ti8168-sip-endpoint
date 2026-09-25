#!/usr/bin/env python3
"""Construct a deterministic, hash-verified LS-200 Zoom release payload.

Inputs are prebuilt artifacts only.  This tool never downloads, compiles, or
claims ARM compatibility for any component.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import stat
from pathlib import Path


DEPLOYMENT = Path(__file__).resolve().parent
RUNTIME = DEPLOYMENT / "runtime"
REPOSITORY = DEPLOYMENT.parent.parent
EVIDENCE_ROOT = REPOSITORY / "evidence"
WORK_ROOT = REPOSITORY / ".work"
FORMAT = "ls200-zoom-payload-v1"
INSTALL_PREFIXES = (
    "/opt/ls200-zoom",
    "/var/lib/cbox/ls200-zoom",
    "/run/ls200-zoom-state",
)


def die(message: str) -> None:
    raise SystemExit(f"build-payload: {message}")


def inside(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def canonical_input(path: Path, label: str) -> Path:
    lexical = path.absolute()
    try:
        resolved = path.resolve(strict=True)
    except OSError as error:
        die(f"{label} is unavailable: {error}")
    evidence = EVIDENCE_ROOT.resolve(strict=False)
    if inside(lexical, EVIDENCE_ROOT.absolute()) or inside(resolved, evidence):
        die(f"{label} must not come from repository evidence: {path}")
    return resolved


def regular(path: Path, label: str, executable: bool = False) -> Path:
    if not path.is_file() or path.is_symlink():
        die(f"{label} must be a regular non-symlink file: {path}")
    path = canonical_input(path, label)
    if executable and not path.stat().st_mode & stat.S_IXUSR:
        die(f"{label} must be owner-executable: {path}")
    return path


def directory(path: Path, label: str) -> Path:
    if not path.is_dir() or path.is_symlink():
        die(f"{label} must be a directory without a final symlink: {path}")
    return canonical_input(path, label)


def copy_file(source: Path, destination: Path, mode: int) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    os.chmod(destination, mode)


def copy_tree(source: Path, destination: Path) -> None:
    for item in sorted(source.rglob("*")):
        relative = item.relative_to(source)
        target = destination / relative
        if item.is_symlink():
            die(f"UI contains a symlink: {item}")
        if item.is_dir():
            target.mkdir(parents=True, exist_ok=True)
        elif item.is_file():
            copy_file(item, target, 0o444)
        else:
            die(f"UI contains a non-regular entry: {item}")


def normalize_directory_modes(root: Path) -> None:
    """Make every immutable release directory readable and traversable."""
    os.chmod(root, 0o555)
    for item in root.rglob("*"):
        if item.is_dir():
            os.chmod(item, 0o555)


def nginx_config(source: Path, install_prefix: str) -> bytes:
    value = regular(source, "nginx config").read_text(encoding="utf-8")
    replacements = {
        "root /var/lib/ls200-console/ui;": f"root {install_prefix}/current/ui;",
        "/usr/libexec/ls200-gateway-fcgi": f"{install_prefix}/current/bin/ls200-gateway-fcgi",
        "/etc/ls200-console/tls/": "/run/ls200-zoom-state/tls/",
        "pid /run/ls200-nginx.pid;": "pid /run/ls200-zoom-state/nginx/nginx.pid;",
        "/var/log/ls200-console/": "/run/ls200-zoom-state/nginx/",
        "include mime.types;": f"include {install_prefix}/current/etc/nginx/mime.types;",
        "include fastcgi_params;": f"include {install_prefix}/current/etc/nginx/fastcgi_params;",
    }
    for before, after in replacements.items():
        if before not in value:
            die(f"nginx config does not contain required packaging anchor: {before}")
        value = value.replace(before, after)
    if "listen 8443 ssl;" not in value or "/etc/nginx/mime.types;" not in value:
        die("nginx config must retain the 8443 TLS listener and mime include")
    return value.encode("utf-8")


def gateway_config(source: Path) -> bytes:
    value = json.loads(regular(source, "gateway config example").read_text(encoding="utf-8"))
    required = {"allowed_origin", "control_socket_path", "account_store_path", "expected_sipd_uid",
                "preview_enabled", "preview_rtsp_ipv4", "preview_rtsp_port"}
    if set(value) != required:
        die("gateway config example has an unexpected schema")
    if value["account_store_path"] != "/var/lib/ls200-console/account.json":
        die("gateway config example has an unexpected account-store path")
    value["account_store_path"] = "/run/ls200-zoom-state/gateway/account.json"
    return (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("utf-8")


def rewrite_runtime_prefix(path: Path, install_prefix: str) -> None:
    value = regular(path, "runtime path contract").read_text(encoding="utf-8")
    anchor = "/opt/ls200-zoom/current"
    if anchor not in value:
        die(f"runtime path contract lacks fixed prefix anchor: {path}")
    os.chmod(path, 0o644)
    path.write_text(value.replace(anchor, f"{install_prefix}/current"), encoding="utf-8")


def manifest(root: Path) -> str:
    entries: list[tuple[str, int, str, int]] = []
    for item in sorted(root.rglob("*")):
        if not item.is_file() or item.name == "payload-manifest.tsv":
            continue
        if item.is_symlink():
            die(f"payload contains a symlink: {item}")
        relative = item.relative_to(root).as_posix()
        raw = item.read_bytes()
        entries.append((relative, stat.S_IMODE(item.stat().st_mode), hashlib.sha256(raw).hexdigest(), len(raw)))
    header = f"{FORMAT}\n"
    rows = [f"{path}\t{mode:04o}\t{digest}\t{size}\n" for path, mode, digest, size in entries]
    manifest_path = root / "payload-manifest.tsv"
    manifest_path.write_text(header + "".join(rows), encoding="utf-8")
    os.chmod(manifest_path, 0o444)
    return hashlib.sha256(manifest_path.read_bytes()).hexdigest()


def checked_output(requested: Path) -> tuple[Path, Path]:
    work = WORK_ROOT.absolute()
    resolved_work = WORK_ROOT.resolve(strict=False)
    if work != resolved_work:
        die("repository .work must not be a symlink alias")
    lexical = requested.absolute()
    resolved = requested.resolve(strict=False)
    if not inside(lexical, work) or not inside(resolved, resolved_work) or resolved == resolved_work:
        die("output must be strictly below repository .work")
    receipt = resolved.parent / "payload-manifest.sha256"
    if (requested.exists() or requested.is_symlink() or receipt.exists() or receipt.is_symlink()
            or not requested.parent.is_dir() or requested.parent.is_symlink()):
        die("output must be new beside an absent digest receipt in an existing non-symlink parent")
    return resolved, receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sipd", required=True, type=Path)
    parser.add_argument("--gateway", required=True, type=Path)
    parser.add_argument("--device", required=True, type=Path)
    parser.add_argument("--nginx", required=True, type=Path)
    parser.add_argument("--atomic-replace", required=True, type=Path)
    parser.add_argument("--ui-dist", required=True, type=Path)
    parser.add_argument("--nginx-config", required=True, type=Path)
    parser.add_argument("--mime-types", required=True, type=Path)
    parser.add_argument("--fastcgi-params", required=True, type=Path)
    parser.add_argument("--sip-config", required=True, type=Path)
    parser.add_argument("--gateway-config", required=True, type=Path)
    parser.add_argument("--install-prefix", choices=INSTALL_PREFIXES,
                        default="/opt/ls200-zoom")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    output, digest_receipt = checked_output(args.output)
    directory(RUNTIME, "maintained runtime")
    for path, label, executable in ((args.sipd, "sipd", True), (args.gateway, "gateway", True),
                                    (args.nginx, "nginx", True), (args.device, "device companion", True),
                                    (args.atomic_replace, "atomic replacement helper", True),
                                    (args.mime_types, "mime types", False),
                                    (args.fastcgi_params, "FastCGI parameters", False),
                                    (args.sip_config, "sip config example", False)):
        regular(path, label, executable)
    directory(args.ui_dist, "UI dist")
    regular(args.ui_dist / "index.html", "UI dist index")

    shutil.copytree(RUNTIME, output, copy_function=shutil.copy2, symlinks=False)
    for item in output.rglob("*"):
        if item.is_file():
            os.chmod(item, 0o444)
    for executable in (output / "bin/ls200-zoom-service",
                       output / "bin/verify-ls200-zoom-payload",
                       output / "etc/init.d/S99ls200-zoom"):
        os.chmod(executable, 0o555)
    rewrite_runtime_prefix(output / "bin/ls200-zoom-service", args.install_prefix)
    rewrite_runtime_prefix(output / "etc/init.d/S99ls200-zoom", args.install_prefix)
    os.chmod(output / "bin/ls200-zoom-service", 0o555)
    os.chmod(output / "etc/init.d/S99ls200-zoom", 0o555)
    copy_file(args.sipd, output / "bin/ls200-sipd", 0o555)
    copy_file(args.gateway, output / "bin/ls200-gateway-fcgi", 0o555)
    copy_file(args.device, output / "bin/ls200-device-control", 0o555)
    copy_file(args.nginx, output / "bin/nginx", 0o555)
    copy_file(args.atomic_replace, output / "bin/ls200-atomic-replace", 0o555)
    copy_file(args.mime_types, output / "etc/nginx/mime.types", 0o444)
    copy_file(args.fastcgi_params, output / "etc/nginx/fastcgi_params", 0o444)
    (output / "etc/nginx/nginx.conf").write_bytes(
        nginx_config(args.nginx_config, args.install_prefix))
    os.chmod(output / "etc/nginx/nginx.conf", 0o444)
    copy_file(args.sip_config, output / "etc/ls200-zoom/ls200-sipd.conf.example", 0o444)
    (output / "etc/ls200-zoom/gateway.conf.example").write_bytes(gateway_config(args.gateway_config))
    os.chmod(output / "etc/ls200-zoom/gateway.conf.example", 0o444)
    # nginx serves the console below /zoom/ with a normal `root` directive.
    # Preserve that URI prefix in the payload so /zoom/index.html resolves to
    # <release>/ui/zoom/index.html and absolute /zoom/assets/* URLs remain
    # inside the immutable release tree.
    copy_tree(args.ui_dist, output / "ui" / "zoom")
    manifest_sha256 = manifest(output)
    digest_receipt.write_text(manifest_sha256 + "\n", encoding="ascii")
    os.chmod(digest_receipt, 0o444)
    normalize_directory_modes(output)
    print(json.dumps({"format": FORMAT, "install_prefix": args.install_prefix,
                      "payload": str(output),
                      "manifest": str(output / "payload-manifest.tsv"),
                      "manifest_sha256": manifest_sha256,
                      "manifest_sha256_file": str(digest_receipt)}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
