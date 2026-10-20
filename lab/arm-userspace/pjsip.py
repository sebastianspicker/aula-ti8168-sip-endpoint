"""Optional locked-source PJSIP build for the ARM UDP loopback check."""
from __future__ import annotations

import json
import hashlib
import io
import tarfile
from pathlib import Path
from typing import Any


def build_pjsip(run: Any, args: Any, tools: dict[str, Path]) -> list[str]:
    # Importing run.py as a module also makes its provenance helpers available
    # when the launcher itself was executed as __main__.
    from run import ROOT, executable, sha256

    archive = args.pjsip_archive.resolve(strict=True)
    lock = ROOT / "dependencies/dependency-lock.json"
    package = next(item for item in json.loads(lock.read_text())["packages"] if item["name"] == "pjproject")
    if not archive.is_file():
        raise ValueError("PJSIP archive must be a regular local source file")
    with archive.open("rb") as stream:
        archive_bytes = stream.read(128 * 1024 * 1024 + 1)
    archive_digest = hashlib.sha256(archive_bytes).hexdigest()
    if len(archive_bytes) > 128 * 1024 * 1024 or archive_digest != package["archive_sha256"]:
        raise ValueError("PJSIP archive does not match the locked source digest")
    commit = package["version"].removeprefix("git-")
    dependencies = run.output / "dependencies"
    dependencies.mkdir()
    source = dependencies / f"pjproject-{commit}"
    # Extract precisely the immutable bytes whose digest was checked, avoiding
    # a path replacement race between verification and tarfile opening.
    with tarfile.open(fileobj=io.BytesIO(archive_bytes), mode="r:gz") as stream:
        for member in stream.getmembers():
            destination = Path(member.name)
            if (destination.is_absolute() or ".." in destination.parts or
                    not destination.parts or destination.parts[0] != source.name or
                    not (member.isfile() or member.isdir())):
                raise ValueError("locked PJSIP archive contains an unsupported member")
        stream.extractall(dependencies)  # All member paths and types checked above.
    prepare = ROOT / "dependencies/scripts/prepare-pjproject-signaling.sh"
    patch = ROOT / "dependencies/patches/pjproject-signaling-only.patch"
    run.report["pjsip"] = {"source": package, "archive_sha256": archive_digest,
                            "lock_sha256": sha256(lock), "patch_sha256": sha256(patch),
                            "prepare_sha256": sha256(prepare), "tls": False}
    run.report["limits"] = [item for item in run.report["limits"] if not item.startswith("PJSIP,")]
    run.report["limits"].extend([
        "Native AAC/AEC and SRTP integrations are disabled",
        "PJSIP UDP loopback only: no TLS, provider interoperability or media I/O validation",
    ])
    run.command(["/bin/sh", str(prepare), str(source)])
    # The owned adapter uses strict POSIX/C99 visibility. pj_bzero must use
    # standard memset rather than the legacy bzero declaration hidden by it.
    site = source / "pjlib/include/pj/config_site.h"
    site.write_text("#ifdef PJ_HAS_BZERO\n#undef PJ_HAS_BZERO\n#endif\n#define PJ_HAS_BZERO 0\n", encoding="ascii")
    run.report["pjsip"]["config_site_sha256"] = sha256(site)
    # Upstream configure probes a C++ compiler even though the reviewed patch
    # excludes the unused C++ object from these signaling-only archives.
    cxx = executable(args.cross_prefix + "g++")
    run.tool("g++", cxx, ["--version"])
    environment = dict(run.environment, CC=str(tools["gcc"]), AR=str(tools["ar"]),
                       RANLIB=str(tools["ranlib"]), CXX=str(cxx),
                       CFLAGS="-O2 -march=armv7-a -mtune=cortex-a8 -mfloat-abi=soft")
    if args.compiler_sysroot is not None:
        environment["CFLAGS"] += f" --sysroot={args.compiler_sysroot.resolve()}"
        environment["LDFLAGS"] = f"--sysroot={args.compiler_sysroot.resolve()}"
    environment["CXXFLAGS"] = environment["CFLAGS"]
    run.command([
        str(source / "configure"), "--host=arm-linux-gnueabi", "--disable-ssl",
        "--disable-pjsua2", "--disable-upnp", "--disable-libuuid",
        "--disable-resample", "--disable-sound", "--disable-video", "--disable-speex-aec",
        "--disable-g711-codec", "--disable-l16-codec", "--disable-gsm-codec",
        "--disable-g722-codec", "--disable-g7221-codec", "--disable-speex-codec",
        "--disable-ilbc-codec", "--disable-sdl", "--disable-ffmpeg", "--disable-v4l2",
        "--disable-openh264", "--disable-vpx", "--disable-android-mediacodec",
        "--disable-darwin-ssl", "--disable-opencore-amr", "--disable-silk",
        "--disable-opus", "--disable-bcg729", "--disable-lyra", "--disable-libsrtp",
        "--disable-libyuv", "--disable-libwebrtc",
    ], cwd=source, environment=environment)
    make = executable("make")
    run.tool("make", make, ["--version"])
    for directory, targets in (("pjlib", ["lib"]), ("pjlib-util", ["lib"]),
                               ("pjmedia", ["pjmedia"]), ("pjsip", ["pjsip", "pjsip-ua", "pjsip-simple"])):
        run.command([str(make), "-C", str(source / directory / "build"),
                     f"-j{args.jobs}", *targets], cwd=source, environment=environment, timeout=600)

    pjlib = list((source / "pjlib/lib").glob("libpj-*.a"))
    if len(pjlib) != 1:
        raise ValueError("PJSIP build lacks an unambiguous libpj archive")
    triple = pjlib[0].name.removeprefix("libpj-").removesuffix(".a")

    def library(directory: str, name: str) -> Path:
        candidate = source / directory / "lib" / f"lib{name}-{triple}.a"
        if not candidate.is_file() or candidate.is_symlink():
            raise ValueError(f"PJSIP build lacks a regular lib{name} archive")
        return candidate

    pjmedia = library("pjmedia", "pjmedia")
    libraries = [library("pjsip", "pjsip-simple"), library("pjsip", "pjsip"), pjmedia,
                 library("pjlib-util", "pjlib-util"), library("pjlib", "pj")]
    run.report["pjsip"]["libraries"] = [{"path": str(path.relative_to(run.output)), "sha256": sha256(path)}
                                          for path in [library("pjsip", "pjsip-ua"), *libraries]]
    return ["-DAULA_SIPD_ENABLE_PJSIP=ON", f"-DAULA_SIPD_PJSIP_INCLUDE_DIR={source / 'pjsip/include'}",
            f"-DAULA_SIPD_PJSIP_UA_LIBRARY={library('pjsip', 'pjsip-ua')}",
            f"-DAULA_SIPD_PJMEDIA_LIBRARY={pjmedia}",
            "-DAULA_SIPD_PJSIP_LIBRARIES=" + ";".join(str(path) for path in libraries)]
