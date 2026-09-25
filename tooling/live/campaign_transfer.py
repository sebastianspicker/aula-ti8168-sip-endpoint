"""Descriptor-anchored campaign transfer and payload verification."""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import re
import stat
import subprocess
import tarfile
from pathlib import Path
from typing import Any

from campaign_evidence import _fsync_directory, _new_path, _private_root
from campaign_transport import _run, _ssh_options
from campaign_types import CampaignError, LIVE_TARGET, PAYLOAD, ROOT, SAFE_ENV
from session import LiveSession
from gates import GateError, package_digest

def _transfer_member_name(path: Path) -> str:
    try:
        relative = path.relative_to(ROOT).as_posix()
    except ValueError as error:
        raise CampaignError("transfer source escaped the repository") from error
    if not relative or not re.fullmatch(r"[A-Za-z0-9._/+:-]+", relative):
        raise CampaignError("transfer source contains an unsafe path")
    return relative


class _TransferCollector:
    def __init__(self) -> None:
        self.members: list[tuple[str, int | None, os.stat_result]] = []
        self.seen: set[str] = set()
        self.retained_files: list[int] = []
        self.directory_flags = (
            os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_CLOEXEC", 0)
            | getattr(os, "O_NOFOLLOW", 0)
        )
        self.file_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)

    def _collect_directory(
        self, parent_descriptor: int, leaf: str, name: str, before: os.stat_result,
    ) -> None:
        try:
            child_descriptor = os.open(leaf, self.directory_flags, dir_fd=parent_descriptor)
        except OSError as error:
            raise CampaignError("transfer source directory could not be opened without following links") from error
        try:
            opened = os.fstat(child_descriptor)
            if not stat.S_ISDIR(opened.st_mode) or (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
                raise CampaignError("transfer source directory changed during anchored traversal")
            self.members.append((name, None, opened))
            for entry_name in sorted(os.listdir(child_descriptor)):
                if entry_name in {".", ".."} or "/" in entry_name:
                    raise CampaignError("transfer source contains an unsafe path")
                self.collect_entry(child_descriptor, entry_name, f"{name}/{entry_name}")
        finally:
            os.close(child_descriptor)

    def _collect_file(
        self, parent_descriptor: int, leaf: str, name: str, before: os.stat_result,
    ) -> None:
        try:
            child_descriptor = os.open(leaf, self.file_flags, dir_fd=parent_descriptor)
        except OSError as error:
            raise CampaignError("transfer source file could not be opened without following links") from error
        opened = os.fstat(child_descriptor)
        if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino):
            os.close(child_descriptor)
            raise CampaignError("transfer source file changed during anchored traversal")
        self.retained_files.append(child_descriptor)
        self.members.append((name, child_descriptor, opened))

    def collect_entry(self, parent_descriptor: int, leaf: str, name: str) -> None:
        if name in self.seen:
            raise CampaignError("transfer source contains duplicate paths")
        try:
            before = os.stat(leaf, dir_fd=parent_descriptor, follow_symlinks=False)
        except OSError as error:
            raise CampaignError("transfer source disappeared during anchored traversal") from error
        if stat.S_ISLNK(before.st_mode) or not (stat.S_ISDIR(before.st_mode) or stat.S_ISREG(before.st_mode)):
            raise CampaignError("transfer source contains a symlink or special file")
        self.seen.add(name)
        if stat.S_ISDIR(before.st_mode):
            self._collect_directory(parent_descriptor, leaf, name, before)
        else:
            self._collect_file(parent_descriptor, leaf, name, before)

    def collect_source(self, root_descriptor: int, source: Path) -> None:
        name = _transfer_member_name(source)
        relative = source.relative_to(ROOT)
        components = relative.parts
        if not components or any(component in {"", ".", ".."} for component in components):
            raise CampaignError("transfer source contains an unsafe path")
        parent_descriptor = os.dup(root_descriptor)
        try:
            for component in components[:-1]:
                try:
                    next_descriptor = os.open(component, self.directory_flags, dir_fd=parent_descriptor)
                except OSError as error:
                    raise CampaignError("transfer source escaped the descriptor-anchored repository") from error
                os.close(parent_descriptor)
                parent_descriptor = next_descriptor
            self.collect_entry(parent_descriptor, components[-1], name)
        finally:
            os.close(parent_descriptor)

    def collect(self, paths: list[Path]) -> None:
        root_descriptor = os.open(ROOT, self.directory_flags)
        try:
            if not stat.S_ISDIR(os.fstat(root_descriptor).st_mode):
                raise CampaignError("repository root is not an anchored directory")
            for source in paths:
                self.collect_source(root_descriptor, source)
        finally:
            os.close(root_descriptor)


def _write_transfer_archive(descriptor: int, collector: _TransferCollector) -> bytes:
    rows: list[str] = []
    with os.fdopen(descriptor, "wb", closefd=False) as raw, tarfile.open(fileobj=raw, mode="w|") as output:
        for name, source_descriptor, expected_info in sorted(collector.members):
            member = tarfile.TarInfo(name + ("/" if stat.S_ISDIR(expected_info.st_mode) else ""))
            member.uid = member.gid = 0
            member.uname = member.gname = "root"
            member.mtime = 0
            if stat.S_ISDIR(expected_info.st_mode):
                member.type = tarfile.DIRTYPE
                member.mode = 0o755
                member.size = 0
                output.addfile(member)
                rows.append(f"D\t0755\t-\t{name}")
                continue
            if source_descriptor is None:
                raise CampaignError("anchored transfer file descriptor is absent")
            try:
                opened = os.fstat(source_descriptor)
                if not stat.S_ISREG(opened.st_mode) or (opened.st_dev, opened.st_ino) != (expected_info.st_dev, expected_info.st_ino):
                    raise CampaignError("transfer source changed before its descriptor could be bound")
                member.mode = 0o555 if opened.st_mode & 0o111 else 0o444
                member.size = opened.st_size
                digest = hashlib.sha256()

                class _DigestReader:
                    def read(self, count: int = -1) -> bytes:
                        block = os.read(source_descriptor, count if count >= 0 else 1024 * 1024)
                        digest.update(block)
                        return block

                output.addfile(member, _DigestReader())
                finished = os.fstat(source_descriptor)
                if (opened.st_dev, opened.st_ino, opened.st_size, opened.st_mtime_ns) != (
                    finished.st_dev, finished.st_ino, finished.st_size, finished.st_mtime_ns
                ):
                    raise CampaignError("transfer source changed while its snapshot was read")
                rows.append(f"F\t{member.mode:04o}\t{digest.hexdigest()}\t{name}")
            finally:
                os.close(source_descriptor)
                collector.retained_files.remove(source_descriptor)
    return ("\n".join(rows) + "\n").encode("ascii")


def _snapshot_transfer_archive(session: LiveSession, paths: list[Path]) -> tuple[Path, bytes]:
    """Snapshot descriptor-bound regular files into a deterministic transfer archive."""
    archive, descriptor = _new_path(_private_root(session), "transfer", ".tar")
    collector = _TransferCollector()
    try:
        collector.collect(paths)
        manifest = _write_transfer_archive(descriptor, collector)
        os.fsync(descriptor)
        _fsync_directory(archive.parent)
        return archive, manifest
    except BaseException:
        with contextlib.suppress(OSError):
            os.close(descriptor)
        with contextlib.suppress(FileNotFoundError):
            archive.unlink()
        raise
    finally:
        for source_descriptor in collector.retained_files:
            with contextlib.suppress(OSError):
                os.close(source_descriptor)
        with contextlib.suppress(OSError):
            os.close(descriptor)


def _tar_transfer(
    session: LiveSession, known: Path, control: Path, paths: list[Path], remote: str,
    *, expected_payload_digest: str | None = None,
) -> None:
    if not re.fullmatch(r"/run/\.ls200-live-[A-Za-z0-9._-]+(?:-[ab])?", remote):
        raise CampaignError("remote transfer path is unsafe")
    if expected_payload_digest is not None and not re.fullmatch(r"[0-9a-f]{64}", expected_payload_digest):
        raise CampaignError("expected payload digest is unsafe")
    archive, manifest = _snapshot_transfer_archive(session, paths)
    target = f"{session.ssh_user}@{session.target_ipv4}"
    expected_check = ""
    if expected_payload_digest is not None:
        expected_check = (
            f"[ \"$(sha256sum {remote}/.work/dist/ls200-live/runtime/payload-manifest.tsv | awk '{{print $1}}')\" = {expected_payload_digest} ]; "
        )
    tab = "\t"
    command = (
        "set -eu; umask 077; [ -d /run ] && [ ! -L /run ]; "
        "[ \"$(ls -nd /run | awk '{print $3}')\" = 0 ]; "
        "! find /run -prune \\( -perm -020 -o -perm -002 \\) | grep -q .; "
        f"[ ! -e {remote} ] && [ ! -L {remote} ]; mkdir {remote}; chmod 0700 {remote}; "
        f"trap 'rm -rf {remote}' 0 1 2 15; IFS= read -r manifest_bytes; "
        "case $manifest_bytes in ''|*[!0-9]*) exit 1 ;; esac; "
        f"dd bs=1 count=\"$manifest_bytes\" of={remote}/.manifest.tsv; "
        f"cat > {remote}/.archive.tar; chmod 0600 {remote}/.manifest.tsv {remote}/.archive.tar; "
        f"expected=$(cut -f4 {remote}/.manifest.tsv | LC_ALL=C sort); actual=$(tar -tf {remote}/.archive.tar | sed 's:/$::' | LC_ALL=C sort); "
        "[ \"$expected\" = \"$actual\" ]; "
        f"while IFS='{tab}' read -r kind mode digest name; do "
        "case $kind:$mode:$digest:$name in D:0755:-:*|F:0444:[0-9a-f][0-9a-f]*:*|F:0555:[0-9a-f][0-9a-f]*:*) ;; *) exit 1 ;; esac; "
        "[ \"$kind\" = D ] || [ \"${#digest}\" -eq 64 ]; "
        "case $name in ''|/*|*'..'*|*[!A-Za-z0-9._/+:-]*) exit 1 ;; esac; "
        f"if [ \"$kind\" = F ]; then actual_digest=$(tar -xOf {remote}/.archive.tar \"$name\" | sha256sum | awk '{{print $1}}'); [ \"$actual_digest\" = \"$digest\" ]; fi; "
        f"done < {remote}/.manifest.tsv; "
        f"types=$(tar -tvf {remote}/.archive.tar | awk '{{type=substr($0,1,1); name=$NF; sub(/\\/$/,\"\",name); if(type==\"d\") print \"D\\t0755\\t\" name; else if(type==\"-\") print \"F\\t\" ((substr($0,2,3) ~ /x/) ? \"0555\" : \"0444\") \"\\t\" name; else exit 1}}' | LC_ALL=C sort); "
        f"expected_types=$(awk -F'{tab}' '{{print $1 \"\\t\" $2 \"\\t\" $4}}' {remote}/.manifest.tsv | LC_ALL=C sort); [ \"$types\" = \"$expected_types\" ]; "
        f"tar -xf {remote}/.archive.tar -C {remote}; {expected_check}trap - 0 1 2 15;"
    )
    process: subprocess.Popen[bytes] | None = None
    try:
        process = subprocess.Popen(
            ["/usr/bin/ssh", *_ssh_options(session, known, control), "-o", "BatchMode=yes", target, command],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=SAFE_ENV,
        )
        assert process.stdin is not None
        process.stdin.write(str(len(manifest)).encode("ascii") + b"\n")
        process.stdin.write(manifest)
        with archive.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                process.stdin.write(block)
        process.stdin.close()
        if process.wait(timeout=300.0) != 0:
            raise CampaignError("strict-host-key transfer or pre-extraction archive validation failed")
    except (OSError, subprocess.TimeoutExpired) as error:
        if process is not None:
            with contextlib.suppress(OSError):
                process.kill()
            with contextlib.suppress(OSError):
                process.wait(timeout=5.0)
        raise CampaignError("strict-host-key transfer failed without retained output") from error
    finally:
        with contextlib.suppress(FileNotFoundError):
            archive.unlink()


def _verified_payload_digest() -> str:
    try:
        digest = package_digest(PAYLOAD)
    except (OSError, ValueError, GateError) as error:
        raise CampaignError("live package digest receipt is absent or unsafe") from error
    _run([
        "/bin/sh", str(LIVE_TARGET / "verify-payload.sh"),
        "--payload", str(PAYLOAD), "--manifest-sha256", digest,
    ], timeout=120.0)
    return digest


def _payload_kib() -> int:
    total = 0
    for entry in PAYLOAD.rglob("*"):
        info = entry.lstat()
        if stat.S_ISREG(info.st_mode):
            total += info.st_size
    return (total + 1023) // 1024
