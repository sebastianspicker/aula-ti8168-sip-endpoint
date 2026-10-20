"""Real gateway C transport/normalizer against inherited socketpair emulator."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
from typing import Any
import unittest

from aula_emulator.lsz1 import FaultOptions, SyntheticDaemon, serve

ROOT = Path(__file__).resolve().parents[3]


class GatewayContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cc") or not shutil.which("pkg-config"):
            raise unittest.SkipTest("C compiler and pkg-config required for gateway contract")
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "jansson", "openssl"], capture_output=True, text=True)
        if flags.returncode:
            raise unittest.SkipTest("gateway contract requires jansson and OpenSSL development packages")
        cls.directory = tempfile.TemporaryDirectory(prefix="aula-lsz1-contract-")
        cls.binary = Path(cls.directory.name) / "gateway-contract"
        gateway = subprocess.check_output(["make", "-s", "-C", str(ROOT / "product/console"), "print-gateway-sources"], text=True).split()
        device = subprocess.check_output(["make", "-s", "-C", str(ROOT / "product/console"), "print-device-client-sources"], text=True).split()
        command = ["cc", "-std=c11", "-D_GNU_SOURCE", "-Dconnect=contract_connect",
                   "-I" + str(ROOT / "product/console/gateway"), "-I" + str(ROOT / "product/sipd/include"),
                   str(Path(__file__).with_name("lsz1_contract.c")),
                   *[str(ROOT / "product/console" / source) for source in gateway + device],
                   *shlex.split(flags.stdout), "-pthread", "-o", str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            cls.directory.cleanup()
            raise AssertionError("gateway contract compilation failed:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="aula-lsz1-peer-")
        self.path = str(Path(self.temporary.name).resolve() / "control.sock")
        kind = socket.SOCK_STREAM if sys.platform == "darwin" else socket.SOCK_SEQPACKET
        self.socket_kind = kind
        self.fixture = socket.socket(socket.AF_UNIX, kind)
        try:
            self.fixture.bind(self.path)
        except PermissionError:
            self.fixture.close()
            self.temporary.cleanup()
            self.skipTest("sandbox denies fixture Unix bind; run transport lane outside sandbox")
        os.chmod(self.path, 0o600)
        self.model = SyntheticDaemon(step_seconds=0.001)

    def tearDown(self):
        self.fixture.close()
        self.temporary.cleanup()

    def exchange(self, opcode, value=None, request_id=0, expected=1) -> Any:
        payload = json.dumps({} if value is None else value, separators=(",", ":"))
        client, server = socket.socketpair(type=self.socket_kind)
        failures = []
        def run():
            try:
                serve(server.fileno(), daemon=self.model)
            except BaseException as exc:
                failures.append(exc)
            finally:
                server.close()
        worker = threading.Thread(target=run)
        worker.start()
        try:
            result = subprocess.run([str(self.binary), str(client.fileno()), self.path,
                                     str(opcode), str(request_id), payload, str(expected)],
                                    pass_fds=(client.fileno(),), capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        finally:
            client.close()
            worker.join(3)
        self.assertFalse(worker.is_alive())
        self.assertEqual(failures, [])
        return json.loads(result.stdout) if result.stdout.strip() else None

    def test_all_read_opcodes_and_real_gateway_schema(self):
        for opcode in (1, 7, 8, 9, 10):
            with self.subTest(opcode=opcode):
                self.assertIsInstance(self.exchange(opcode), dict)

    def test_settings_success_conflict_uncertain_and_replay(self):
        value = {"revision": 1, "profile": "zoom_direct", "media": "managed"}
        result = self.exchange(9, value, 1)
        self.assertEqual(result["revision"], 2)
        self.assertEqual(result, self.exchange(9, value, 1))
        self.assertEqual(self.exchange(9, value, 2, 2)["error"]["code"], "REVISION_CONFLICT")
        self.assertEqual(self.exchange(3, request_id=1, expected=2)["error"]["code"], "REQUEST_ID_CONFLICT")
        self.model = SyntheticDaemon(faults=FaultOptions(persistence_uncertain=True))
        result = self.exchange(9, value, 1, 2)
        self.assertEqual(result["error"]["code"], "PERSISTENCE_UNCERTAIN")
        self.assertEqual(self.exchange(9)["revision"], 2)
        self.assertEqual(self.exchange(9, value, 1, 2), result)

    def test_call_media_dtmf_and_hangup(self):
        import time
        value = {"profile": "private_lab", "layout": "gallery", "meeting_id": "123456789",
                 "passcode": "", "host_key": "", "dial_code": ""}
        self.assertEqual(self.exchange(2, value, 1), {"ok": True})
        time.sleep(0.01)
        self.assertEqual(self.exchange(1)["call_state"], "established")
        self.assertEqual(self.exchange(4, {"tone": "#"}, 2), {"ok": True})
        self.assertEqual(self.exchange(5, {"mode": "disabled"}, 3), {"ok": True})
        self.assertFalse(self.exchange(1)["video_transmit_enabled"])
        self.assertEqual(self.exchange(3, request_id=4), {"ok": True})

    def test_real_gateway_rejects_oversized_and_times_out_delayed_reply(self):
        self.model = SyntheticDaemon(faults=FaultOptions(oversized_response=True))
        self.assertIsNone(self.exchange(1, expected=0))
        self.model = SyntheticDaemon(faults=FaultOptions(reply_delay=0.2))
        self.assertIsNone(self.exchange(1, expected=0))
