from __future__ import annotations

import errno
import json
import os
import shlex
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import unittest
from pathlib import Path
from typing import Any
from unittest.mock import patch

from aula_emulator.device_server import serve_device
from aula_emulator.runtime import SyntheticRuntime


def packet(request: object) -> bytes:
    body = json.dumps(request, separators=(",", ":")).encode()
    return struct.pack("!I", len(body)) + body


class DeviceServerTests(unittest.TestCase):
    def query(self, data: bytes, runtime: SyntheticRuntime) -> dict[str, Any] | None:
        server, client = socket.socketpair()
        client.settimeout(2)
        errors: list[Exception] = []

        def run() -> None:
            try:
                serve_device(server.fileno(), runtime=runtime)
            except Exception as error:  # noqa: BLE001 - report worker failures in the test thread
                errors.append(error)
            finally:
                server.close()

        thread = threading.Thread(target=run)
        thread.start()
        try:
            client.sendall(data)
            try:
                client.shutdown(socket.SHUT_WR)
            except OSError as error:
                if error.errno not in (errno.ENOTCONN, errno.EPIPE):
                    raise
            header = client.recv(4)
            if not header:
                return None
            self.assertEqual(len(header), 4)
            size = struct.unpack("!I", header)[0]
            response = bytearray()
            while len(response) < size:
                chunk = client.recv(size - len(response))
                self.assertTrue(chunk)
                response.extend(chunk)
            return json.loads(response)
        finally:
            client.close()
            thread.join(3)
            self.assertFalse(thread.is_alive())
            self.assertEqual(errors, [])

    def test_exact_projection_and_all_activity_states(self) -> None:
        runtime = SyntheticRuntime()
        for state in ("inactive", "active", "paused", "unknown"):
            runtime.set_activity("recording", state)
            runtime.set_activity("streaming", state)
            self.assertEqual(self.query(packet({"revision": 1, "operation": "status"}), runtime),
                             {"revision": 1, "recording": state, "streaming": state})

    def test_refreshes_state_written_by_another_runtime(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            provider = SyntheticRuntime(path)
            writer = SyntheticRuntime(path)
            writer.set_activity("recording", "paused")
            writer.set_activity("streaming", "active")
            self.assertEqual(self.query(packet({"revision": 1, "operation": "status"}), provider),
                             {"revision": 1, "recording": "paused", "streaming": "active"})

    def test_rejects_invalid_and_unsupported_requests(self) -> None:
        requests = [
            {}, {"revision": True, "operation": "status"},
            {"revision": 2, "operation": "status"},
            {"revision": 1, "operation": "credentials.status"},
            {"revision": 1, "operation": "jobs.list"},
            {"revision": 1, "operation": "status", "extra": 1},
            [1, "status"],
        ]
        for request in requests:
            with self.subTest(request=request):
                self.assertIsNone(self.query(packet(request), SyntheticRuntime()))
        for data in (b"\0\0\0\0", struct.pack("!I", 4097), b"\0\0", b"\0\0\0\x05{}",
                     packet(None), struct.pack("!I", 2) + b"\xff\xff"):
            with self.subTest(data=data):
                self.assertIsNone(self.query(data, SyntheticRuntime()))
        body = b'{"revision":1,"revision":1,"operation":"status"}'
        self.assertIsNone(self.query(struct.pack("!I", len(body)) + body, SyntheticRuntime()))

    def test_partial_request_has_bounded_deadline(self) -> None:
        server, client = socket.socketpair()
        client.settimeout(1)

        def run() -> None:
            try:
                serve_device(server.fileno())
            finally:
                server.close()

        with patch("aula_emulator.device_server._TIMEOUT", 0.03):
            thread = threading.Thread(target=run)
            thread.start()
            try:
                client.sendall(b"\0\0")
                self.assertEqual(client.recv(4), b"")
            finally:
                client.close()
                thread.join(1)
                self.assertFalse(thread.is_alive())

    def test_rejects_invalid_descriptor_and_ambiguous_runtime(self) -> None:
        with self.assertRaises(ValueError):
            serve_device(-1)
        with self.assertRaises(ValueError):
            serve_device(0, runtime=SyntheticRuntime(), state_file="state.json")
        with tempfile.TemporaryFile() as file, self.assertRaises(OSError):
            serve_device(file.fileno())


class DeviceCContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if not shutil.which("cc") or not shutil.which("pkg-config"):
            raise unittest.SkipTest("C contract requires cc and pkg-config (jansson, openssl)")
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "jansson", "openssl"],
                               text=True, capture_output=True, check=False)
        if flags.returncode:
            raise unittest.SkipTest("C contract requires jansson and openssl development libraries")
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / "device-contract"
        repo = Path(__file__).resolve().parents[3]
        console = repo / "product/console"
        sources = subprocess.check_output(["make", "-s", "-C", str(console),
                                          "print-gateway-sources", "print-device-client-sources"], text=True)
        result = subprocess.run(["cc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-O2", "-pthread",
                        "-I" + str(console / "gateway"), "-I" + str(repo / "product/sipd/include"),
                        str(Path(__file__).with_name("device_contract.c")),
                        *[str(console / source) for source in sources.split()],
                        *shlex.split(flags.stdout), "-o", str(cls.binary)], check=False,
                       capture_output=True, text=True)
        if result.returncode:
            raise AssertionError("device contract compilation failed:\n" + result.stderr)

    def exchange(self, uid: int, runtime: SyntheticRuntime) -> subprocess.CompletedProcess[str]:
        server, client = socket.socketpair()
        thread = threading.Thread(target=serve_device, args=(server.fileno(),), kwargs={"runtime": runtime})
        thread.start()
        try:
            return subprocess.run([str(self.binary), str(client.fileno()), str(uid)],
                                  pass_fds=(client.fileno(),), text=True, capture_output=True, timeout=8, check=False)
        finally:
            client.close()
            thread.join(5)
            server.close()
            self.assertFalse(thread.is_alive())

    def test_real_client_and_gateway_accept_provider_projection(self) -> None:
        runtime = SyntheticRuntime()
        runtime.set_activity("recording", "paused")
        runtime.set_activity("streaming", "unknown")
        result = self.exchange(os.geteuid(), runtime)
        self.assertEqual(result.returncode, 0, result.stderr)
        response = json.loads(result.stdout)
        self.assertTrue(response["ok"])
        self.assertEqual(response["data"], {"revision": 1, "recording": "paused", "streaming": "unknown"})

    def test_real_client_rejects_invalid_provider_replies(self) -> None:
        bodies = [
            b'{"revision":true,"recording":"active","streaming":"active"}',
            b'{"revision":1,"recording":"bad","streaming":"active"}',
            b'{"revision":1,"recording":"active","streaming":"active","secret":"no"}',
            b'{"revision":1,"revision":1,"recording":"active","streaming":"active"}',
            b'{"revision":1,"recording":"active","streaming":"active"} trailing',
        ]
        for body in bodies:
            with self.subTest(body=body):
                server, client = socket.socketpair()

                def run(connection: socket.socket, reply: bytes) -> None:
                    with connection:
                        connection.recv(4096)
                        connection.sendall(struct.pack("!I", len(reply)) + reply)

                thread = threading.Thread(target=run, args=(server, body))
                thread.start()
                try:
                    result = subprocess.run([str(self.binary), str(client.fileno()), str(os.geteuid())],
                                            pass_fds=(client.fileno(),), text=True, capture_output=True, timeout=8, check=False)
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn("DEVICE_UNAVAILABLE", result.stdout)
                finally:
                    client.close()
                    thread.join(2)
                    self.assertFalse(thread.is_alive())

    def test_real_client_rejects_wrong_peer_uid(self) -> None:
        result = self.exchange(os.geteuid() + 1, SyntheticRuntime())
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("DEVICE_UNAVAILABLE", result.stdout)


if __name__ == "__main__":
    unittest.main()
