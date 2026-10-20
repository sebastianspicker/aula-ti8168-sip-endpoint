"""Hermetic CLI mutation/reload check used by ``make emulator-status``."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="aula-emulator-verify-") as directory:
        root = Path(directory)
        state = root / "state.json"

        def command(action: str) -> dict[str, object]:
            result = subprocess.run(
                [
                    sys.executable, "-m", "aula_emulator", action,
                    "--work-root", str(root), "--state-file", str(state),
                ],
                check=True, capture_output=True, text=True,
            )
            value = json.loads(result.stdout)
            if not isinstance(value, dict):
                raise RuntimeError("CLI did not return a snapshot")
            return value

        expected = {
            "kind": "synthetic-local-state-v2", "revision": 1,
            "recording": "inactive", "streaming": "inactive",
        }
        if command("reset") != expected:
            raise RuntimeError("fresh reset did not produce revision 1 idle state")
        expected.update(revision=2, recording="active")
        if command("record-start") != expected or command("status") != expected:
            raise RuntimeError("recording mutation did not survive a CLI reload")
        expected.update(revision=3, streaming="paused")
        if command("stream-pause") != expected or command("status") != expected:
            raise RuntimeError("streaming pause did not survive a CLI reload")
        expected.update(revision=4, recording="inactive", streaming="inactive")
        if command("reset") != expected or command("status") != expected:
            raise RuntimeError("reset did not retain the monotonic revision")
        print(json.dumps(expected, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
