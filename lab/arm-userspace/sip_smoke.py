#!/usr/bin/env python3
"""One isolated loopback UDP call through the real ARM PJSIP-linked daemon."""
from __future__ import annotations

import argparse
import dataclasses
import hashlib
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HEADER = struct.Struct("!4sBBHII")


def client(path: str, opcode: int, request: int, payload: str) -> dict:
    data = payload.encode("ascii")
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
        connection.settimeout(3)
        connection.connect(path)
        connection.sendall(HEADER.pack(b"LSZ1", 1, opcode, 0, request, len(data)) + data)
        response = connection.recv(65552)
    if len(response) < HEADER.size:
        raise ValueError("short LSZ1 reply")
    magic, version, received_opcode, flags, received_request, length = HEADER.unpack_from(response)
    if (magic, version, received_opcode, received_request, length) != (
            b"LSZ1", 1, opcode, request, len(response) - HEADER.size) or flags != 1:
        raise ValueError(f"LSZ1 reply rejected: opcode={opcode} flags={flags} payload={response[16:]!r}")
    return json.loads(response[16:])


def smoke(args: argparse.Namespace) -> dict:
    # This is a public source helper with a protocol-neutral peer; no private
    # session, physical identity or transport tooling is imported.
    sys.path.insert(0, str(ROOT / "lab/sip-peer"))
    from private_sip_peer import SipRtpPeer

    if sys.platform != "linux":
        raise ValueError("the daemon SIP smoke requires a disposable Linux environment")
    gateway_uid = 10001 if os.geteuid() == 0 else os.geteuid()
    if gateway_uid in (0, 65534):
        raise ValueError("use root in a disposable container or a dedicated non-nobody user")
    identity = {"user": gateway_uid, "group": os.getegid(), "extra_groups": []} if os.geteuid() == 0 else {}
    command = [args.qemu_arm, "-cpu", "cortex-a8", "-L", str(args.sysroot), str(args.binary)]
    with tempfile.TemporaryDirectory(prefix="aula-arm-sip-") as temporary:
        runtime = Path(temporary)
        runtime.chmod(0o755)
        control = runtime / "control.sock"
        # The fixture backend still opens both configured inputs when inactive
        # media is committed. Empty local files satisfy that contract without
        # supplying or claiming media payloads.
        (runtime / "video.h264").touch()
        (runtime / "audio.pcm").touch()
        request_id = 100

        def exchange(opcode: int, payload: dict | None = None) -> dict:
            nonlocal request_id
            request_id += 1
            # The production peer-UID check remains enabled. When the daemon
            # runs as container root, only this client drops to gateway UID.
            result = subprocess.run(
                [sys.executable, str(Path(__file__).resolve()), "--client", str(control),
                 str(opcode), str(request_id), json.dumps(payload or {}, separators=(",", ":"))],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5, check=False, **identity)
            if result.returncode:
                raise ValueError(f"LSZ1 client failed: {result.stderr.decode(errors='replace')}")
            return json.loads(result.stdout)

        with SipRtpPeer(0, bind_address="127.0.0.1", advertised_address="127.0.0.1",
                        authorized_source_address="127.0.0.1", signaling_only=True) as peer:
            config = (ROOT / "product/sipd/config/aula-sipd.host.example.conf").read_text()
            replacements = {
                "pid_file": str(runtime / "daemon.pid"), "settings_file": str(runtime / "settings.json"),
                "uri": f"sip:aula@127.0.0.1:{peer.port}", "enable_public_network": "true",
                "enable_local_control": "true", "unix_socket_path": str(control),
                "gateway_uid": str(gateway_uid),
                "video_source": str(runtime / "video.h264"), "audio_source": str(runtime / "audio.pcm"),
            }
            for key, value in replacements.items():
                config, count = re.subn(rf"^{key} =.*$", f"{key} = {value}", config, flags=re.MULTILINE)
                if count != 1:
                    raise ValueError(f"configuration template lacks unique {key}")
            config_path = runtime / "daemon.conf"
            config_path.write_text(config)
            config_path.chmod(0o600)
            logfile = args.evidence.with_suffix(".daemon.log")
            with logfile.open("wb") as log:
                process = subprocess.Popen([*command, "--foreground", "--config", str(config_path)],
                                           stdout=log, stderr=subprocess.STDOUT)
                try:
                    deadline = time.monotonic() + 10
                    while not control.exists():
                        if process.poll() is not None:
                            raise ValueError(f"daemon exited during startup; see {logfile}")
                        if time.monotonic() >= deadline:
                            raise ValueError("daemon control socket startup timed out")
                        time.sleep(0.02)
                    initial = exchange(1)
                    if initial.get("call_state") != "idle":
                        raise ValueError(f"fresh daemon did not start idle: {initial}")
                    originated = exchange(2, {"profile": "private_lab", "layout": "gallery",
                                             "meeting_id": "123456789", "passcode": "",
                                             "host_key": "", "dial_code": ""})
                    deadline = time.monotonic() + 10
                    while True:
                        peer.assert_healthy()
                        established = exchange(1)
                        if established.get("call_state") == "established" and peer.evidence.ack_seen:
                            break
                        if time.monotonic() >= deadline:
                            raise ValueError(f"real SIP call was not established: {established}")
                        time.sleep(0.05)
                    hung_up = exchange(3)
                    deadline = time.monotonic() + 10
                    while not peer.evidence.bye_seen:
                        peer.assert_healthy()
                        if time.monotonic() >= deadline:
                            raise ValueError("peer did not observe the real SIP BYE")
                        time.sleep(0.02)
                    while True:
                        final = exchange(1)
                        if final.get("call_state") in ("terminated", "idle"):
                            break
                        if time.monotonic() >= deadline:
                            raise ValueError(f"daemon did not finish hangup: {final}")
                        time.sleep(0.02)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                if process.returncode != 0:
                    raise ValueError(f"daemon shutdown failed ({process.returncode}); see {logfile}")
            peer.assert_healthy()
            evidence = dataclasses.asdict(peer.evidence)
            if (not all(evidence[name] for name in ("invite_seen", "ack_seen", "bye_seen")) or
                    evidence["dialogs_completed"] != 1 or evidence["rejected_sip_packets"] != 0):
                raise ValueError(f"peer did not validate exactly one clean SIP dialog: {evidence}")
            return {
                "schema": "aula-arm-sip-loopback-v1", "status": "passed", "peer": evidence,
                "initial_status": initial, "originate_response": originated,
                "established_status": established, "hangup_response": hung_up, "final_status": final,
                "daemon_exit_code": process.returncode, "gateway_uid": gateway_uid,
                "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                "peer_source_sha256": hashlib.sha256((ROOT / "lab/sip-peer/private_sip_peer.py").read_bytes()).hexdigest(),
                "limits": "Loopback UDP signaling only; TLS, media I/O and board hardware are unproven",
            }


def main() -> int:
    if len(sys.argv) == 6 and sys.argv[1] == "--client":
        print(json.dumps(client(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5])))
        return 0
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu-arm", required=True)
    parser.add_argument("--sysroot", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = smoke(args)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        args.evidence.write_text(json.dumps({"schema": "aula-arm-sip-loopback-v1", "status": "failed", "error": str(error)}, indent=2) + "\n")
        print(str(error), file=sys.stderr)
        return 1
    args.evidence.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print("ARM PJSIP daemon loopback INVITE/ACK/BYE passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
