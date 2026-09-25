"""Pinned target transport, baseline, TLS, and console operations."""

from __future__ import annotations

import contextlib
import getpass
import hashlib
import http.client
import json
import os
import re
import secrets
import socket
import ssl
import stat
import struct
import subprocess
import sys
import tempfile
import uuid
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Callable, Iterator, Sequence

from campaign_evidence import _private_root, _secure_directory
from campaign_types import (
    BOOT_ID_COMMAND as _BOOT_ID_COMMAND, BOOT_ID_PATTERN, CONTROL_IDENTITY_REQUEST,
    CONTROL_IDENTITY_RESPONSE, CampaignError, ROOT, SAFE_ENV, SECRET_PATTERN,
)
from session import LiveSession


def _run(
    argv: Sequence[str], *, input_bytes: bytes | None = None, capture: bool = True,
    timeout: float = 30.0, check: bool = True, cwd: Path | None = None,
) -> subprocess.CompletedProcess[bytes]:
    completed = subprocess.run(
        list(argv), input=input_bytes, stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None, env=SAFE_ENV, cwd=cwd,
        timeout=timeout, check=False,
    )
    if check and completed.returncode != 0:
        raise CampaignError(f"command failed without retained output: {Path(argv[0]).name}")
    return completed

def _ssh_options(session: LiveSession, known_hosts: Path, control_path: Path | None = None) -> list[str]:
    options = [
        "-4", "-F", "/dev/null",
        "-p", str(session.ssh_port),
        "-o", "HostKeyAlgorithms=ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521",
        "-o", "StrictHostKeyChecking=yes",
        "-o", f"UserKnownHostsFile={known_hosts}",
        "-o", "GlobalKnownHostsFile=/dev/null",
        "-o", "PubkeyAuthentication=no",
        "-o", "PreferredAuthentications=keyboard-interactive,password",
        "-o", "PasswordAuthentication=yes",
        "-o", "NumberOfPasswordPrompts=1",
        "-o", "IdentitiesOnly=yes",
        "-o", "IdentityAgent=none",
        "-o", "ProxyCommand=none",
        "-o", "PermitLocalCommand=no",
        "-o", "ClearAllForwardings=yes",
        "-o", "LogLevel=ERROR",
    ]
    if control_path is not None:
        options += ["-o", f"ControlPath={control_path}"]
    return options


def _scan_host_key(
    session: LiveSession, directory: Path, *, runner: Callable[..., Any] = _run,
) -> tuple[Path, list[str]]:
    scan = runner(
        ["/usr/bin/ssh-keyscan", "-4", "-p", str(session.ssh_port), "-T", "5", "-t", "ecdsa", session.target_ipv4],
        timeout=10.0,
    ).stdout
    matches: list[bytes] = []
    fingerprints: list[str] = []
    for line in scan.splitlines():
        if not line or line.startswith(b"#"):
            continue
        result = runner(["/usr/bin/ssh-keygen", "-lf", "-", "-E", "sha256"], input_bytes=line + b"\n")
        fields = result.stdout.decode("ascii", "strict").split()
        if len(fields) < 2:
            raise CampaignError("SSH key scan produced an ambiguous fingerprint")
        fingerprints.append(fields[1])
        if fields[1] == session.ssh_fingerprint:
            matches.append(line)
    if len(matches) != 1:
        raise CampaignError("SSH key scan has zero or multiple approved ECDSA lines")
    known = directory / "known-hosts"
    descriptor = os.open(known, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "wb") as handle:
        handle.write(matches[0] + b"\n")
    return known, fingerprints


@contextlib.contextmanager
def _ssh_master(
    session: LiveSession, *, topology: Callable[..., None],
    scan_host_key: Callable[..., tuple[Path, list[str]]],
) -> Iterator[tuple[Path, Path]]:
    with tempfile.TemporaryDirectory(prefix="ssh-", dir=_private_root(session)) as temporary:
        directory = Path(temporary)
        os.chmod(directory, 0o700)
        topology(session, contact=True)
        known, _fingerprints = scan_host_key(session, directory)
        control = directory / "control"
        target = f"{session.ssh_user}@{session.target_ipv4}"
        # Authentication is intentionally attached to the operator's terminal.
        _run(
            ["/usr/bin/ssh", *_ssh_options(session, known), "-M", "-o", f"ControlPath={control}",
             "-o", "ControlPersist=no", "-N", "-f", target],
            capture=False, timeout=180.0,
        )
        try:
            topology(session, contact=True)
            yield known, control
        finally:
            _run(
                ["/usr/bin/ssh", *_ssh_options(session, known, control), "-O", "exit", target],
                check=False, timeout=10.0,
            )


def _ssh(session: LiveSession, known: Path, control: Path, command: str,
         *, capture: bool = True, timeout: float = 60.0) -> bytes:
    target = f"{session.ssh_user}@{session.target_ipv4}"
    result = _run(
        ["/usr/bin/ssh", *_ssh_options(session, known, control), "-o", "BatchMode=yes", target, command],
        capture=capture, timeout=timeout,
    )
    return result.stdout if capture else b""


def _control_identity(session: LiveSession) -> None:
    if sys.platform != "darwin":
        raise CampaignError("control identity binding is supported only on macOS")
    connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        interface_index = socket.if_nametoindex(session.interface)
        if interface_index <= 0:
            raise CampaignError("the authorized interface has no valid index")
        connection.setsockopt(socket.IPPROTO_IP, 25, struct.pack("I", interface_index))
        connection.settimeout(3.0)
        connection.connect((session.target_ipv4, 5080))
        connection.sendall(CONTROL_IDENTITY_REQUEST)
        response = bytearray()
        while len(response) < 3:
            chunk = connection.recv(3 - len(response))
            if not chunk:
                raise CampaignError("the LS-200 control identity response was truncated")
            response.extend(chunk)
        frame_size = 4 + response[2]
        if frame_size != len(CONTROL_IDENTITY_RESPONSE):
            raise CampaignError("the LS-200 control identity frame size was inexact")
        while len(response) < frame_size:
            chunk = connection.recv(frame_size - len(response))
            if not chunk:
                raise CampaignError("the LS-200 control identity response was truncated")
            response.extend(chunk)
    except OSError as error:
        raise CampaignError("the interface-bound LS-200 control identity probe failed") from error
    finally:
        connection.close()
    if bytes(response) != CONTROL_IDENTITY_RESPONSE:
        raise CampaignError("the target did not return the exact GM LS-200 identity")


def _verify_identity(session: LiveSession, known: Path, control: Path) -> None:
    _control_identity(session)
    output = _ssh(
        session, known, control,
        "set -eu; [ \"$(id -u)\" = 0 ]; "
        "[ -f /etc/os-release ] && [ ! -L /etc/os-release ]; "
        "[ \"$(ls -nd /etc/os-release | awk '{print $3}')\" = 0 ]; "
        "! find /etc/os-release -prune \\( -perm -020 -o -perm -002 \\) | grep -q .; "
        "manufacturer=$(awk -F= '$1 == \"MANUFACTURER\" {print $2; exit}' /etc/os-release); "
        "name=$(awk -F= '$1 == \"NAME\" {print $2; exit}' /etc/os-release); "
        "product=$(awk -F= '$1 == \"ID\" {print $2; exit}' /etc/os-release); "
        "firmware=$(awk -F= '$1 == \"VERSION\" {print $2; exit}' /etc/os-release); "
        "[ \"$manufacturer\" = 'AREC Inc.' ]; [ \"$name\" = LS-200 ]; "
        "[ \"$product\" = LS-200VA1 ]; [ \"$firmware\" = v2.11.26.80 ]; "
        "printf 'manufacturer=AREC Inc.\\nname=LS-200\\nproduct=LS-200VA1\\nfirmware=v2.11.26.80\\n'",
    )
    if output != b"manufacturer=AREC Inc.\nname=LS-200\nproduct=LS-200VA1\nfirmware=v2.11.26.80\n":
        raise CampaignError("the authenticated target did not provide the exact LS-200 identity proof")


def _authenticated_boot_id(
    session: LiveSession, known: Path, control: Path, *, ssh: Callable[..., bytes] = _ssh,
) -> str:
    """Return one exact, authenticated boot identifier without retaining it as evidence."""
    output = ssh(session, known, control, _BOOT_ID_COMMAND)
    try:
        boot_id = output.decode("ascii", "strict")
    except UnicodeDecodeError as error:
        raise CampaignError("authenticated boot identifier is malformed") from error
    if not BOOT_ID_PATTERN.fullmatch(boot_id.rstrip("\n")) or boot_id != boot_id.rstrip("\n") + "\n":
        raise CampaignError("authenticated boot identifier is malformed")
    return boot_id.rstrip("\n")


def _reboot_persistent_contract(version: str) -> str:
    """Return the target-state assertions that must hold on both sides of reboot."""
    return f"""[ -L \"$current\" ]
selector=$(readlink \"$current\")
[ \"$selector\" = \"releases/{version}\" ]
[ -d \"$base/releases/{version}\" ] && [ ! -L \"$base/releases/{version}\" ]
[ -f \"$marker\" ] && [ ! -L \"$marker\" ]
[ \"$(LC_ALL=C ls -lnd \"$marker\" | awk 'NR == 1 {{print $1 \":\" $3 \":\" $4}}')\" = \"-rw-------:0:0\" ]
[ \"$(cat \"$marker\")\" = enabled-v1 ]
[ -f \"$journal\" ] && [ ! -L \"$journal\" ]
[ \"$(LC_ALL=C ls -lnd \"$journal\" | awk 'NR == 1 {{print $1 \":\" $3 \":\" $4}}')\" = \"-rw-------:0:0\" ]
[ \"$(grep -Fxc \"file:$marker\" \"$journal\")\" = 1 ]
trust_helper=$base/live-transaction-records.sh
[ -f \"$trust_helper\" ] && [ ! -L \"$trust_helper\" ]
[ \"$(LC_ALL=C ls -lnd \"$trust_helper\" | awk 'NR == 1 {{print $1 ":" $3 ":" $4}}')\" = \"-r--r--r--:0:0\" ]
die() {{ printf '%s\\n' "$*" >&2; exit 1; }}
[ \"$(grep -Fxc \"file:$trust_helper\" \"$journal\")\" = 1 ]
BASE=$base; JOURNAL=$journal
. \"$trust_helper\"
verify_installed_release \"{version}\""""


def _reboot_pending_absence_contract() -> str:
    """Return an explicit absence check that is not weakened by shell `set -e` rules."""
    return """for pending in \"$base/rollback-transaction\" \"$base/autostart-transaction\" \"$base/cron-transaction\" /var/run/ls200-zoom-operation.lock; do
    if [ -e \"$pending\" ] || [ -L \"$pending\" ]; then
        exit 1
    fi
done"""


def _reboot_preflight_command(version: str) -> str:
    """Build a side-effect-free target-state gate immediately before reboot."""
    if not re.fullmatch(r"[A-Za-z0-9._-]+", version):
        raise CampaignError("reboot selector version is unsafe")
    return f"""set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
base=/var/lib/cbox/ls200-zoom
current=$base/current
marker=$base/autostart-enabled
journal=$base/live-owned-files
[ \"$(id -u)\" = 0 ]
{_reboot_persistent_contract(version)}
{_reboot_pending_absence_contract()}
\"$base/live-start.sh\" health
{_reboot_persistent_contract(version)}
{_reboot_pending_absence_contract()}
sync && /sbin/reboot"""


def _reboot_postboot_command(version: str) -> str:
    """Build the strict state gate that proves the cron-mediated autostart ran."""
    if not re.fullmatch(r"[A-Za-z0-9._-]+", version):
        raise CampaignError("reboot selector version is unsafe")
    return f"""set -eu
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
base=/var/lib/cbox/ls200-zoom
current=$base/current
marker=$base/autostart-enabled
journal=$base/live-owned-files
sentinel=/var/run/ls200-zoom-autostart.ok
[ \"$(id -u)\" = 0 ]
{_reboot_persistent_contract(version)}
sentinel_wait=75
while [ \"$sentinel_wait\" -gt 0 ] && [ ! -e \"$sentinel\" ] && [ ! -L \"$sentinel\" ]; do
    sleep 1
    sentinel_wait=$((sentinel_wait - 1))
done
[ -f \"$sentinel\" ] && [ ! -L \"$sentinel\" ]
[ \"$(LC_ALL=C ls -lnd \"$sentinel\" | awk 'NR == 1 {{print $1 \":\" $3 \":\" $4}}')\" = \"-rw-------:0:0\" ]
{_reboot_persistent_contract(version)}
{_reboot_pending_absence_contract()}
\"$base/live-start.sh\" health
{_reboot_persistent_contract(version)}
{_reboot_pending_absence_contract()}"""


def _remote_baseline(
    session: LiveSession, known: Path, control: Path, *, ssh: Callable[..., bytes] = _ssh,
) -> dict[str, str]:
    command = (
        "set -eu; printf 'mounts=%s\\n' \"$(sha256sum /proc/mounts | awk '{print $1}')\"; "
        "printf 'crontab=%s\\n' \"$(sha256sum /var/lib/cbox/crontabs/root | awk '{print $1}')\"; "
        "printf 'passwd=%s\\n' \"$(sha256sum /etc/passwd | awk '{print $1}')\"; "
        "printf 'group=%s\\n' \"$(sha256sum /etc/group | awk '{print $1}')\"; "
        "printf 'shadow=%s\\n' \"$(sha256sum /etc/shadow | awk '{print $1}')\"; "
        "printf 'passwd_meta=%s\\n' \"$(ls -ln /etc/passwd | awk '{print $3 \"_\" $4 \"_\" $1}')\"; "
        "printf 'group_meta=%s\\n' \"$(ls -ln /etc/group | awk '{print $3 \"_\" $4 \"_\" $1}')\"; "
        "printf 'shadow_meta=%s\\n' \"$(ls -ln /etc/shadow | awk '{print $3 \"_\" $4 \"_\" $1}')\"; "
        "printf 'firmware=%s\\n' \"$(awk -F= '$1 == \"VERSION\" {print $2; exit}' /etc/os-release)\"; "
        "printf 'product_id=%s\\n' \"$(awk -F= '$1 == \"ID\" {print $2; exit}' /etc/os-release)\"; "
        "printf 'os_release=%s\\n' \"$(sha256sum /etc/os-release | awk '{print $1}')\"; "
        "printf 'cpu_ticks=%s\\n' \"$(awk '/^cpu / {total=0; for (i=2;i<=NF;i++) total+=$i; print total; exit}' /proc/stat)\"; "
        "printf 'fd_total=%s\\n' \"$(find /proc/[0-9]*/fd -mindepth 1 -maxdepth 1 2>/dev/null | wc -l | tr -d ' ')\"; "
        "printf 'sockets=%s\\n' \"$(cat /proc/net/tcp /proc/net/tcp6 /proc/net/udp /proc/net/udp6 2>/dev/null | sha256sum | awk '{print $1}')\"; "
        "printf 'diskstats=%s\\n' \"$(sha256sum /proc/diskstats | awk '{print $1}')\"; "
        "printf 'persistent_paths=%s\\n' \"$(find /var/lib/cbox -mindepth 1 -maxdepth 1 -print | LC_ALL=C sort | sha256sum | awk '{print $1}')\"; "
        "printf 'mem_available_kib=%s\\n' \"$(awk '"
        "$1 == \"MemAvailable:\" {print $2; found=1; exit} "
        "$1 == \"MemFree:\" {free=$2} $1 == \"Buffers:\" {buffers=$2} "
        "$1 == \"Cached:\" {cached=$2} $1 == \"SReclaimable:\" {reclaim=$2} "
        "$1 == \"Shmem:\" {shmem=$2} END {if (!found) print free+buffers+cached+reclaim-shmem}' "
        "/proc/meminfo)\"; "
        "printf 'free_kib=%s\\n' \"$(df -k /var/lib/cbox | awk 'END {print $4}')\"; "
        "printf 'jffs_kib=%s\\n' \"$(du -sk /var/lib/cbox | awk '{print $1}')\"; "
        "printf 'vendor_media=%s\\n' \"$([ -x /usr/share/media/wait_media_ready ] && echo ready || echo absent)\"; "
        "passwd_root=$(awk '$5 == \"/etc/passwd\" {count++; root=$4} END {if (count == 1) print root}' /proc/self/mountinfo); "
        "root_ssh=absent; case $passwd_root in /root-ssh-provision/account-overlays/*/passwd) "
        "account=${passwd_root#/root-ssh-provision/account-overlays/}; account=${account%/passwd}; "
        "case $account in ''|*[!a-z0-9_-]*) ;; *) "
        "overlay=/var/lib/cbox/root-ssh-provision/account-overlays/$account; "
        "comment=\"# ls200-root-ssh-$account managed cron-bind\"; "
        "job=\"* * * * * $overlay/apply.sh >/dev/null 2>&1\"; "
        "if [ -f \"$overlay/passwd\" ] && [ ! -L \"$overlay/passwd\" ] && "
        "[ -f \"$overlay/shadow\" ] && [ ! -L \"$overlay/shadow\" ] && "
        "[ -x \"$overlay/apply.sh\" ] && [ ! -L \"$overlay/apply.sh\" ] && "
        "[ -f \"$overlay/.ls200-root-shell-managed\" ] && "
        "[ \"$(grep -Fxc managed-by=open-ls200-root-shell.sh \"$overlay/.ls200-root-shell-managed\")\" = 1 ] && "
        "[ \"$(grep -Fxc \"account=$account\" \"$overlay/.ls200-root-shell-managed\")\" = 1 ] && "
        "[ \"$(grep -Fxc \"$comment\" /var/lib/cbox/crontabs/root)\" = 1 ] && "
        "[ \"$(grep -Fxc \"$job\" /var/lib/cbox/crontabs/root)\" = 1 ] && "
        "grep -Fx \"passwd_source='$overlay/passwd'\" \"$overlay/apply.sh\" >/dev/null && "
        "cmp -s \"$overlay/passwd\" /etc/passwd; then root_ssh=ready; fi ;; esac ;; *) : ;; esac; "
        "printf 'root_ssh=%s\\n' \"$root_ssh\"; "
        "printf 'profile=%s\\n' \"$([ ! -e /var/lib/cbox/ls200-zoom ] && echo absent || echo present)\""
    )
    output = ssh(session, known, control, command).decode("ascii", "strict")
    values: dict[str, str] = {}
    for line in output.splitlines():
        key, separator, value = line.partition("=")
        if not separator or not re.fullmatch(r"[a-z_]+", key) or not re.fullmatch(r"[A-Za-z0-9._-]+", value):
            raise CampaignError("target baseline output was malformed")
        if key in values:
            raise CampaignError("target baseline output contained a duplicate key")
        values[key] = value
    expected = {
        "mounts", "crontab", "passwd", "group", "shadow", "passwd_meta", "group_meta",
        "shadow_meta", "firmware", "product_id", "os_release", "cpu_ticks",
        "fd_total", "sockets", "diskstats", "persistent_paths", "mem_available_kib",
        "free_kib", "jffs_kib", "vendor_media", "root_ssh", "profile",
    }
    if set(values) != expected:
        raise CampaignError("target baseline output was incomplete")
    return values


def _certificate(session: LiveSession) -> tuple[Path, Path]:
    directory = _secure_directory(_private_root(session) / "tls", create=True, label="ephemeral TLS directory")
    certificate = directory / "server.crt"
    key = directory / "server.key"
    present = (certificate.exists() or certificate.is_symlink(), key.exists() or key.is_symlink())
    if present.count(True) == 1:
        raise CampaignError("ephemeral TLS material is incomplete for this session")
    generated = not any(present)
    try:
        if generated:
            _run([
                "/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes", "-days", "2",
                "-subj", "/CN=console.invalid", "-addext", "subjectAltName=DNS:console.invalid",
                "-keyout", str(key), "-out", str(certificate),
            ])
            os.chmod(key, 0o600)
            os.chmod(certificate, 0o600)
        for path in (certificate, key):
            if path.is_symlink() or not path.is_file() or stat.S_IMODE(path.stat().st_mode) != 0o600:
                raise CampaignError("ephemeral TLS material is unsafe")
        check = _run(["/usr/bin/openssl", "x509", "-in", str(certificate), "-noout", "-text"]).stdout
        if b"DNS:console.invalid" not in check:
            raise CampaignError("ephemeral certificate SAN validation failed")
        cert_pub = _run(["/usr/bin/openssl", "x509", "-in", str(certificate), "-pubkey", "-noout"]).stdout
        key_pub = _run(["/usr/bin/openssl", "pkey", "-in", str(key), "-pubout"]).stdout
        if cert_pub != key_pub:
            raise CampaignError("ephemeral certificate and key do not match")
    except BaseException:
        if generated:
            for path in (key, certificate):
                with contextlib.suppress(FileNotFoundError):
                    path.unlink()
        raise
    return certificate, key


class _PinnedHTTPSConnection(http.client.HTTPSConnection):
    def __init__(self, session: LiveSession, certificate: Path) -> None:
        context = ssl.create_default_context(cafile=str(certificate))
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.check_hostname = True
        super().__init__("console.invalid", 8443, context=context, timeout=8.0)
        self._session = session
        pem = certificate.read_text(encoding="ascii")
        der = ssl.PEM_cert_to_DER_cert(pem)
        self._certificate_sha256 = hashlib.sha256(der).digest()

    def connect(self) -> None:
        plain = socket.create_connection(
            (self._session.target_ipv4, 8443), timeout=self.timeout,
            source_address=(self._session.host_ipv4, 0),
        )
        wrapped = self._context.wrap_socket(plain, server_hostname="console.invalid")
        peer = wrapped.getpeercert(binary_form=True)
        if not peer or not secrets.compare_digest(hashlib.sha256(peer).digest(), self._certificate_sha256):
            wrapped.close()
            raise ssl.SSLError("console presented a certificate other than the exact session pin")
        self.sock = wrapped


def _request_headers(
    method: str, encoded: bytes, cookie: str | None, csrf: str | None, host: str,
) -> dict[str, str]:
    headers = {
        "Accept": "application/json", "Content-Type": "application/json",
        "Host": host, "Origin": "https://" + host,
        "Content-Length": str(len(encoded)),
    }
    if method in {"POST", "PUT", "PATCH", "DELETE"}:
        headers["Idempotency-Key"] = str(uuid.uuid4())
    if cookie is not None:
        headers["Cookie"] = cookie
    if csrf is not None:
        headers["X-CSRF-Token"] = csrf
    return headers


def _validated_response(response: http.client.HTTPResponse) -> tuple[dict[str, Any], str | None]:
    if response.getheader("Location") is not None:
        raise CampaignError("console redirect was rejected")
    raw = response.read(65537)
    if len(raw) > 65536 or response.status < 200 or response.status >= 300:
        raise CampaignError("console request failed with redacted response")
    value = json.loads(raw)
    if (
        not isinstance(value, dict)
        or set(value) != {"revision", "ok", "data"}
        or type(value.get("revision")) is not int
        or value["revision"] != 1
        or value.get("ok") is not True
    ):
        raise CampaignError("console returned a malformed or failed envelope")
    data = value["data"]
    if not isinstance(data, dict):
        raise CampaignError("console returned a malformed data object")
    set_cookie = response.getheader("Set-Cookie")
    if set_cookie is not None:
        match = re.search(r"(?:^|;\s*)ls200_session=([0-9a-f]{64})(?:;|$)", set_cookie)
        if match is None:
            raise CampaignError("console returned an ambiguous session cookie")
        set_cookie = "ls200_session=" + match.group(1)
    return data, set_cookie


def _api(
    session: LiveSession, certificate: Path, method: str, path: str,
    body: dict[str, Any], *, cookie: str | None = None, csrf: str | None = None,
    connection_class: type[_PinnedHTTPSConnection] = _PinnedHTTPSConnection,
) -> tuple[dict[str, Any], str | None]:
    encoded = json.dumps(body, separators=(",", ":")).encode("utf-8")
    headers = _request_headers(method, encoded, cookie, csrf, f"{session.target_ipv4}:8443")
    connection = connection_class(session, certificate)
    try:
        connection.request(method, "/zoom/api/v1" + path, body=encoded, headers=headers)
        response = connection.getresponse()
        return _validated_response(response)
    except (OSError, ssl.SSLError, http.client.HTTPException, json.JSONDecodeError) as error:
        raise CampaignError("console HTTPS operation failed with redacted details") from error
    finally:
        connection.close()


def _temporary_console_password(prompt: str) -> bytearray:
    first = getpass.getpass(prompt)
    confirmation = getpass.getpass("Repeat the temporary console administrator password: ")
    try:
        encoded = first.encode("utf-8", "strict")
        confirmation_encoded = confirmation.encode("utf-8", "strict")
        if not secrets.compare_digest(encoded, confirmation_encoded):
            raise CampaignError("temporary console administrator passwords did not match")
        if not 12 <= len(encoded) <= 256 or b"\x00" in encoded:
            raise CampaignError("temporary console administrator password must be 12-256 UTF-8 bytes")
        return bytearray(encoded)
    finally:
        first = ""
        confirmation = ""


def _login_console(
    session: LiveSession, certificate: Path, password: bytearray,
    *, api: Callable[..., tuple[dict[str, Any], str | None]] = _api,
) -> tuple[str, str]:
    data, cookie = api(session, certificate, "POST", "/auth/login", {
        "username": "liveadmin", "password": password.decode("utf-8", "strict"),
    })
    csrf = data.get("csrf_token")
    if cookie is None or not isinstance(csrf, str) or not re.fullmatch(r"[0-9a-f]{64}", csrf):
        raise CampaignError("console login did not establish an exact session and CSRF token")
    if data.get("role") != 3:
        raise CampaignError("console administrator role was not established")
    return cookie, csrf


def _refresh_console_session(
    session: LiveSession, certificate: Path, cookie: str, csrf: str,
    *, api: Callable[..., tuple[dict[str, Any], str | None]] = _api,
) -> tuple[str, str]:
    data, rotated_cookie = api(
        session, certificate, "GET", "/auth/session", {}, cookie=cookie, csrf=csrf,
    )
    refreshed_csrf = data.get("csrf_token")
    if data.get("role") != 3 or not isinstance(refreshed_csrf, str) or not re.fullmatch(
        r"[0-9a-f]{64}", refreshed_csrf
    ):
        raise CampaignError("console session refresh did not establish an administrator CSRF token")
    return rotated_cookie or cookie, refreshed_csrf


def _confirm_reconnect(old_fingerprints: list[str], new_fingerprints: list[str]) -> None:
    print(f"old ECDSA fingerprints: {old_fingerprints}")
    print(f"new ECDSA fingerprints: {new_fingerprints}")
    if input("Type RECONNECT after visually comparing the ECDSA fingerprints: ") != "RECONNECT":
        raise CampaignError("operator declined the post-reboot reconnect")


def _establish_console(
    session: LiveSession, known: Path, control: Path, certificate: Path, password: bytearray,
    *, ssh: Callable[..., bytes] = _ssh,
    api: Callable[..., tuple[dict[str, Any], str | None]] = _api,
) -> tuple[str, str]:
    bootstrap = bytearray(
        ssh(session, known, control, "cat /var/lib/cbox/ls200-zoom/bootstrap-token", timeout=20.0).strip()
    )
    if not re.fullmatch(rb"[0-9a-f]{32}", bootstrap):
        raise CampaignError("bootstrap token was absent or malformed")
    try:
        try:
            api(session, certificate, "POST", "/auth/bootstrap", {
                "username": "liveadmin", "password": password.decode("utf-8", "strict"),
                "bootstrap_code": bootstrap.decode("ascii"),
            })
        except CampaignError:
            ssh(
                session, known, control,
                "set -eu; account=/var/lib/cbox/ls200-zoom/gateway/account.json; "
                "[ -f \"$account\" ] && [ ! -L \"$account\" ]; sync; "
                "/var/lib/cbox/ls200-zoom/live-start.sh stop; "
                "/var/lib/cbox/ls200-zoom/live-start.sh start; "
                "/var/lib/cbox/ls200-zoom/live-start.sh health",
                capture=False, timeout=180.0,
            )
            return _login_console(session, certificate, password, api=api)
    finally:
        for index in range(len(bootstrap)):
            bootstrap[index] = 0
    return _login_console(session, certificate, password, api=api)
