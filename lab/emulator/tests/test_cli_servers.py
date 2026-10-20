"""Exercise the installed-style server entry points with inherited sockets."""

from __future__ import annotations

import json
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


class ServerCliTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.state = self.root / "activity.json"

    def arguments(self, action: str, *options: str) -> list[str]:
        return [
            sys.executable, "-m", "aula_emulator", action,
            "--work-root", str(self.root), "--state-file", str(self.state), *options,
        ]

    def exchange(self, action: str, request: bytes, *options: str) -> bytes:
        client, server = socket.socketpair()
        with client, server:
            client.settimeout(5)
            process = subprocess.Popen(
                self.arguments(action, "--fd", str(server.fileno()), *options),
                pass_fds=(server.fileno(),),
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            try:
                server.close()
                client.sendall(request)
                chunks = bytearray()
                while data := client.recv(65_552):
                    chunks.extend(data)
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stderr.decode())
                self.assertEqual(stdout, b"")
                return bytes(chunks)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate()

    def test_device_server_cli_projects_another_cli_mutation(self) -> None:
        subprocess.run(self.arguments("record-pause"), check=True, capture_output=True)
        request = b'{"revision":1,"operation":"status"}'
        response = self.exchange("serve-device", struct.pack("!I", len(request)) + request)
        self.assertEqual(struct.unpack("!I", response[:4])[0], len(response) - 4)
        self.assertEqual(json.loads(response[4:]), {
            "revision": 1, "recording": "paused", "streaming": "inactive",
        })

    def test_lsz1_cli_status_and_fault(self) -> None:
        request = struct.pack("!4sBBHII", b"LSZ1", 1, 1, 0, 23, 2) + b"{}"
        response = self.exchange("serve", request)
        magic, version, opcode, flags, request_id, size = struct.unpack("!4sBBHII", response[:16])
        self.assertEqual((magic, version, opcode, flags, request_id), (b"LSZ1", 1, 1, 1, 23))
        self.assertEqual(size, len(response) - 16)
        self.assertEqual(json.loads(response[16:])["call_state"], "idle")
        oversized = self.exchange("serve", request, "--fault", "oversized")
        self.assertGreater(len(oversized) - 16, 2048)

    def test_server_argument_errors_are_clean(self) -> None:
        for action, options in (
            ("serve", ()), ("serve-device", ("--fd", "-1")),
            ("serve", ("--fd", "999999")),
            ("status", ("--fd", "1")),
            ("status", ("--fault", "oversized")),
        ):
            with self.subTest(action=action, options=options):
                result = subprocess.run(self.arguments(action, *options), capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn(b"Traceback", result.stderr)
                self.assertEqual(result.stdout, b"")


if __name__ == "__main__":
    unittest.main()
