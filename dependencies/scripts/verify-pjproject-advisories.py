#!/usr/bin/env python3
"""Verify the dated pjproject advisory snapshot against a local checkout."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess


ROOT = pathlib.Path(__file__).resolve().parents[2]
SNAPSHOT = ROOT / "dependencies/advisories/pjproject-advisory-snapshot.json"
ADAPTER = ROOT / "product/sipd/src/sip/pjsip.c"
FORBIDDEN_SURFACES = (
    "pj_dns_resolver",
    "pj_xml_parse",
    "pjsip_multipart",
    "pjstun_",
    "pjmedia_",
    "pjsua_",
)


def git(checkout: pathlib.Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(checkout), *arguments],
        check=False,
        text=True,
        capture_output=True,
    )


# These SDP syntax types/parser are used without PJMEDIA transport or negotiation.
ALLOWED_IDENTIFIERS = {"pjmedia_sdp_parse", "pjmedia_sdp_session"}


def excluded_surfaces(adapter: pathlib.Path) -> list[str]:
    """Inspect the adapter and its local include closure after source splitting."""
    pending = [adapter]
    seen: set[pathlib.Path] = set()
    present: set[str] = set()
    while pending:
        source = pending.pop().resolve()
        if source in seen:
            continue
        seen.add(source)
        text = source.read_text(encoding="utf-8")
        for name in re.findall(r'^\s*#\s*include\s*"([^"\n]+)"', text, re.MULTILINE):
            included = source.parent / name
            if included.is_file() or included.suffix == ".inc":
                pending.append(included)
        for identifier in re.findall(r"\b[A-Za-z_][A-Za-z_0-9]*\b", text):
            if identifier not in ALLOWED_IDENTIFIERS and identifier.startswith(FORBIDDEN_SURFACES):
                present.add(identifier)
    return sorted(present)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkout", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    snapshot = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
    pin = snapshot["pin"]
    resolved = git(arguments.checkout, "rev-parse", "HEAD")
    if resolved.returncode != 0 or resolved.stdout.strip() != pin:
        raise SystemExit(f"pjproject checkout is not the reviewed pin: {pin}")
    for finding in snapshot["required_ancestor_commits"]:
        result = git(
            arguments.checkout,
            "merge-base",
            "--is-ancestor",
            finding["commit"],
            pin,
        )
        if result.returncode != 0:
            raise SystemExit(
                f"missing advisory ancestor {finding['advisory']}: {finding['commit']}"
            )
    present = excluded_surfaces(ADAPTER)
    if present:
        raise SystemExit("excluded PJSIP surface is referenced: " + ", ".join(present))
    print(
        "pjproject advisory snapshot verified: "
        f"{len(snapshot['required_ancestor_commits'])} fix ancestors, "
        f"{len(snapshot['no_fix_exclusions'])} no-fix exclusions, "
        f"{len(snapshot['disabled_feature_advisories'])} disabled features"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
