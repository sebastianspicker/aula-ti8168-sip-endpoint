#!/usr/bin/env python3
"""Acquire a public Squeeze armel development ABI from signed Debian metadata.

This historical development sysroot is for compatibility experiments only.
No firmware is accepted as a build input and no package scripts are executed.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import subprocess
import urllib.request
from pathlib import Path, PurePosixPath

from run import create_output, executable, inventory, sha256

SIGNERS = {
    "9FED2BCBDCD29CDF762678CBAED4B06F473041FA",
    "0E4EDE2C7F3E1FC0D033800E64481591B98321F9",
}
PACKAGES = ("libc6", "libc6-dev", "linux-libc-dev")


def normalize_links(sysroot: Path) -> None:
    for path in sysroot.rglob("*"):
        if path.is_symlink():
            destination = os.readlink(path)
            if destination.startswith("/"):
                target = sysroot / destination.lstrip("/")
                path.unlink()
                path.symlink_to(os.path.relpath(target, path.parent))


def reconstruct_verified_sysroot(cache: Path, output: Path, keyring: Path, gpgv: Path, dpkg: Path) -> Path:
    """Independently reverify cached packages and reconstruct their build tree.

    A locally editable acquisition receipt is useful evidence, not a trust
    authority. The source lock and Debian signature chain remain authoritative.
    """
    lock = json.loads(Path(__file__).with_name("legacy-debian-lock.json").read_text())
    metadata = output / "verified-metadata"
    metadata.mkdir()
    for name, expected in (("Release", lock["release_sha256"]),
                           ("Packages.gz", lock["package_index_sha256"]), ("Release.gpg", None)):
        raw = (cache / "metadata" / name).read_bytes()
        if expected is not None and hashlib.sha256(raw).hexdigest() != expected:
            raise ValueError(f"cached {name} differs from the frozen public lock")
        (metadata / name).write_bytes(raw)
    result = subprocess.run([str(gpgv), "--status-fd", "1", "--keyring", str(keyring),
                             str(metadata / "Release.gpg"), str(metadata / "Release")],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False)
    (metadata / "signature-status.log").write_bytes(result.stdout + result.stderr)
    fingerprints = re.findall(rb"^\[GNUPG:\] VALIDSIG ([0-9A-F]+) ", result.stdout, re.MULTILINE)
    if result.returncode or not ({item.decode("ascii") for item in fingerprints} & SIGNERS):
        raise ValueError("cached Debian archive signature failed independent verification")
    release = stanzas((metadata / "Release").read_text())[0]
    if not any(line.split() == [lock["package_index_sha256"], str((metadata / "Packages.gz").stat().st_size),
                                "main/binary-armel/Packages.gz"] for line in release["SHA256"].splitlines()):
        raise ValueError("cached package index is absent from the signed Release")
    records = stanzas(gzip.decompress((metadata / "Packages.gz").read_bytes()).decode("utf-8"))
    packages = output / "verified-packages"
    packages.mkdir()
    sysroot = output / "public-build-sysroot"
    sysroot.mkdir()
    for pinned in lock["packages"]:
        matches = [entry for entry in records if (entry.get("Package"), entry.get("Architecture"),
                   entry.get("Version"), entry.get("Filename"), entry.get("SHA256")) ==
                   (pinned["name"], "armel", pinned["version"], pinned["filename"], pinned["sha256"])]
        if len(matches) != 1:
            raise ValueError("locked public package is not uniquely authenticated by the signed index")
        name = Path(pinned["filename"]).name
        raw = (cache / "packages" / name).read_bytes()
        if hashlib.sha256(raw).hexdigest() != pinned["sha256"] or len(raw) != int(matches[0]["Size"]):
            raise ValueError("cached public package differs from the signed index")
        package = packages / name
        package.write_bytes(raw)
        subprocess.run([str(dpkg), "--extract", str(package), str(sysroot)], timeout=60, check=True)
    normalize_links(sysroot)
    return sysroot


def acquire(url: str, output: Path, expected: str | None = None, size: int | None = None) -> bytes:
    with urllib.request.urlopen(url, timeout=60) as response:
        raw = response.read(64 * 1024 * 1024 + 1)
    if len(raw) > 64 * 1024 * 1024 or (size is not None and len(raw) != size):
        raise ValueError(f"download size rejected: {url}")
    if expected is not None and hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError(f"download SHA256 rejected: {url}")
    output.write_bytes(raw)
    return raw


def stanzas(text: str) -> list[dict[str, str]]:
    records = []
    for paragraph in text.strip().split("\n\n"):
        record: dict[str, str] = {}
        previous = ""
        for line in paragraph.splitlines():
            if line.startswith((" ", "\t")) and previous:
                record[previous] += "\n" + line
            else:
                key, separator, value = line.partition(":")
                if not separator or key in record:
                    raise ValueError("invalid or duplicate Debian metadata field")
                previous = key
                record[key] = value.lstrip()
        records.append(record)
    return records


def build(args: argparse.Namespace, output: Path) -> dict:
    lock_path = Path(__file__).with_name("legacy-debian-lock.json")
    lock = json.loads(lock_path.read_text())
    archive = lock["archive"]
    metadata = output / "metadata"
    metadata.mkdir()
    release = acquire(archive + "/dists/squeeze/Release", metadata / "Release")
    if hashlib.sha256(release).hexdigest() != lock["release_sha256"]:
        raise ValueError("historical Release differs from the pinned archive metadata")
    acquire(archive + "/dists/squeeze/Release.gpg", metadata / "Release.gpg")
    keyring = args.keyring.resolve(strict=True)
    gpgv = executable(args.gpgv)
    verified = subprocess.run([str(gpgv), "--status-fd", "1", "--keyring", str(keyring),
                               str(metadata / "Release.gpg"), str(metadata / "Release")],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False)
    (metadata / "signature-status.log").write_bytes(verified.stdout + verified.stderr)
    fingerprints = re.findall(rb"^\[GNUPG:\] VALIDSIG ([0-9A-F]+) ", verified.stdout, re.MULTILINE)
    accepted = sorted({item.decode("ascii") for item in fingerprints} & SIGNERS)
    if verified.returncode != 0 or not accepted:
        raise ValueError("historical Debian Release signature did not validate against the expected archive signers")
    release_record = stanzas(release.decode("utf-8"))[0]
    if release_record.get("Codename") != "squeeze" or release_record.get("Origin") != "Debian":
        raise ValueError("signed Release is not Debian Squeeze")
    index_name = "main/binary-armel/Packages.gz"
    matches = [line.split() for line in release_record["SHA256"].splitlines() if line.split() and line.split()[-1] == index_name]
    if len(matches) != 1 or len(matches[0]) != 3:
        raise ValueError("signed Release lacks a unique SHA256 for the armel package index")
    digest, size, _ = matches[0]
    if digest != lock["package_index_sha256"]:
        raise ValueError("signed package-index digest differs from the frozen lock")
    index = acquire(archive + "/dists/squeeze/" + index_name, metadata / "Packages.gz", digest, int(size))
    records = stanzas(gzip.decompress(index).decode("utf-8"))
    selected = []
    for name in PACKAGES:
        candidates = [item for item in records if item.get("Package") == name and item.get("Architecture") == "armel"]
        if len(candidates) != 1:
            raise ValueError(f"package index does not uniquely identify armel {name}")
        entry = candidates[0]
        if name in ("libc6", "libc6-dev") and entry.get("Version") != "2.11.3-4":
            raise ValueError(f"unexpected historical libc development ABI: {entry.get('Version')}")
        path = PurePosixPath(entry["Filename"])
        if path.is_absolute() or ".." in path.parts or not str(path).startswith("pool/"):
            raise ValueError("package path escaped the Debian archive")
        if re.fullmatch(r"[0-9a-f]{64}", entry["SHA256"]) is None:
            raise ValueError("package index lacks a SHA256 digest")
        pinned = next(item for item in lock["packages"] if item["name"] == name)
        if (entry["Version"], entry["Filename"], entry["SHA256"]) != (
                pinned["version"], pinned["filename"], pinned["sha256"]):
            raise ValueError(f"signed {name} package differs from the frozen lock")
        selected.append(entry)
    packages = output / "packages"
    packages.mkdir()
    sysroot = output / "sysroot"
    sysroot.mkdir()
    dpkg = executable(args.dpkg_deb)
    for entry in selected:
        target = packages / PurePosixPath(entry["Filename"]).name
        acquire(archive + "/" + entry["Filename"], target, entry["SHA256"], int(entry["Size"]))
        subprocess.run([str(dpkg), "--extract", str(target), str(sysroot)], timeout=60, check=True)
    # Native armel packages contain /lib symlinks. Resolve those within the
    # separate sysroot so neither compiler nor linker follows a host /lib.
    normalize_links(sysroot)
    for name in ("crt1.o", "Scrt1.o", "crti.o", "crtn.o", "libc_nonshared.a", "libc.so"):
        if not (sysroot / "usr/lib" / name).is_file():
            raise ValueError(f"public development package lacks {name}")
    (output / "sysroot-manifest.json").write_text(json.dumps(inventory(sysroot), indent=2, sort_keys=True) + "\n")
    return {
        "schema": "aula-public-legacy-sysroot-v1", "status": "passed",
        "suite": "squeeze", "architecture": "armel", "glibc": "2.11.3",
        "release_date": release_record.get("Date"), "release_sha256": hashlib.sha256(release).hexdigest(),
        "package_index_sha256": hashlib.sha256(index).hexdigest(), "verified_signers": accepted,
        "keyring": {"path": str(keyring), "sha256": sha256(keyring)},
        "gpgv": {"path": str(gpgv), "sha256": sha256(gpgv)},
        "packages": [{key: entry[key] for key in ("Package", "Version", "Architecture", "Filename", "Size", "SHA256")}
                     for entry in selected],
        "sysroot": str(sysroot),
        "sysroot_manifest_sha256": sha256(output / "sysroot-manifest.json"),
        "acquirer_sha256": sha256(Path(__file__)),
        "input_lock_sha256": sha256(lock_path),
        "limits": ["Historical signatures and package integrity only; no current security or freshness claim",
                   "Archive signing keys are now expired; the retained Release was signed in 2015",
                   "Development ABI compatibility requires separate ELF and execution checks",
                   "No firmware inputs, package installation or maintainer-script execution"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keyring", type=Path, required=True)
    parser.add_argument("--gpgv", default="gpgv")
    parser.add_argument("--dpkg-deb", default="dpkg-deb")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = create_output(args.output)
    try:
        report = build(args, output)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report = {"schema": "aula-public-legacy-sysroot-v1", "status": "failed", "error": str(error)}
    (output / "acquisition.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"status": report["status"], "report": str(output / "acquisition.json")}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
