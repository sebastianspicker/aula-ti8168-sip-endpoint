"""Helpers that run one function of a TI8168 shell script against injected temp paths."""
import subprocess
from pathlib import Path

TI8168 = Path(__file__).resolve().parents[1] / "targets/ti8168"
PRELUDE = "set -eu\ndie() { printf '%s\\n' \"$*\" >&2; exit 1; }\n"


def function_text(script, name):
    text = (TI8168 / script).read_text()
    start = text.index(f"\n{name}() {{") + 1
    line_end = text.index("\n", start)
    if text[start:line_end].rstrip().endswith("}"):
        return text[start:line_end + 1]
    return text[start:text.index("\n}\n", start) + 3]


# Real device paths: a fragment that still names one was not redirected to the temp dir.
FORBIDDEN = ("/etc/passwd", "/etc/group", "/var/lib/cbox")


def run_sh(path, body, timeout=60):
    leaked = [name for name in FORBIDDEN if name in body]
    if leaked:
        raise AssertionError(f"fragment still names real paths {leaked}; refusing to run it")
    path.write_text(body)
    return subprocess.run(["sh", str(path)], capture_output=True, text=True, timeout=timeout, check=False)
