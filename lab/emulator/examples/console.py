#!/usr/bin/env python3
"""Run the real console UI/gateway against synthetic providers on loopback TLS."""
from __future__ import annotations

import argparse
import errno
import fcntl
import http.cookiejar
import json
import os
import secrets
import signal
import socket
import ssl
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import webbrowser
from collections.abc import Callable
from pathlib import Path
from typing import Any


class Stopping(Exception):
    """An owned launcher received its stop command or a termination signal."""


def private_directory(path: Path) -> None:
    for directory in reversed((path, *path.parents)):
        try:
            directory.mkdir(mode=0o700)
        except FileExistsError:
            pass
        info = directory.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid not in (0, os.getuid()) or info.st_mode & 0o022:
            raise ValueError(f"unsafe canonical runtime ancestor: {directory}")
    if path.stat().st_uid != os.getuid() or path.stat().st_mode & 0o077:
        raise ValueError(f"runtime directory must be owned and mode0700: {path}")


def private_file(path: Path) -> bytes:
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1
                or info.st_mode & 0o077 or info.st_size > 1024 * 1024):
            raise ValueError(f"expected owned private regular file: {path}")
        with os.fdopen(fd, "rb", closefd=False) as source:
            return source.read(1024 * 1024 + 1)
    finally:
        os.close(fd)


def write_private(path: Path, value: dict[str, Any]) -> None:
    if path.exists() or path.is_symlink():
        private_file(path)
    fd, temporary = tempfile.mkstemp(prefix=".console-", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(value, output, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


def socket_paths(root: Path) -> dict[str, Path]:
    directory = root / ".work/ec"
    paths = {"control": directory / "c", "gateway": directory / "g",
             "sipd": directory / "sipd.sock", "device": directory / "device.sock"}
    if any(len(os.fsencode(path)) >= 104 for path in paths.values()):
        raise ValueError("canonical checkout path is too long for Unix sockets; use a shorter checkout location")
    return paths


def remove_stale(path: Path, kind: int = socket.SOCK_STREAM) -> None:
    try:
        before = path.lstat()
    except FileNotFoundError:
        return
    if not stat.S_ISSOCK(before.st_mode) or before.st_uid != os.getuid() or before.st_nlink != 1:
        raise ValueError(f"refusing unexpected socket entry: {path}")
    with socket.socket(socket.AF_UNIX, kind) as probe:
        probe.settimeout(0.2)
        try:
            probe.connect(str(path))
        except OSError as error:
            if error.errno not in (errno.ECONNREFUSED, errno.ENOENT):
                raise ValueError(f"socket may still be active: {path}") from error
        else:
            raise ValueError(f"socket is still active: {path}")
    after = path.lstat()
    if (after.st_dev, after.st_ino) != (before.st_dev, before.st_ino):
        raise ValueError("socket changed during stale cleanup")
    path.unlink()


def nginx_quote(value: Path | str) -> str:
    text = str(value)
    if "\n" in text or "\r" in text or "\0" in text:
        raise ValueError("nginx paths cannot contain line breaks or NUL")
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$") + '"'


def nginx_config(root: Path, runtime: Path, gateway: Path, port: int) -> str:
    ui = root / ".work/dist/emulator-web"
    return f'''worker_processes 1;
pid {nginx_quote(runtime / 'nginx.pid')};
error_log {nginx_quote(runtime / 'nginx-error.log')} info;
events {{ worker_connections 128; }}
http {{
 include {nginx_quote(root / '.work/build/nginx-host/prefix/conf/mime.types')};
 default_type application/octet-stream;
 access_log {nginx_quote(runtime / 'nginx-access.log')};
 server_tokens off;
 client_max_body_size 1k;
 client_body_temp_path {nginx_quote(runtime / 'body')};
 fastcgi_temp_path {nginx_quote(runtime / 'fastcgi')};
 client_body_timeout 5s;
 client_header_timeout 5s;
 send_timeout 10s;
 keepalive_timeout 10s;
 server {{
  listen 127.0.0.1:{port} ssl;
  server_name 127.0.0.1;
  ssl_certificate {nginx_quote(runtime / 'server.crt')};
  ssl_certificate_key {nginx_quote(runtime / 'server.key')};
  ssl_protocols TLSv1.2 TLSv1.3;
  ssl_session_cache off;
  ssl_session_tickets off;
  add_header X-Content-Type-Options nosniff always;
  add_header Referrer-Policy no-referrer always;
  add_header Content-Security-Policy "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; media-src 'self' blob:; font-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'" always;
  location /zoom/api/v1/ {{
   fastcgi_pass {nginx_quote('unix:' + str(gateway))};
   fastcgi_pass_request_headers off;
   fastcgi_param REQUEST_METHOD $request_method;
   fastcgi_param DOCUMENT_URI $uri;
   fastcgi_param REQUEST_URI $request_uri;
   fastcgi_param QUERY_STRING $query_string;
   fastcgi_param CONTENT_TYPE $content_type;
   fastcgi_param CONTENT_LENGTH $content_length;
   fastcgi_param SERVER_ADDR $server_addr;
   fastcgi_param SERVER_PORT $server_port;
   fastcgi_param AULA_CLIENT_ID $remote_addr;
   fastcgi_param HTTP_ORIGIN $http_origin if_not_empty;
   fastcgi_param HTTP_SEC_FETCH_SITE $http_sec_fetch_site if_not_empty;
   fastcgi_param HTTP_COOKIE $http_cookie if_not_empty;
   fastcgi_param HTTP_X_CSRF_TOKEN $http_x_csrf_token if_not_empty;
   fastcgi_param HTTP_IDEMPOTENCY_KEY $http_idempotency_key if_not_empty;
   fastcgi_read_timeout 10s;
  }}
  location /zoom/ {{ alias {nginx_quote(str(ui) + '/')}; index index.html; }}
  location / {{ deny all; }}
 }}
}}
'''


class Api:
    def __init__(self, port: int, certificate: Path) -> None:
        self.origin = f"https://127.0.0.1:{port}"
        self.csrf = ""
        context = ssl.create_default_context(cafile=str(certificate))
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
            urllib.request.HTTPSHandler(context=context), urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def request(self, method: str, route: str, body: dict[str, Any] | None = None) -> tuple[int, dict[str, Any]]:
        headers = {"Origin": self.origin, "Sec-Fetch-Site": "same-origin"}
        data = None
        if body is not None:
            data = json.dumps(body, separators=(",", ":")).encode()
            headers["Content-Type"] = "application/json"
            headers["X-CSRF-Token"] = self.csrf
            headers["Idempotency-Key"] = "demo-" + secrets.token_hex(16)
        request = urllib.request.Request(self.origin + "/zoom/api/v1" + route,
                                         method=method, data=data, headers=headers)
        try:
            response = self.opener.open(request, timeout=2)
        except urllib.error.HTTPError as error:
            response = error
        with response:
            raw = response.read(16385)
            if len(raw) > 16384:
                raise ValueError("gateway reply exceeded its bounded response size")
            result = json.loads(raw)
            if not isinstance(result, dict):
                raise TypeError("gateway reply is not an object")
            status = response.getcode()
            if not isinstance(status, int):
                raise TypeError("gateway returned no HTTP status")
            return status, result

    def success(self, method: str, route: str, body: dict[str, Any] | None = None) -> dict[str, Any]:
        status, response = self.request(method, route, body)
        if not 200 <= status < 300 or response.get("ok") is not True:
            code = response.get("error", {}).get("code", "INVALID_RESPONSE")
            raise RuntimeError(f"gateway {method} {route} returned {status} {code}")
        data = response.get("data")
        if not isinstance(data, dict):
            raise TypeError("gateway success projection is not an object")
        return data


def provision(api: Api, credentials: dict[str, Any], bootstrap: bool,
              ready: Callable[[], None] | None = None) -> None:
    if bootstrap:
        api.success("POST", "/auth/bootstrap", {key: credentials[key] for key in ("username", "password", "bootstrap_code")})
    login = api.success("POST", "/auth/login", {key: credentials[key] for key in ("username", "password")})
    api.csrf = login["csrf_token"]
    directory = api.success("GET", "/directory")
    if not any(entry.get("id") == "synthetic-room" for entry in directory["entries"]):
        api.success("POST", "/directory", {"id": "synthetic-room", "name": "Synthetic room", "meeting_id": "123456789",
                    "profile": "private_lab", "default_layout": "full_screen", "revision": directory["revision"]})
    if ready is None:
        api.success("GET", "/status")
        api.success("GET", "/device/status")
    else:
        ready()
    api.success("POST", "/auth/logout", {})


class Supervisor:
    def __init__(self, root: Path, port: int) -> None:
        self.root, self.port = root, port
        self.runtime = root / ".work/console-local"
        self.paths = socket_paths(root)
        self.children: list[tuple[str, subprocess.Popen[bytes]]] = []
        self.stopping = False
        self.server: socket.socket | None = None
        self.lock_fd = -1
        self.phase = "starting"
        self.owned: dict[Path, tuple[int, int]] = {}

    def status(self) -> dict[str, Any]:
        return {"phase": self.phase, "url": f"https://127.0.0.1:{self.port}/zoom/", "pid": os.getpid(),
                "credentials_path": str(self.runtime / "credentials.json"), "logs": str(self.runtime),
                "activity_state": str(self.paths["sipd"].parent / "activity.json")}

    def record(self, phase: str) -> None:
        self.phase = phase
        write_private(self.runtime / "status.json", self.status())

    def tick(self, delay: float = 0.05) -> None:
        if self.stopping:
            raise Stopping
        if self.server is not None:
            try:
                connection, _ = self.server.accept()
            except BlockingIOError:
                pass
            else:
                with connection:
                    connection.settimeout(0.2)
                    try:
                        request = json.loads(connection.recv(1025))
                        if request == {"action": "stop"}:
                            self.stopping = True
                        elif request != {"action": "status"}:
                            raise ValueError("unsupported control request")
                        connection.sendall(json.dumps(self.status()).encode() + b"\n")
                    except (OSError, ValueError):
                        pass
        if self.stopping:
            raise Stopping
        time.sleep(delay)

    def spawn(self, name: str, command: list[str], environment: dict[str, str] | None = None) -> subprocess.Popen[bytes]:
        log = self.runtime / (name + ".log")
        if log.exists() or log.is_symlink():
            private_file(log)
        with log.open("ab") as stream:
            log.chmod(0o600)
            process = subprocess.Popen(command, cwd=self.root, env=environment, stdin=subprocess.DEVNULL,
                                       stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        self.children.append((name, process))
        return process

    def command(self, name: str, command: list[str]) -> None:
        process = self.spawn(name, command)
        while process.poll() is None:
            self.tick()
        if process.returncode != 0:
            raise RuntimeError(f"{name} failed; inspect {self.runtime / (name + '.log')}")
        self.children.remove((name, process))

    def claim_socket(self, path: Path) -> None:
        info = path.lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
            raise ValueError(f"expected owned listener socket: {path}")
        self.owned[path] = (info.st_dev, info.st_ino)

    def start_control(self) -> None:
        private_directory(self.runtime)
        private_directory(self.paths["control"].parent)
        lock = self.runtime / "supervisor.lock"
        self.lock_fd = os.open(lock, os.O_RDWR | os.O_CREAT | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0), 0o600)
        info = os.fstat(self.lock_fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1 or info.st_mode & 0o077:
            raise ValueError("launcher lock must be an owned private regular file")
        try:
            fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("this checkout already has an active console launcher; use status/stop") from error
        for name, path in self.paths.items():
            kind = socket.SOCK_SEQPACKET if name == "sipd" and sys.platform.startswith("linux") else socket.SOCK_STREAM
            remove_stale(path, kind)
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(str(self.paths["control"]))
        self.paths["control"].chmod(0o600)
        self.claim_socket(self.paths["control"])
        self.server.listen(8)
        self.server.setblocking(False)
        self.record("preparing")

    def build(self) -> None:
        fcgi = self.root / ".work/build/fastcgi/prefix"
        self.command("build-fastcgi", ["sh", "dependencies/scripts/build-fastcgi-host.sh"])
        self.command("build-nginx", ["sh", "dependencies/scripts/build-nginx-host.sh"])
        self.command("build-gateway", ["make", "-C", "product/console", "fastcgi", "FCGI_PREFIX=" + str(fcgi)])
        self.command("ui-install", ["make", "-C", "product/console", "ui-install"])
        self.command("ui-types", ["sh", "tooling/console/with-dependencies.sh", "tsc", "-b"])
        self.command("ui-build", ["sh", "tooling/console/with-dependencies.sh", "vite", "build", "--mode", "emulator",
                                  "--outDir", str(self.root / ".work/dist/emulator-web"), "--emptyOutDir"])

    def wait_for_providers(self, api: Api) -> None:
        deadline = time.monotonic() + 15
        while True:
            for name, child in self.children:
                if child.poll() is not None:
                    raise RuntimeError(f"{name} exited before provider readiness; inspect its log")
            try:
                api.success("GET", "/status")
                api.success("GET", "/device/status")
                return
            except (OSError, ValueError, RuntimeError, urllib.error.URLError):
                if time.monotonic() >= deadline:
                    raise RuntimeError("authenticated provider readiness timed out; inspect provider logs") from None
            self.tick(0.1)

    def credentials(self) -> dict[str, Any]:
        path = self.runtime / "credentials.json"
        if path.exists() or path.is_symlink():
            value = json.loads(private_file(path))
            if not isinstance(value, dict) or any(not isinstance(value.get(key), str) or not value[key]
                                                for key in ("username", "password", "bootstrap_code")):
                raise ValueError("saved launcher credentials are invalid")
            return value
        if (self.runtime / "account.json").exists():
            raise ValueError("saved account exists without launcher credentials; restore credentials.json")
        value = {"username": "admin", "password": "Aula-" + secrets.token_urlsafe(24) + "-7",
                 "bootstrap_code": secrets.token_urlsafe(24), "url": self.status()["url"]}
        write_private(path, value)
        return value

    def certificate(self) -> None:
        certificate, key = self.runtime / "server.crt", self.runtime / "server.key"
        if certificate.exists() and key.exists():
            private_file(certificate)
            private_file(key)
            return
        if certificate.is_symlink() or key.is_symlink():
            raise ValueError("TLS files cannot be symlinks")
        with tempfile.TemporaryDirectory(prefix="tls-", dir=self.runtime) as name:
            pending = Path(name)
            self.command("tls", ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "365",
                          "-subj", "/CN=127.0.0.1", "-addext", "subjectAltName=IP:127.0.0.1,DNS:localhost",
                          "-keyout", str(pending / "key"), "-out", str(pending / "crt")])
            for source, target in ((pending / "key", key), (pending / "crt", certificate)):
                source.chmod(0o600)
                os.replace(source, target)

    def policy(self, credentials: dict[str, Any]) -> dict[str, Any]:
        result = {"allowed_origin": f"https://127.0.0.1:{self.port}", "control_socket_path": str(self.paths["sipd"]),
                  "expected_sipd_uid": os.getuid(), "account_store_path": str(self.runtime / "account.json"),
                  "device_socket_path": str(self.paths["device"]), "expected_device_uid": os.getuid(),
                  "development_device_provider": True, "preview_enabled": False,
                  "preview_rtsp_ipv4": "127.0.0.1", "preview_rtsp_port": 554}
        if not (self.runtime / "account.json").exists():
            result["bootstrap_code"] = credentials["bootstrap_code"]
        return result

    def start(self, no_build: bool, open_browser: bool) -> int:
        try:
            self.start_control()
            if not no_build:
                self.build()
            gateway = self.root / ".work/build/console/aula-gateway-fcgi"
            nginx = self.root / ".work/build/nginx-host/prefix/sbin/nginx"
            for artifact in (gateway, nginx, self.root / ".work/dist/emulator-web/index.html"):
                if not artifact.is_file():
                    raise ValueError(f"required built artifact is missing: {artifact}; run start without --no-build")
            credentials = self.credentials()
            self.certificate()
            policy = self.policy(credentials)
            policy_path = self.runtime / "gateway.json"
            write_private(policy_path, policy)
            config = self.runtime / "nginx.conf"
            if config.exists() or config.is_symlink():
                private_file(config)
            config.write_text(nginx_config(self.root, self.runtime, self.paths["gateway"], self.port))
            config.chmod(0o600)
            for name in ("body", "fastcgi", "logs"):
                private_directory(self.runtime / name)
            self.command("nginx-check", [str(nginx), "-p", str(self.runtime) + "/", "-c", str(config), "-t"])
            self.record("starting")
            env = dict(os.environ, PYTHONPATH=str(self.root / "lab/emulator/src"), PYTHONUNBUFFERED="1")
            self.spawn("providers", [sys.executable, str(self.root / "lab/emulator/examples/host.py"),
                                     "--work-root", str(self.paths["sipd"].parent)], env)
            self.spawn("gateway", [str(gateway), "--policy", str(policy_path), "--listen", str(self.paths["gateway"])])
            self.spawn("nginx", [str(nginx), "-p", str(self.runtime) + "/", "-c", str(config),
                                 "-g", "daemon off; master_process off;"])
            api = Api(self.port, self.runtime / "server.crt")
            deadline = time.monotonic() + 20
            while True:
                for name, child in self.children:
                    if child.poll() is not None:
                        raise RuntimeError(f"{name} exited during startup; inspect {self.runtime / (name + '.log')}")
                try:
                    status, reply = api.request("GET", "/auth/session")
                    if status in (200, 401) and reply.get("revision") == 1:
                        break
                except (OSError, ValueError, urllib.error.URLError):
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError("gateway readiness timed out; inspect nginx/gateway/provider logs")
                self.tick(0.1)
            deadline = time.monotonic() + 15
            pending = {"sipd", "device", "gateway"}
            while pending:
                for name, child in self.children:
                    if child.poll() is not None:
                        raise RuntimeError(f"{name} exited before provider readiness; inspect its log")
                for name in list(pending):
                    try:
                        self.claim_socket(self.paths[name])
                    except FileNotFoundError:
                        continue
                    pending.remove(name)
                if pending:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("provider socket readiness timed out")
                    self.tick()

            provision(api, credentials, "bootstrap_code" in policy, lambda: self.wait_for_providers(api))
            policy.pop("bootstrap_code", None)
            write_private(policy_path, policy)
            self.record("ready")
            print(json.dumps(self.status(), indent=2), flush=True)
            print("Open the URL and sign in using credentials.json. Accept the local self-signed certificate once.", flush=True)
            if open_browser:
                webbrowser.open(self.status()["url"])
            while True:
                for name, child in self.children:
                    if child.poll() is not None:
                        raise RuntimeError(f"{name} exited; inspect {self.runtime / (name + '.log')}")
                self.tick()
        except Stopping:
            return 0
        finally:
            self.cleanup()

    def cleanup(self) -> None:
        groups = {process.pid for _name, process in self.children}
        for group in groups:
            try:
                os.killpg(group, signal.SIGTERM)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 4
        pending = set(groups)
        while pending:
            for _name, process in self.children:
                process.poll()  # Reap leaders before checking their group.
            for group in list(pending):
                try:
                    os.killpg(group, 0)
                except ProcessLookupError:
                    pending.remove(group)
            if not pending or time.monotonic() >= deadline:
                break
            time.sleep(0.05)
        for group in pending:
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        for _name, process in self.children:
            process.wait(timeout=2)
        if self.server is not None:
            self.server.close()
        for path, identity in self.owned.items():
            try:
                info = path.lstat()
                if stat.S_ISSOCK(info.st_mode) and (info.st_dev, info.st_ino) == identity:
                    path.unlink()
            except FileNotFoundError:
                pass
        if self.lock_fd >= 0:
            # Only the successful lock owner writes status; a failed second
            # launcher must not overwrite the active owner's readiness.
            if self.server is not None:
                self.record("stopped")
            os.close(self.lock_fd)
            self.lock_fd = -1


def control(root: Path, action: str) -> dict[str, Any]:
    paths = socket_paths(root)
    lock = root / ".work/console-local/supervisor.lock"
    if not lock.exists():
        return {"phase": "stopped"}
    private_file(lock)
    fd = os.open(lock, os.O_RDWR | os.O_NONBLOCK | getattr(os, "O_NOFOLLOW", 0))
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            pass
        else:
            return {"phase": "stopped"}
        private_directory(paths["control"].parent)
        info = paths["control"].lstat()
        if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
            raise ValueError("launcher control path is not an owned socket")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(2)
            client.connect(str(paths["control"]))
            client.sendall(json.dumps({"action": action}).encode())
            data = client.recv(8193)
            if len(data) > 8192:
                raise ValueError("launcher control response is too large")
            result = json.loads(data)
        if action == "stop":
            deadline = time.monotonic() + 12
            while True:
                try:
                    fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    pass
                else:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("owned launcher has not completed shutdown; inspect its logs")
                time.sleep(0.05)
            result["phase"] = "stopped"
        return result
    finally:
        os.close(fd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=("start", "status", "stop"), default="start")
    parser.add_argument("--port", type=int, default=8443)
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--open", action="store_true")
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    if not 1024 <= args.port <= 65535:
        parser.error("--port must be between 1024 and 65535")
    root = args.repo_root.resolve(strict=True)
    if not (root / "product/console/Makefile").is_file() or not (root / "lab/emulator/examples/host.py").is_file():
        parser.error("--repo-root must identify the source checkout")
    if args.action != "start":
        print(json.dumps(control(root, args.action), indent=2))
        return 0
    supervisor = Supervisor(root, args.port)

    def stop_requested(_signal: int, _frame: object) -> None:
        supervisor.stopping = True

    signal.signal(signal.SIGTERM, stop_requested)
    signal.signal(signal.SIGINT, stop_requested)
    return supervisor.start(args.no_build, args.open)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, TypeError, RuntimeError, KeyError, urllib.error.URLError) as error:
        print(f"console launcher: {error}", file=sys.stderr)
        raise SystemExit(1) from error
