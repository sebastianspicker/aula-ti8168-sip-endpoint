#!/usr/bin/env python3
"""Build against public old armel ABI inputs; execute against a private runtime.

Run this inside a disposable, networkless container with the private runtime
and public sysroot mounted read-only. Firmware is never a compiler/linker input.
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

from run import ROOT, create_output, executable, inventory, sha256
from acquire_legacy_sysroot import reconstruct_verified_sysroot


def validate(args: argparse.Namespace, output: Path) -> dict:
    public = args.public_sysroot.resolve(strict=True)
    runtime = args.runtime.resolve(strict=True)
    if not public.is_dir() or not runtime.is_dir() or public == runtime or public in runtime.parents or runtime in public.parents:
        raise ValueError("public build sysroot and private execution runtime must be separate trees")
    acquisition_path = public.parent / "acquisition.json"
    acquisition = json.loads(acquisition_path.read_text())
    if (acquisition.get("schema"), acquisition.get("status"), acquisition.get("glibc")) != (
            "aula-public-legacy-sysroot-v1", "passed", "2.11.3"):
        raise ValueError("public build sysroot requires a successful authenticated acquisition receipt")
    for entry in acquisition["packages"]:
        package = public.parent / "packages" / Path(entry["Filename"]).name
        if sha256(package) != entry["SHA256"]:
            raise ValueError("public development package changed after authenticated acquisition")
    for library in ("ld-2.12.2.so", "libc-2.12.2.so", "libm-2.12.2.so", "libpthread-2.12.2.so"):
        if not (runtime / "lib" / library).is_file():
            raise ValueError("private runtime is not the prepared glibc 2.12.2 execution oracle")
    public_manifest = inventory(public)
    acquired_manifest = public.parent / "sysroot-manifest.json"
    if (sha256(acquired_manifest) != acquisition["sysroot_manifest_sha256"] or
            json.loads(acquired_manifest.read_text()) != public_manifest):
        raise ValueError("public sysroot differs from its authenticated acquisition receipt")
    verified_public = reconstruct_verified_sysroot(public.parent, output, args.keyring.resolve(strict=True),
                                                   executable("gpgv"), executable("dpkg-deb"))
    if inventory(verified_public) != public_manifest:
        raise ValueError("supplied build sysroot differs from independently verified public package extraction")
    public = verified_public
    prepared_path = runtime.parent / "preparation.json"
    prepared = json.loads(prepared_path.read_text())
    runtime_manifest = runtime.parent / "runtime-manifest.json"
    if (prepared.get("schema"), prepared.get("status"), prepared.get("glibc")) != (
            "aula-private-legacy-runtime-v1", "prepared", "2.12.2") or (
            sha256(runtime_manifest) != prepared["manifest_sha256"] or
            json.loads(runtime_manifest.read_text()) != inventory(runtime)):
        raise ValueError("private execution runtime differs from its preparation receipt")
    if b"stable release version 2.12.2" not in (runtime / "lib/libc-2.12.2.so").read_bytes():
        raise ValueError("private execution libc lacks glibc 2.12.2 version metadata")
    (output / "public-sysroot-manifest.json").write_text(json.dumps(public_manifest, indent=2, sort_keys=True) + "\n")
    compiler = executable(args.cross_prefix + "gcc")
    qemu = executable(args.qemu_arm)
    builtin_result = subprocess.run([str(compiler), "-print-file-name=include"], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15, check=True)
    builtin = Path(builtin_result.stdout.strip()).resolve(strict=True)
    if not builtin.is_dir():
        raise ValueError("compiler builtin include root is unavailable")
    wrappers = output / "toolchain"
    wrappers.mkdir()
    real_tools = {}
    flags = [f"--sysroot={public}", f"-B{public}/usr/lib/", f"-L{public}/usr/lib", f"-L{public}/lib",
             "-nostdinc", "-isystem", str(builtin), "-isystem", str(public / "usr/include")]
    for name in ("gcc", "ar", "ranlib", "readelf", *(('g++',) if args.pjsip_archive else ())):
        tool = executable(args.cross_prefix + name)
        real_tools[name] = {"path": str(tool), "sha256": sha256(tool)}
        wrapper = wrappers / ("arm-linux-gnueabi-" + name)
        if name in ("gcc", "g++"):
            wrapper.write_text("#!/bin/sh\nexec " + " ".join(shlex.quote(item) for item in [str(tool), *flags]) + ' "$@"\n')
            wrapper.chmod(0o755)
        else:
            wrapper.symlink_to(tool)
    # Record compiler/linker selections before compiling maintained product.
    selections = {}
    gcc = wrappers / "arm-linux-gnueabi-gcc"
    for name in ("crt1.o", "Scrt1.o", "crti.o", "crtn.o", "libc.so", "libm.so", "libpthread.so", "libresolv.so"):
        result = subprocess.run([str(gcc), "-print-file-name=" + name], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=15, check=True)
        selected = Path(result.stdout.strip()).resolve(strict=True)
        if not selected.is_relative_to(public):
            raise ValueError(f"compiler/linker selects a non-public-legacy input: {name}: {selected}")
        selections[name] = {"path": str(selected.relative_to(public)), "sha256": sha256(selected)}
    child = output / "execution"
    command = [sys.executable, str(ROOT / "lab/arm-userspace/run.py"),
               "--cross-prefix", str(wrappers / "arm-linux-gnueabi-"),
               "--qemu-arm", str(qemu), "--sysroot", str(runtime),
               "--compiler-sysroot", str(public), "--output", str(child), "--jobs", str(args.jobs)]
    if args.pjsip_archive:
        command.extend(["--pjsip-archive", str(args.pjsip_archive.resolve(strict=True))])
    with (output / "execution.log").open("wb") as log:
        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=1800, check=False)
    if completed.returncode:
        raise ValueError(f"legacy application execution failed ({completed.returncode}); see {output / 'execution.log'}")
    execution = json.loads((child / "report.json").read_text())
    if execution.get("status") != "passed":
        raise ValueError("application runner did not produce a passing execution receipt")
    readelf = executable(args.cross_prefix + "readelf")
    gate = ROOT / "product/sipd/tools/check-arm-abi.sh"
    environment = dict(os.environ, READELF=str(readelf), LANG="C", LC_ALL="C")
    abi = []
    for artifact in execution["artifacts"]:
        binary = child / artifact["path"]
        result = subprocess.run(["/bin/sh", str(gate), "--binary", str(binary),
                                 "--reference", str(runtime / "lib/libc-2.12.2.so"),
                                 "--sysroot", str(runtime)], env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30, check=False)
        logfile = output / (binary.name + "-abi.log")
        logfile.write_bytes(result.stdout)
        if result.returncode or result.stdout.strip() != b"check-arm-abi: OK":
            raise ValueError(f"legacy ABI gate failed for {binary.name}; see {logfile}")
        abi.append({"binary": str(binary.relative_to(output)), "sha256": sha256(binary), "status": "passed"})
    if inventory(public) != public_manifest:
        raise ValueError("public build sysroot changed during validation")
    return {
        "schema": "aula-legacy-arm-compatibility-v1", "status": "passed",
        "public_acquisition_sha256": sha256(acquisition_path),
        "private_runtime_preparation_sha256": sha256(prepared_path),
        "private_runtime_manifest_sha256": sha256(runtime_manifest),
        "legacy_runner_sha256": sha256(Path(__file__)),
        "isolation_attested_by_runner": False,
        "public_manifest_sha256": sha256(output / "public-sysroot-manifest.json"),
        "build_glibc": "2.11.3", "execution_glibc": "2.12.2", "compiler_builtin_headers": str(builtin),
        "real_tools": real_tools, "selected_public_inputs": selections,
        "execution_report_sha256": sha256(child / "report.json"), "abi": abi,
        "limits": ["Private runtime compatibility experiment, not redistribution approval",
                   "User-mode QEMU does not run the original Linux 2.6.37 kernel",
                   "No board peripherals, DSP, capture, physical media or timing validation"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cross-prefix", required=True)
    parser.add_argument("--qemu-arm", required=True)
    parser.add_argument("--public-sysroot", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--keyring", required=True, type=Path)
    parser.add_argument("--pjsip-archive", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    output = create_output(args.output)
    try:
        report = validate(args, output)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        report = {"schema": "aula-legacy-arm-compatibility-v1", "status": "failed", "error": str(error)}
    (output / "legacy-report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"status": report["status"], "report": str(output / "legacy-report.json")}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
