#!/usr/bin/env python3
"""Build and run maintained C tests on emulated ARMv7 using explicit local tools.

This is an application userspace check, not a TI8168 board or old-kernel gate.
No tools or dependencies are acquired and no host execution fallback exists.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
TESTS = (
    "test_control_frame", "test_control_protocol", "test_config_diagnostics",
    "test_config_parse", "test_request_decode", "test_sdp", "test_pjsip_options",
    "check_config_explains_rejection",
)
LIMITS = [
    "No TI8168 peripheral, NAND, DSP, video capture, display or audio hardware validation",
    "User-mode QEMU uses the host kernel; Linux 2.6.37 compatibility is unproven",
    "The supplied runtime does not establish the target glibc 2.12 ABI ceiling",
    "PJSIP, native AAC/AEC and SRTP integrations are disabled in this lane",
    "The daemon fixture call exercises call-state transitions without SIP or media I/O",
]


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def inventory(directory: Path) -> list[dict[str, str]]:
    entries = []
    for path in sorted(directory.rglob("*")):
        relative = path.relative_to(directory).as_posix()
        if path.is_symlink():
            entries.append({"path": relative, "symlink": os.readlink(path)})
        elif path.is_file():
            entries.append({"path": relative, "sha256": sha256(path)})
    return entries


def executable(value: str) -> Path:
    found = shutil.which(value)
    if found is None:
        raise ValueError(f"required executable is unavailable: {value}")
    path = Path(found).resolve(strict=True)
    if not path.is_file():
        raise ValueError(f"required executable is not a regular file: {value}")
    return path


def create_output(value: Path) -> Path:
    """Only create a fresh directory below this checkout's .work tree."""
    requested = Path(os.path.abspath(value))
    allowed = ROOT / ".work"
    if not requested.is_relative_to(allowed) or requested == allowed:
        raise ValueError(f"output must be a fresh directory below {allowed}")
    current = ROOT
    for part in requested.relative_to(ROOT).parts:
        current = current / part
        if current.is_symlink():
            raise ValueError("output ancestors must not be symlinks")
        if current == requested:
            current.mkdir(mode=0o700)
        else:
            current.mkdir(mode=0o700, exist_ok=True)
    return requested


class Run:
    def __init__(self, output: Path) -> None:
        self.output = output
        self.report: dict[str, Any] = {
            "schema": "aula-arm-userspace-v1", "status": "running",
            "started_utc": datetime.now(timezone.utc).isoformat(),
            "cpu": "cortex-a8", "float_abi": "soft", "limits": LIMITS,
            "commands": [], "artifacts": [],
        }
        self.environment = dict(os.environ, LANG="C", LC_ALL="C")
        # CMake must not inherit a host compiler or user-injected compiler flags.
        for variable in ("CC", "CXX", "AR", "RANLIB", "CFLAGS", "CPPFLAGS",
                         "CXXFLAGS", "LDFLAGS", "CMAKE_TOOLCHAIN_FILE",
                         "CMAKE_PROJECT_INCLUDE", "CMAKE_PROJECT_INCLUDE_BEFORE"):
            self.environment.pop(variable, None)

    def command(self, arguments: list[str], *, timeout: int = 120,
                cwd: Path = ROOT, environment: dict[str, str] | None = None) -> str:
        entry: dict[str, Any] = {"argv": arguments}
        self.report["commands"].append(entry)
        log = self.output / f"command-{len(self.report['commands']):02d}.log"
        entry["log"] = log.name
        try:
            result = subprocess.run(arguments, cwd=cwd, env=environment or self.environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=timeout, check=False)
            log.write_bytes(result.stdout)
            entry["exit_code"] = result.returncode
            if result.returncode != 0:
                raise ValueError(f"command failed ({result.returncode}); see {log}")
            return result.stdout.decode("utf-8", errors="replace")
        except subprocess.TimeoutExpired as error:
            log.write_bytes(error.stdout or b"")
            entry["timed_out"] = True
            raise ValueError(f"command timed out; see {log}") from error

    def tool(self, label: str, path: Path, version_arguments: list[str]) -> None:
        self.report.setdefault("tools", {})[label] = {
            "path": str(path), "sha256": sha256(path),
            "version": self.command([str(path), *version_arguments]).strip(),
        }

    def inspect(self, readelf: Path, path: Path) -> None:
        metadata = self.command([str(readelf), "-h", "-A", "-l", "-d", "-V", str(path)])
        require_arm_elf(metadata)
        self.report["artifacts"].append({
            "path": str(path.relative_to(self.output)), "sha256": sha256(path),
            "validated": ["ELF32", "little endian", "ARM", "EABI5", "ARMv7",
                          "soft float calling convention", "/lib/ld-linux.so.3"],
            "readelf_log": self.report["commands"][-1]["log"],
        })

    def finish(self, status: str, error: str | None = None) -> None:
        self.report["status"] = status
        self.report["finished_utc"] = datetime.now(timezone.utc).isoformat()
        if error:
            self.report["error"] = error
        (self.output / "report.json").write_text(
            json.dumps(self.report, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def require_arm_elf(metadata: str) -> None:
    checks = (
        r"Class:\s+ELF32", r"Data:\s+2's complement, little endian",
        r"Machine:\s+ARM", r"Flags:.*Version5 EABI", r"Tag_CPU_arch:\s+v7\s",
        r"Requesting program interpreter: /lib/ld-linux\.so\.3\]",
    )
    if any(re.search(check, metadata) is None for check in checks):
        raise ValueError("candidate does not match ARMv7 EABI5 soft-float ELF contract")
    if "hard-float ABI" in metadata or re.search(r"Tag_ABI_VFP_args:\s+VFP registers", metadata):
        raise ValueError("hard-float candidates are outside the soft-float ARM lane")


def require_ctest_results(path: Path) -> None:
    suite = ET.parse(path).getroot()
    cases = suite.findall("testcase")
    if (suite.tag != "testsuite" or suite.get("tests") != str(len(TESTS)) or
            any(suite.get(key, "0") != "0" for key in ("failures", "disabled", "skipped", "errors")) or
            sorted(case.get("name", "") for case in cases) != sorted(TESTS) or
            any(case.get("status") != "run" or case.find("failure") is not None or
                case.find("skipped") is not None or case.find("error") is not None for case in cases)):
        raise ValueError("CTest did not execute and pass every required ARM test")


def validate(run: Run, args: argparse.Namespace) -> None:
    prefix = args.cross_prefix
    tools = {name: executable(prefix + name) for name in ("gcc", "ar", "ranlib", "readelf")}
    tools.update({"qemu": executable(args.qemu_arm), "cmake": executable(args.cmake),
                  "ctest": executable(args.ctest)})
    for name, path in tools.items():
        run.tool(name, path, ["--version"])
    triple = run.command([str(tools["gcc"]), "-dumpmachine"]).strip()
    if re.fullmatch(r"arm[^\s]*-linux-gnueabi", triple) is None:
        raise ValueError(f"compiler must target soft-float ARM Linux gnueabi, got {triple!r}")
    sysroot = args.sysroot.resolve(strict=True)
    if not sysroot.is_dir() or sysroot == Path("/") or not (sysroot / "lib/ld-linux.so.3").is_file():
        raise ValueError("sysroot must contain lib/ld-linux.so.3 and must not be the host root")
    run.report["compiler_target"] = triple
    run.report["compiler_default_sysroot"] = run.command([str(tools["gcc"]), "-print-sysroot"]).strip()
    run.report["runtime_root"] = str(sysroot)
    inventories = {
        "runtime": inventory(sysroot),
        "source": inventory(ROOT / "product/sipd"),
    }
    for name, entries in inventories.items():
        path = run.output / f"{name}-manifest.json"
        path.write_text(json.dumps(entries, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        run.report[f"{name}_manifest"] = {"path": path.name, "sha256": sha256(path)}
    run.report["runner_sha256"] = sha256(Path(__file__))
    build = run.output / "build"
    emulator = [str(tools["qemu"]), "-cpu", "cortex-a8", "-L", str(sysroot)]
    if any(";" in item for item in emulator):
        raise ValueError("emulator paths cannot contain CMake list separators")
    compiler_sysroot = None
    if args.compiler_sysroot is not None:
        compiler_sysroot = args.compiler_sysroot.resolve(strict=True)
        if not compiler_sysroot.is_dir():
            raise ValueError("compiler sysroot must be a directory")
        run.report["compiler_explicit_sysroot"] = str(compiler_sysroot)
    find_root = compiler_sysroot if compiler_sysroot is not None else sysroot
    configure = [
        str(tools["cmake"]), "-S", str(ROOT / "product/sipd"), "-B", str(build),
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_SYSTEM_NAME=Linux",
        "-DCMAKE_SYSTEM_PROCESSOR=armv7", f"-DCMAKE_C_COMPILER={tools['gcc']}",
        f"-DCMAKE_AR={tools['ar']}", f"-DCMAKE_RANLIB={tools['ranlib']}",
        "-DCMAKE_C_FLAGS=-march=armv7-a -mtune=cortex-a8 -mfloat-abi=soft",
        f"-DCMAKE_FIND_ROOT_PATH={find_root}", "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER",
        "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY", "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY",
        "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY",
        "-DAULA_SIPD_TARGET_PROFILE=aula-arm-eabi5", "-DAULA_SIPD_BUILD_TESTS=ON",
        "-DAULA_SIPD_ENABLE_CROSS_TESTS=ON", "-DCMAKE_CROSSCOMPILING_EMULATOR=" + ";".join(emulator),
        "-DAULA_SIPD_ENABLE_PJSIP=OFF", "-DAULA_SIPD_ENABLE_FAAD2=OFF",
        "-DAULA_SIPD_ENABLE_SPEEXDSP=OFF", "-DAULA_SIPD_ENABLE_SRTP=OFF",
    ]
    if compiler_sysroot is not None:
        configure.append(f"-DCMAKE_SYSROOT={compiler_sysroot}")
    if args.pjsip_archive is not None:
        from pjsip import build_pjsip
        configure.extend(build_pjsip(run, args, tools))
    run.command(configure)
    run.command([str(tools["cmake"]), "--build", str(build), "--parallel", str(args.jobs)], timeout=600)
    tests = json.loads(run.command([str(tools["ctest"]), "--test-dir", str(build), "--show-only=json-v1"]))
    if sorted(item["name"] for item in tests["tests"]) != sorted(TESTS):
        raise ValueError("CTest inventory differs from the eight required maintained checks")
    for test in tests["tests"]:
        if test.get("command", [])[:len(emulator)] != emulator:
            raise ValueError(f"test bypasses the required ARM emulator: {test['name']}")
    for name in ("aula-sipd", *(name for name in TESTS if name.startswith("test_"))):
        run.inspect(tools["readelf"], build / (name if name == "aula-sipd" else f"tests/{name}"))
    run.command([str(tools["ctest"]), "--test-dir", str(build), "--output-on-failure",
                 "--timeout", "30", "--output-junit", str(run.output / "ctest.xml")], timeout=300)
    require_ctest_results(run.output / "ctest.xml")
    run.report["ctest_checks_passed"] = list(TESTS)
    for option, expected in (("--self-test", "self-test: ok"),
                             ("--fixture-call", "fixture-call: ok network=disabled media_io=disabled rx_rendering=false")):
        result = run.command([*emulator, str(build / "aula-sipd"), option], timeout=30)
        if result.strip() != expected:
            raise ValueError(f"unexpected daemon output for {option}")
    run.report["daemon_checks_passed"] = ["--self-test", "--fixture-call"]
    if args.pjsip_archive is not None:
        run.command([sys.executable, str(ROOT / "lab/arm-userspace/sip_smoke.py"),
                     "--qemu-arm", str(tools["qemu"]), "--sysroot", str(sysroot),
                     "--binary", str(build / "aula-sipd"),
                     "--evidence", str(run.output / "sip-evidence.json")], timeout=60)
        run.report["sip_loopback"] = json.loads((run.output / "sip-evidence.json").read_text())
    if any(sha256(Path(tool["path"])) != tool["sha256"] for tool in run.report["tools"].values()):
        raise ValueError("tool executables changed during validation")
    # A changing runtime/source cannot retain a passing provenance receipt.
    if inventory(sysroot) != inventories["runtime"] or inventory(ROOT / "product/sipd") != inventories["source"]:
        raise ValueError("source or runtime inputs changed during validation")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cross-prefix", required=True, help="prefix supplying ARM gcc, ar, ranlib and readelf")
    parser.add_argument("--qemu-arm", required=True, help="explicit local qemu-arm executable")
    parser.add_argument("--sysroot", required=True, type=Path, help="ARM runtime root passed to qemu-arm -L")
    parser.add_argument("--compiler-sysroot", type=Path, help="optional CMAKE_SYSROOT; otherwise GCC's configured sysroot is used")
    parser.add_argument("--pjsip-archive", type=Path, help="optional exact dependency-lock pjproject archive; enables a UDP loopback daemon call")
    parser.add_argument("--output", required=True, type=Path, help="fresh output directory below this checkout's .work")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    try:
        output = create_output(args.output)
    except (OSError, ValueError) as error:
        print(f"arm-userspace: {error}", file=sys.stderr)
        return 2
    run = Run(output)
    try:
        validate(run, args)
    except (OSError, ValueError, KeyError, ET.ParseError) as error:
        run.finish("failed", str(error))
        print(f"arm-userspace: {error}; report: {output / 'report.json'}", file=sys.stderr)
        return 1
    run.finish("passed")
    print(f"ARMv7 userspace checks passed; report: {output / 'report.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
