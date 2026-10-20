"""Protocol, synthetic behavior and inherited-FD transport regression checks."""
from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

from aula_emulator.lsz1 import (
    ERROR, HEADER, MAX_PAYLOAD, MAX_REQUEST, FaultOptions, Frame, Opcode,
    SyntheticDaemon, serve,
)
from aula_emulator.persistence import PersistenceLoadError


class ModelTests(unittest.TestCase):
    def setUp(self):
        self.now = 10.0
        self.model = SyntheticDaemon(clock=lambda: self.now, step_seconds=1)

    def ask(self, op, value=None, request_id=0, *, raw=None, flags=0):
        payload = raw if raw is not None else json.dumps({} if value is None else value, separators=(",", ":")).encode()
        response = self.model.handle(Frame(op, request_id, payload, flags))
        return response, json.loads(response.payload)

    def originate(self, request_id=1):
        return self.ask(2, {"profile": "private_lab", "layout": "gallery", "meeting_id": "123456789",
                            "passcode": "1234", "host_key": "", "dial_code": ""}, request_id)

    def establish(self):
        self.originate()
        self.now += 4
        self.model.tick()

    def test_framing(self):
        for op in Opcode:
            frame = Frame(op, 0xFFFFFFFF, b"{}")
            self.assertEqual(Frame.decode(frame.encode()), frame)
        frame = Frame(1).encode()
        bad = [b"", frame[:15], frame + b"x", b"XXXX" + frame[4:], frame[:4] + b"\x02" + frame[5:],
               frame[:5] + b"\x0b" + frame[6:], frame[:6] + b"\x00\x80" + frame[8:],
               HEADER.pack(b"LSZ1", 1, 1, 0, 0, MAX_PAYLOAD + 1)]
        for value in bad:
            with self.subTest(value=value), self.assertRaises(ValueError):
                Frame.decode(value)
        for frame in [Frame(0), Frame(1, -1), Frame(1, flags=8), Frame(1, payload=b"x" * (MAX_PAYLOAD + 1))]:
            with self.assertRaises(ValueError):
                frame.encode()

    def test_direct_model_enforces_daemon_payload_bound(self):
        with self.assertRaises(ValueError):
            self.model.handle(Frame(9, payload=b" " * (MAX_REQUEST + 1)))

    def test_read_shapes_and_truth(self):
        _, status = self.ask(1)
        self.assertEqual(status["call_state"], "idle")
        self.assertFalse(status["renderer"]["healthy"])
        self.assertFalse(status["aec"]["available"])
        self.assertEqual(status["video"]["packets"], 0)
        _, diagnostics = self.ask(7)
        self.assertEqual(diagnostics["state"], "not_ready")
        self.assertEqual(diagnostics["checks"]["media_readiness"]["state"], "unavailable")
        _, metrics = self.ask(10)
        self.assertEqual([item["kind"] for item in metrics["streams"]], ["audio", "video"])
        self.assertEqual(self.ask(6, {"username": "name", "password": "secretsecret"})[1]["error"]["code"], "UNSUPPORTED")

    def test_exact_empty_read_contracts(self):
        for op in (1, 7, 8):
            response, value = self.ask(op, raw=b" { } ")
            self.assertTrue(response.flags & ERROR)
            self.assertEqual(value["error"]["code"], "INVALID_REQUEST" if op == 1 else "COMMAND_FAILED")
        for op in (9, 10):
            self.assertFalse(self.ask(op, raw=b" \n{ }\t")[0].flags & ERROR)
            self.assertFalse(self.ask(op, raw=b"")[0].flags & ERROR)
        self.assertEqual(self.ask(1, flags=1)[1]["error"]["code"], "INVALID_FLAGS")

    def test_progression_hangup_and_uncached_events(self):
        first = self.ask(8)[1]
        self.assertEqual(self.originate()[1], {"ok": True})
        for index, state in enumerate(("resolving", "inviting", "early", "establishing_media", "established")):
            self.now = 10 + index
            self.assertEqual(self.ask(1)[1]["call_state"], state)
        latest = self.ask(8)[1]
        self.assertGreater(latest["revision"], first["revision"])
        self.assertEqual(latest["events"][0]["message"], "established")
        self.assertEqual(self.ask(3, request_id=2)[1], {"ok": True})
        self.assertEqual(self.ask(1)[1]["call_state"], "terminating")
        self.now += 1
        self.assertEqual(self.ask(1)[1]["call_state"], "terminated")
        self.assertEqual(self.originate(3)[1], {"ok": True})

    def test_actions_and_validation(self):
        self.assertTrue(self.ask(4, {"tone": "1"})[0].flags & ERROR)
        self.assertTrue(self.ask(3)[0].flags & ERROR)
        self.establish()
        for index, tone in enumerate("0123456789*#ABCD", 10):
            self.assertEqual(self.ask(4, {"tone": tone}, index)[1], {"ok": True})
        self.assertEqual(self.ask(5, {"mode": "disabled"}, 30)[1], {"ok": True})
        self.assertFalse(self.ask(1)[1]["video_transmit_enabled"])
        self.assertEqual(self.ask(5, {"action": "audio_mute", "value": "enabled"}, 31)[1], {"ok": True})
        self.assertTrue(self.ask(1)[1]["audio_muted"])
        for index, action in enumerate(("keyframe", "layout_next"), 32):
            self.assertEqual(self.ask(5, {"action": action, "value": "request"}, index)[1], {"ok": True})
        invalid = [b'{"tone":"a"}', b'{"tone":"12"}', b'{"tone":"1","extra":""}',
                   b'{"tone":"1","tone":"2"}', b'{"tone":"\\u0031"}', b'{"tone":true}',
                   b'{"tone":{}}', b'{"tone":-0}', b'{"tone":"\xc3\xa4"}', b'{"tone":"1"}\v']
        for payload in invalid:
            self.assertTrue(self.ask(4, raw=payload, request_id=100)[0].flags & ERROR)
        self.assertTrue(self.ask(5, raw=b'{"mode":"disabled"}' + b" " * 15, request_id=100)[0].flags & ERROR)
        self.assertTrue(self.ask(5, {"action": "shell", "value": "request"}, 100)[0].flags & ERROR)

    def test_replay_conflict_fifo_ttl_and_failures(self):
        response, _ = self.originate()
        self.now += 4
        self.assertEqual(self.originate()[0], response)
        self.assertEqual(self.ask(3, request_id=1)[1]["error"]["code"], "REQUEST_ID_CONFLICT")
        self.assertEqual(self.ask(2, raw=b" {}", request_id=1)[1]["error"]["code"], "REQUEST_ID_CONFLICT")
        # Failed commands are not cached: same ID can succeed once established.
        self.model = SyntheticDaemon(clock=lambda: self.now)
        self.assertTrue(self.ask(4, {"tone": "1"}, 9)[0].flags & ERROR)
        self.establish()
        self.assertFalse(self.ask(4, {"tone": "1"}, 9)[0].flags & ERROR)
        for ident in range(10, 26):
            self.ask(4, {"tone": "1"}, ident)
        self.assertEqual(len(self.model._completed), 16)
        self.assertFalse(self.ask(4, {"tone": "2"}, 9)[0].flags & ERROR)
        self.now += 121
        self.assertFalse(self.ask(4, {"tone": "3"}, 9)[0].flags & ERROR)

    def test_settings_cas_policy_disabled_and_read_refresh(self):
        read = self.ask(9)[1]
        update = {"revision": 1, "profile": "zoom_direct", "media": "disabled", "tls": "not_required"}
        response, value = self.ask(9, update, 1)
        self.assertEqual(value, {"revision": 2, "profile": "zoom_direct", "media": "disabled", "tls": "required"})
        self.assertEqual(self.ask(9, update, 1)[0], response)
        self.assertEqual(self.ask(9, update, 2)[1]["error"]["code"], "REVISION_CONFLICT")
        self.assertNotEqual(self.ask(9)[1], read)
        self.assertEqual(self.originate(3)[1]["error"]["code"], "PERMISSION_DENIED")
        for payload in (b'{"revision":true,"profile":"private_lab","media":"managed"}',
                        b'{"revision":"2","profile":"private_lab","media":"managed"}',
                        b'{"revision":02,"profile":"private_lab","media":"managed"}',
                        b'{"revision":2,"profile":"private_lab","media":"managed","path":"x"}'):
            self.assertTrue(self.ask(9, raw=payload, request_id=100)[0].flags & ERROR)

    def test_persistence_is_separate_and_safe(self):
        with tempfile.TemporaryDirectory() as directory:
            state = Path(directory) / "activity.json"
            state.write_text("untouched activity file")
            self.model = SyntheticDaemon(state)
            value = {"revision": 1, "profile": "private_lab", "media": "disabled"}
            self.ask(9, value, 1)
            restored = SyntheticDaemon(state)
            self.assertEqual(restored.settings["revision"], 2)
            self.assertEqual(restored.call_state, "idle")
            self.assertEqual(state.read_text(), "untouched activity file")
            saved = Path(str(state) + ".sip-settings").read_text()
            self.assertNotIn("meeting", saved)
            Path(str(state) + ".sip-settings").write_text('{"schema_version":1,"state":{"password":"hidden"}}')
            with self.assertRaises(PersistenceLoadError):
                SyntheticDaemon(state)

    def test_two_daemons_refresh_settings_and_reject_stale_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "activity.json"
            first, second = SyntheticDaemon(path), SyntheticDaemon(path)
            def ask(model, value=None, request_id=0, opcode=9):
                request = Frame(opcode, request_id, json.dumps({} if value is None else value).encode())
                return json.loads(model.handle(request).payload)
            first_update = {"revision": 1, "profile": "private_lab", "media": "disabled"}
            self.assertEqual(ask(first, first_update, 1)["revision"], 2)
            stale = {"revision": 1, "profile": "zoom_direct", "media": "managed"}
            self.assertEqual(ask(second, stale, 2)["error"]["code"], "REVISION_CONFLICT")
            self.assertEqual(ask(second)["media"], "disabled")
            next_update = {**stale, "revision": 2}
            self.assertEqual(ask(second, next_update, 2)["revision"], 3)
            self.assertEqual(ask(first)["revision"], 3)
            self.assertEqual(ask(first, opcode=1)["sip"]["profile"], "zoom_direct")
            # Replays retain the historical mutation response, while reads stay fresh.
            self.assertEqual(ask(first, first_update, 1)["revision"], 2)
            self.assertEqual(ask(first)["revision"], 3)
            self.assertEqual(ask(first, {"revision": 3, "profile": "zoom_direct", "media": "disabled"}, 3)["revision"], 4)
            call = {"profile": "zoom_direct", "layout": "gallery", "meeting_id": "123456789",
                    "passcode": "", "host_key": "", "dial_code": ""}
            self.assertEqual(ask(second, call, 4, 2)["error"]["code"], "PERMISSION_DENIED")

    def test_two_daemons_concurrent_settings_cas_has_one_winner(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "activity.json"
            models = [SyntheticDaemon(path), SyntheticDaemon(path)]
            barrier = threading.Barrier(2)
            responses = []
            failures = []
            def update(model, ident):
                try:
                    barrier.wait(timeout=2)
                    payload = json.dumps({"revision": 1, "profile": "private_lab", "media": "disabled"}).encode()
                    responses.append(json.loads(model.handle(Frame(9, ident, payload)).payload))
                except BaseException as exc:
                    failures.append(exc)
            workers = [threading.Thread(target=update, args=(model, index)) for index, model in enumerate(models)]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(3)
                self.assertFalse(worker.is_alive())
            self.assertEqual(failures, [])
            self.assertEqual(sum(value.get("revision") == 2 for value in responses), 1)
            self.assertEqual(sum(value.get("error", {}).get("code") == "REVISION_CONFLICT" for value in responses), 1)
            self.assertEqual(SyntheticDaemon(path).settings["revision"], 2)

    def test_refreshed_settings_are_validated_before_use(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "activity.json"
            model = SyntheticDaemon(path)
            model._store.save({"revision": 2, "profile": "unknown", "media": "disabled", "tls": "not_required"})
            response = model.handle(Frame(9))
            self.assertEqual(json.loads(response.payload)["error"]["code"], "COMMAND_FAILED")
            # A safe externally repaired state is visible on the next uncached read.
            model._store.save({"revision": 3, "profile": "private_lab", "media": "disabled", "tls": "not_required"})
            self.assertEqual(json.loads(model.handle(Frame(9)).payload)["revision"], 3)

    def test_persistence_fault_and_precommit_failure(self):
        self.model = SyntheticDaemon(faults=FaultOptions(persistence_uncertain=True))
        value = {"revision": 1, "profile": "private_lab", "media": "disabled"}
        response, result = self.ask(9, value, 1)
        self.assertEqual(result["error"]["code"], "PERSISTENCE_UNCERTAIN")
        self.assertEqual(self.ask(9)[1]["revision"], 2)
        self.assertEqual(self.ask(9, value, 1)[0], response)
        self.model = SyntheticDaemon()
        with patch.object(self.model._store, "save", side_effect=OSError("failure")):
            self.assertEqual(self.ask(9, value, 1)[1]["error"]["code"], "COMMAND_FAILED")
        self.assertEqual(self.ask(9)[1]["revision"], 1)
        self.assertFalse(self.ask(9, value, 1)[0].flags & ERROR)

    def test_settings_during_call_overflow_and_postcommit_failure(self):
        self.establish()
        value = {"revision": 1, "profile": "private_lab", "media": "disabled"}
        self.assertEqual(self.ask(9, value, 50)[1]["error"]["code"], "COMMAND_FAILED")
        self.model = SyntheticDaemon()
        self.model.settings["revision"] = 0xFFFFFFFF
        value["revision"] = 0xFFFFFFFF
        self.assertEqual(self.ask(9, value, 50)[1]["error"]["code"], "REVISION_CONFLICT")
        self.model = SyntheticDaemon()
        value["revision"] = 1
        # The shared persistence module raises a PersistenceError subtype.
        from aula_emulator.persistence import PersistenceError
        class Uncertain(PersistenceError):
            committed = True
        with patch.object(self.model._store, "save", side_effect=Uncertain("directory fsync")):
            response, result = self.ask(9, value, 50)
        self.assertEqual(result["error"]["code"], "PERSISTENCE_UNCERTAIN")
        self.assertEqual(self.ask(9)[1]["revision"], 2)
        self.assertEqual(self.ask(9, value, 50)[0], response)

    def test_originate_rejects_unsafe_fields_and_profile_mismatch(self):
        value = {"profile": "private_lab", "layout": "gallery", "meeting_id": "123456789",
                 "passcode": "", "host_key": "", "dial_code": ""}
        invalid = {"meeting_id": ("", "1" * 17, "sip:123"), "passcode": ("abc", "1" * 33),
                   "host_key": ("key", "1" * 17), "dial_code": ("a@b", "a" * 65),
                   "profile": ("zoom_direct", "unknown"), "layout": ("unknown",)}
        for key, choices in invalid.items():
            for choice in choices:
                with self.subTest(key=key, choice=choice):
                    self.assertTrue(self.ask(2, {**value, key: choice}, 1)[0].flags & ERROR)
                    self.assertEqual(self.model.call_state, "idle")

    def test_oversized_reply_retains_replay_unavailable(self):
        self.model = SyntheticDaemon(faults=FaultOptions(oversized_response=True))
        payload = b'{"revision":1,"profile":"private_lab","media":"disabled"}'
        request = Frame(9, 10, payload)
        self.assertEqual(len(self.model.handle(request).payload), MAX_REQUEST + 1)
        self.assertEqual(json.loads(self.model.handle(request).payload)["error"]["code"], "REPLAY_UNAVAILABLE")
        self.assertEqual(self.model.settings["revision"], 2)


class TransportTests(unittest.TestCase):
    def exchange(self, payload, *, kind=socket.SOCK_STREAM, chunks=None, daemon=None):
        client, server = socket.socketpair(type=kind)
        failures = []
        def run():
            try:
                serve(server.fileno(), daemon=daemon)
            except BaseException as error:
                failures.append(error)
            finally:
                server.close()
        worker = threading.Thread(target=run)
        worker.start()
        try:
            client.settimeout(2)
            if chunks:
                for chunk in chunks:
                    client.sendall(chunk)
                    time.sleep(0.005)
            else:
                client.sendall(payload)
            if kind == socket.SOCK_STREAM:
                reply = bytearray()
                while data := client.recv(4096):
                    reply.extend(data)
                result = bytes(reply)
            else:
                result = client.recv(4096)
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertEqual(failures, [])
            return result
        finally:
            client.close()
            worker.join(2)

    def test_connected_stream_partial_header_body_and_eof(self):
        request = Frame(1).encode()
        reply = self.exchange(request, chunks=[request[:3], request[3:15], request[15:17], request[17:]])
        self.assertEqual(json.loads(Frame.decode(reply).payload)["call_state"], "idle")

    def test_keeps_inherited_fd_open_but_finishes_response_with_eof(self):
        client, server = socket.socketpair()
        worker = threading.Thread(target=lambda: serve(server.fileno()))
        worker.start()
        try:
            client.settimeout(2)
            client.sendall(Frame(9).encode())
            reply = bytearray()
            while chunk := client.recv(4096):
                reply.extend(chunk)
            worker.join(2)
            self.assertFalse(worker.is_alive())
            self.assertEqual(Frame.decode(bytes(reply)).opcode, 9)
            self.assertGreaterEqual(server.fileno(), 0)
            self.assertTrue(os.get_blocking(server.fileno()))
        finally:
            client.close()
            worker.join(2)
            server.close()

    @unittest.skipUnless(hasattr(socket, "SOCK_SEQPACKET") and os.uname().sysname != "Darwin", "Linux packet lane")
    def test_connected_seqpacket_and_truncation(self):
        self.assertEqual(Frame.decode(self.exchange(Frame(9).encode(), kind=socket.SOCK_SEQPACKET)).opcode, 9)
        self.assertEqual(self.exchange(b"LSZ1", kind=socket.SOCK_SEQPACKET), b"")

    def test_invalid_frame_and_oversized_request_close(self):
        for payload in [b"BAD!" + Frame(1).encode()[4:], Frame(1).encode() + b"extra",
                        HEADER.pack(b"LSZ1", 1, 1, 0, 0, MAX_REQUEST + 1)]:
            self.assertEqual(self.exchange(payload), b"")

    def test_delay_is_deterministic(self):
        before = time.monotonic()
        model = SyntheticDaemon(faults=FaultOptions(reply_delay=0.06))
        self.assertEqual(Frame.decode(self.exchange(Frame(9).encode(), daemon=model)).opcode, 9)
        self.assertGreaterEqual(time.monotonic() - before, 0.06)

    def test_inherited_listener_multiple_connections_and_stop(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / "control.sock")
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                listener.bind(path)
            except PermissionError:
                listener.close()
                self.skipTest("sandbox denies fixture Unix bind; run transport lane outside sandbox")
            listener.listen(8)
            stop = threading.Event()
            failures = []
            def run():
                try:
                    serve(listener.fileno(), stop_event=stop)
                except BaseException as error:
                    failures.append(error)
            worker = threading.Thread(target=run)
            worker.start()
            try:
                for opcode in (1, 7, 8, 9, 10):
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.settimeout(2)
                        client.connect(path)
                        client.sendall(Frame(opcode).encode())
                        reply = bytearray()
                        while chunk := client.recv(4096):
                            reply.extend(chunk)
                        self.assertEqual(Frame.decode(bytes(reply)).opcode, opcode)
            finally:
                stop.set()
                worker.join(2)
                listener.close()
            self.assertFalse(worker.is_alive())
            self.assertEqual(failures, [])
            self.assertTrue(Path(path).is_socket())

    def test_invalid_fd_and_options(self):
        for value in (-1, True):
            with self.assertRaises(ValueError):
                serve(value)
        for value in (-1, float("nan"), float("inf"), 31, True):
            with self.assertRaises(ValueError):
                FaultOptions(reply_delay=value)
        for value in (0, -1, float("nan")):
            with self.assertRaises(ValueError):
                SyntheticDaemon(step_seconds=value)
