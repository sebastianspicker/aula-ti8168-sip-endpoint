#!/usr/bin/env python3
"""Combine independently generated ARM gate reports into one status report."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
EVIDENCE_ROOT = PROJECT.parents[1] / "evidence/firmware-analysis/extracted/rootfs/401900477/rootfs"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--abi-report", type=Path, required=True)
    parser.add_argument("--interface-audit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.parent.resolve(strict=True) / args.output.name
    if args.output.exists() or args.output.is_symlink():
        raise SystemExit("generate-arm-report: output must be new")
    try:
        output.relative_to(EVIDENCE_ROOT.resolve(strict=False))
        raise SystemExit("generate-arm-report: refusing immutable recovered evidence")
    except ValueError:
        pass
    if not args.abi_report.is_file() or args.abi_report.is_symlink():
        raise SystemExit("generate-arm-report: invalid abi input")
    abi_raw = args.abi_report.read_bytes()
    try:
        abi_text = abi_raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise SystemExit(f"generate-arm-report: invalid ABI text: {error}")
    documents = {
        "abi": {
            "format": "ls200-sipd-check-arm-abi-text-v1",
            "status": "pass" if abi_text.strip() == "check-arm-abi: OK" else "fail",
        },
        "abi_sha256": hashlib.sha256(abi_raw).hexdigest(),
    }
    if not args.interface_audit.is_file() or args.interface_audit.is_symlink():
        raise SystemExit("generate-arm-report: invalid interface input")
    raw = args.interface_audit.read_bytes()
    try:
        documents["interface"] = json.loads(raw)
    except json.JSONDecodeError as error:
        raise SystemExit(f"generate-arm-report: invalid interface JSON: {error}")
    documents["interface_sha256"] = hashlib.sha256(raw).hexdigest()
    status = "pass" if documents["interface"].get("status") == "pass" and documents["abi"]["status"] == "pass" else "incomplete-or-fail"
    report = {"format": "ls200-sipd-arm-report-v1", "status": status, "gates": documents, "unproven": ["qemu-arm confined execution", "fixture private-lab call", "hardware media behavior"]}
    output.write_text(json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    return 0 if status == "pass" else 1


if __name__ == "__main__":
    raise SystemExit(main())
