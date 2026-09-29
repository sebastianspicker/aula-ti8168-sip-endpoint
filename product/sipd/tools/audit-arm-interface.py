#!/usr/bin/env python3
"""Produce a static, evidence-scoped Linux 2.6.37 compatibility report.

The report combines a built candidate's dynamic imports and ARM SVC sites with
an explicit scan of the reviewed C source. It deliberately does not execute
ARM code or infer a syscall number from a register-held value at an SVC site.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
EVIDENCE_ROOT = PROJECT.parents[1] / "evidence"
SOURCE_SUFFIXES = {".c", ".h"}


def die(message: str) -> None:
    raise SystemExit(f"audit-arm-interface: {message}")


def text(command: list[str]) -> str:
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as error:
        die(str(error))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def regular_file(path: Path, label: str) -> Path:
    if not path.is_file() or path.is_symlink() or path.resolve(strict=True) != path.absolute():
        die(f"{label} must be a regular non-symlink file")
    return path.resolve(strict=True)


def new_output(path: Path) -> Path:
    if path.exists() or path.is_symlink() or not path.parent.is_dir() or path.parent.is_symlink():
        die("output must be new beneath an existing non-symlink directory")
    parent = path.parent.resolve(strict=True)
    output = parent / path.name
    if path.parent.absolute() != parent:
        die("output parent symlink alias is forbidden")
    try:
        output.relative_to(EVIDENCE_ROOT.resolve(strict=False))
        die("refusing to write inside protected evidence tree")
    except ValueError:
        return output


def load_string_list(path: Path, label: str) -> list[str]:
    try:
        value = json.loads(regular_file(path, label).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        die(f"invalid {label}: {error}")
    if not isinstance(value, list) or not value or not all(isinstance(item, str) and item for item in value):
        die(f"{label} must be a non-empty JSON string list")
    return sorted(set(value))


def load_policy(path: Path) -> dict[str, object]:
    try:
        policy = json.loads(regular_file(path, "policy").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        die(f"invalid policy: {error}")
    if not isinstance(policy, dict) or policy.get("format") != "aula-sipd-linux-2.6.37-interface-policy-v1" or policy.get("kernel_ceiling") != "2.6.37":
        die("policy format or kernel ceiling is invalid")
    if not isinstance(policy.get("glibc_max"), str) or not re.fullmatch(r"\d+\.\d+", policy["glibc_max"]):
        die("policy GLIBC ceiling is invalid")
    for key in ("forbidden_source_interfaces", "direct_syscall_markers"):
        values = policy.get(key)
        if not isinstance(values, list) or not values or not all(isinstance(value, str) and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", value) for value in values):
            die(f"policy {key} is invalid")
    return policy


def version_at_most(value: str, ceiling: str) -> bool:
    return tuple(int(part) for part in value.split(".")) <= tuple(int(part) for part in ceiling.split("."))


def imported_symbols(binary: Path, readelf: str) -> tuple[list[str], list[str]]:
    names: list[str] = []
    glibc_versions: list[str] = []
    for line in text([readelf, "--dyn-syms", "-W", str(binary)]).splitlines():
        fields = line.split()
        if len(fields) < 8 or fields[6] != "UND" or fields[4] == "WEAK":
            continue
        name, separator, version = fields[7].partition("@")
        names.append(name)
        version = version.lstrip("@")
        if separator and version.startswith("GLIBC_"):
            glibc_versions.append(version.removeprefix("GLIBC_"))
    return sorted(set(names)), sorted(set(glibc_versions), key=lambda value: tuple(int(part) for part in value.split(".")))


def _code_transition(value: str, index: int) -> tuple[str, str, int]:
    char = value[index]
    following = value[index + 1] if index + 1 < len(value) else ""
    pairs = {"//": "line-comment", "/*": "block-comment"}
    comment = pairs.get(char + following)
    if comment is not None:
        return comment, "  ", 2
    quotes = {'"': "string", "'": "character"}
    if char in quotes:
        return quotes[char], " ", 1
    return "code", char, 1


def _comment_transition(state: str, value: str, index: int) -> tuple[str, str, int]:
    char = value[index]
    following = value[index + 1] if index + 1 < len(value) else ""
    if state == "line-comment" and char == "\n":
        return "code", char, 1
    if state == "block-comment" and char + following == "*/":
        return "code", "  ", 2
    return state, "\n" if char == "\n" else " ", 1


def _literal_transition(state: str, value: str, index: int) -> tuple[str, str, int]:
    char = value[index]
    if char == "\\":
        span = value[index:index + 2]
        return state, "".join("\n" if part == "\n" else " " for part in span), len(span)
    delimiter = '"' if state == "string" else "'"
    if char == delimiter:
        return "code", " ", 1
    return state, "\n" if char == "\n" else " ", 1


def _strip_transition(state: str, value: str, index: int) -> tuple[str, str, int]:
    if state == "code":
        return _code_transition(value, index)
    if state in {"line-comment", "block-comment"}:
        return _comment_transition(state, value, index)
    return _literal_transition(state, value, index)


def strip_comments_and_literals(value: str) -> str:
    """Blank comments and literals while preserving line numbers for findings."""
    result: list[str] = []
    index, state = 0, "code"
    while index < len(value):
        state, replacement, consumed = _strip_transition(state, value, index)
        result.append(replacement)
        index += consumed
    return "".join(result)


def _source_files(source_root: Path) -> list[Path]:
    if not source_root.is_dir() or source_root.is_symlink() or source_root.resolve(strict=True) != source_root.absolute():
        die("source root must be a non-symlink directory")
    files = sorted(path for path in source_root.rglob("*") if path.suffix in SOURCE_SUFFIXES and path.is_file())
    if not files or any(path.is_symlink() for path in files):
        die("source root must contain regular C source files without symlinks")
    return files


def _directive_depth(line: str, depth: int) -> int:
    directive = re.match(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b", line)
    if directive is None:
        return depth
    operation = directive.group(1)
    if operation in {"if", "ifdef", "ifndef"}:
        return depth + 1
    if operation == "endif" and depth:
        return depth - 1
    return depth


def _line_findings(path: Path, line: str, number: int, depth: int,
                   interfaces: list[str], forbidden: set[str]) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for interface in interfaces:
        if re.search(rf"\b{re.escape(interface)}\s*\(", line):
            result.append({"path": str(path.relative_to(PROJECT)), "line": number,
                           "interface": interface,
                           "classification": "post-2.6.37" if interface in forbidden else "direct-syscall-review-required",
                           "conditional": bool(depth)})
    return result


def _source_file_findings(path: Path, interfaces: list[str], forbidden: set[str]) -> tuple[dict[str, object], list[dict[str, object]]]:
    raw = path.read_text(encoding="utf-8")
    inventory = {"path": str(path.relative_to(PROJECT)), "sha256": sha256(path)}
    findings: list[dict[str, object]] = []
    depth = 0
    for line_number, line in enumerate(strip_comments_and_literals(raw).splitlines(), start=1):
        prior_depth = depth
        depth = _directive_depth(line, depth)
        if prior_depth == depth and not re.match(r"^\s*#", line):
            findings.extend(_line_findings(path, line, line_number, depth, interfaces, forbidden))
    return inventory, findings


def source_findings(source_root: Path, policy: dict[str, object]) -> tuple[list[dict[str, object]], list[dict[str, object]], list[dict[str, object]]]:
    files = _source_files(source_root)
    forbidden = set(policy["forbidden_source_interfaces"])
    markers = set(policy["direct_syscall_markers"])
    findings: list[dict[str, object]] = []
    conditional_findings: list[dict[str, object]] = []
    inventory: list[dict[str, object]] = []
    for path in files:
        file_inventory, file_findings = _source_file_findings(path, sorted(forbidden | markers), forbidden)
        inventory.append(file_inventory)
        for finding in file_findings:
            (conditional_findings if finding.pop("conditional") else findings).append(finding)
    return inventory, findings, conditional_findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--allowlist", type=Path, required=True, help="JSON list of approved imported symbols")
    parser.add_argument("--policy", type=Path, default=PROJECT / "tools/linux-2.6.37-interface-policy.json")
    parser.add_argument("--source-root", type=Path, default=PROJECT / "src")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--objdump", default="objdump")
    args = parser.parse_args()
    binary = regular_file(args.binary, "binary")
    output = new_output(args.output)
    allowlist = load_string_list(args.allowlist, "allowlist")
    policy_path = regular_file(args.policy, "policy")
    policy = load_policy(policy_path)
    imports, glibc_versions = imported_symbols(binary, args.readelf)
    unexpected = sorted(set(imports) - set(allowlist))
    newer_glibc = [value for value in glibc_versions if not version_at_most(value, str(policy["glibc_max"]))]
    direct_svc_sites = [line.strip() for line in text([args.objdump, "-d", str(binary)]).splitlines() if re.search(r"\bsvc\b", line, flags=re.IGNORECASE)]
    source_inventory, source_violations, conditional_source_findings = source_findings(args.source_root, policy)
    status = "pass" if not unexpected and not newer_glibc and not direct_svc_sites and not source_violations else "fail"
    report = {
        "format": "aula-sipd-linux-2.6.37-interface-audit-v2",
        "binary_sha256": sha256(binary),
        "kernel_ceiling": "2.6.37",
        "policy_sha256": sha256(policy_path),
        "allowlist_sha256": sha256(regular_file(args.allowlist, "allowlist")),
        "imported_interfaces": imports,
        "unexpected_interfaces": unexpected,
        "glibc_versions": glibc_versions,
        "newer_glibc_versions": newer_glibc,
        "direct_svc_sites": direct_svc_sites,
        "source_inventory": source_inventory,
        "source_interface_violations": source_violations,
        "conditional_source_interface_findings": conditional_source_findings,
        "status": status,
        "scope_limitations": [
            "static audit only; it does not execute ARM code or establish target behavior",
            "SVC instructions are failures because a register-held syscall number cannot be inferred reliably from disassembly alone",
            "conditionally compiled source findings require correlation with the built binary imports; they do not alone establish that an interface reached the candidate",
            "the policy rejects selected post-2.6.37 interfaces and direct syscall wrappers; it is not a complete proof for undocumented kernel-driver ioctls"
        ]
    }
    output.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
