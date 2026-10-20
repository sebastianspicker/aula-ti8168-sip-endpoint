"""Launcher contracts without downloads, privileged paths, or listening sockets."""
from __future__ import annotations

import errno
import importlib.util
import json
import os
import signal
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

SOURCE = Path(__file__).resolve().parents[1] / "examples/console.py"
spec = importlib.util.spec_from_file_location("aula_console_launcher", SOURCE)
assert spec is not None and spec.loader is not None
launcher = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = launcher
spec.loader.exec_module(launcher)


def socket_info(inode: int = 10) -> SimpleNamespace:
    return SimpleNamespace(st_mode=stat.S_IFSOCK | 0o600, st_uid=os.getuid(), st_nlink=1, st_dev=1, st_ino=inode)


class LauncherPathsTests(unittest.TestCase):
    def test_private_credentials_atomic_permissions_and_symlink_rejection(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            path = root / "credentials.json"
            launcher.write_private(path, {"username": "admin", "password": "fixture"})
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(json.loads(launcher.private_file(path))["password"], "fixture")
            alias = root / "alias"
            alias.symlink_to(path)
            with self.assertRaises(OSError):
                launcher.write_private(alias, {"password": "changed"})
            self.assertEqual(json.loads(path.read_text())["password"], "fixture")

    def test_fifo_credentials_rejected_without_blocking(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            fifo = Path(name) / "fifo"
            os.mkfifo(fifo)
            code = ("import importlib.util,sys;from pathlib import Path;"
                    "s=importlib.util.spec_from_file_location('launcher',sys.argv[1]);"
                    "m=importlib.util.module_from_spec(s);s.loader.exec_module(m);"
                    "m.private_file(Path(sys.argv[2]))")
            result = subprocess.run([sys.executable, "-c", code, str(SOURCE), str(fifo)],
                                    capture_output=True, check=False, timeout=2)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"expected owned private regular file", result.stderr)

    def test_socket_paths_are_below_work_and_long_paths_fail_early(self) -> None:
        root = Path("/home/user/project")
        paths = launcher.socket_paths(root)
        self.assertTrue(all(path.is_relative_to(root / ".work") for path in paths.values()))
        with self.assertRaisesRegex(ValueError, "too long"):
            launcher.socket_paths(Path("/" + "x" * 100))

    def test_stale_cleanup_removes_only_owned_refused_unchanged_socket(self) -> None:
        path = Mock(spec=Path)
        path.lstat.side_effect = [socket_info(), socket_info()]
        connection = Mock()
        connection.connect.side_effect = ConnectionRefusedError(errno.ECONNREFUSED, "fixture")
        context = Mock()
        context.__enter__ = Mock(return_value=connection)
        context.__exit__ = Mock(return_value=False)
        with patch.object(launcher.socket, "socket", return_value=context):
            launcher.remove_stale(path)
        path.unlink.assert_called_once_with()

    def test_active_or_replaced_sockets_are_preserved(self) -> None:
        path = Mock(spec=Path)
        path.lstat.return_value = socket_info()
        context = Mock()
        context.__enter__ = Mock(return_value=Mock())
        context.__exit__ = Mock(return_value=False)
        with patch.object(launcher.socket, "socket", return_value=context), self.assertRaises(ValueError):
            launcher.remove_stale(path)
        path.unlink.assert_not_called()
        path.lstat.side_effect = [socket_info(), socket_info(11)]
        context.__enter__.return_value.connect.side_effect = ConnectionRefusedError(errno.ECONNREFUSED, "fixture")
        with patch.object(launcher.socket, "socket", return_value=context), self.assertRaises(ValueError):
            launcher.remove_stale(path)
        path.unlink.assert_not_called()


class LauncherConfigurationTests(unittest.TestCase):
    def test_nginx_uses_loopback_tls_and_real_gateway_without_rewrite_module(self) -> None:
        root = Path("/home/user/project")
        config = launcher.nginx_config(root, root / ".work/console-local", root / ".work/ec/g", 8443)
        self.assertIn("listen 127.0.0.1:8443 ssl;", config)
        self.assertNotIn("listen 8443", config)
        self.assertIn("ssl_protocols TLSv1.2 TLSv1.3;", config)
        self.assertIn(".work/dist/emulator-web/", config)
        self.assertIn("fastcgi_pass_request_headers off;", config)
        self.assertIn("fastcgi_param SERVER_ADDR $server_addr;", config)
        self.assertIn("fastcgi_param HTTP_X_CSRF_TOKEN $http_x_csrf_token if_not_empty;", config)
        self.assertIn("fastcgi_param HTTP_ORIGIN $http_origin if_not_empty;", config)
        self.assertIn("fastcgi_param DOCUMENT_URI $uri;", config)
        self.assertIn("fastcgi_param REQUEST_URI $request_uri;", config)
        self.assertIn("location / { deny all; }", config)
        for unsupported in ("rewrite ", "return ", "location ~", "Strict-Transport-Security"):
            self.assertNotIn(unsupported, config)

    def test_nginx_path_quoting_and_invalid_newline(self) -> None:
        self.assertEqual(launcher.nginx_quote('/a path/$value'), '"/a path/\\$value"')
        with self.assertRaises(ValueError):
            launcher.nginx_quote("/path\nnext")

    def test_device_development_policy_is_explicit_and_bootstrap_is_one_time(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            supervisor = launcher.Supervisor(Path(name), 8443)
            supervisor.runtime.mkdir(parents=True)
            credentials = {"bootstrap_code": "fixture-code"}
            policy = supervisor.policy(credentials)
            self.assertEqual(policy["expected_device_uid"], os.getuid())
            self.assertTrue(policy["development_device_provider"])
            self.assertEqual(policy["allowed_origin"], "https://127.0.0.1:8443")
            self.assertFalse(policy["preview_enabled"])
            self.assertEqual(policy["bootstrap_code"], "fixture-code")
            (supervisor.runtime / "account.json").write_text("{}")
            self.assertNotIn("bootstrap_code", supervisor.policy(credentials))

    def test_credentials_persist_and_status_never_contains_password(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            supervisor = launcher.Supervisor(Path(name), 8443)
            supervisor.runtime.mkdir(parents=True)
            first = supervisor.credentials()
            self.assertEqual(first, supervisor.credentials())
            self.assertNotIn(first["password"], json.dumps(supervisor.status()))
            self.assertNotIn("password", supervisor.status())
            self.assertEqual(stat.S_IMODE((supervisor.runtime / "credentials.json").stat().st_mode), 0o600)


class LauncherProvisionAndCleanupTests(unittest.TestCase):
    def test_provision_uses_real_bootstrap_login_csrf_directory_and_logout(self) -> None:
        api = Mock()
        api.success.side_effect = [
            {"bootstrapped": True}, {"csrf_token": "fixture-csrf"}, {"entries": [], "revision": 7},
            {"id": "synthetic-room"}, {}, {}, {},
        ]
        credentials = {"username": "admin", "password": "fixture", "bootstrap_code": "bootstrap-fixture"}
        launcher.provision(api, credentials, True)
        calls = api.success.call_args_list
        self.assertEqual(calls[0].args, ("POST", "/auth/bootstrap", credentials))
        self.assertEqual(calls[1].args[1], "/auth/login")
        self.assertEqual(api.csrf, "fixture-csrf")
        self.assertEqual(calls[3].args[2]["profile"], "private_lab")
        self.assertEqual(calls[3].args[2]["revision"], 7)
        self.assertEqual(calls[-1].args, ("POST", "/auth/logout", {}))

    def test_delayed_provider_requires_both_authenticated_status_responses(self) -> None:
        supervisor = launcher.Supervisor(Path("/home/user/project"), 8443)
        child = Mock()
        child.poll.return_value = None
        supervisor.children = [("providers", child)]
        api = Mock()
        # First the SIP provider is absent; then only device is absent.
        api.success.side_effect = [RuntimeError("SIPD_UNAVAILABLE"), {},
                                   RuntimeError("DEVICE_UNAVAILABLE"), {}, {}]
        with patch.object(supervisor, "tick") as tick:
            supervisor.wait_for_providers(api)
        self.assertEqual(tick.call_count, 2)
        self.assertEqual([call.args[1] for call in api.success.call_args_list],
                         ["/status", "/status", "/device/status", "/status", "/device/status"])

    def test_provider_readiness_timeout_and_child_exit_never_claim_success(self) -> None:
        supervisor = launcher.Supervisor(Path("/home/user/project"), 8443)
        api = Mock()
        api.success.side_effect = RuntimeError("DEVICE_UNAVAILABLE")
        with patch.object(launcher.time, "monotonic", side_effect=[0, 16]), self.assertRaisesRegex(
                RuntimeError, "readiness timed out"):
            supervisor.wait_for_providers(api)
        child = Mock()
        child.poll.return_value = 1
        supervisor.children = [("providers", child)]
        api.reset_mock()
        with self.assertRaisesRegex(RuntimeError, "exited before provider readiness"):
            supervisor.wait_for_providers(api)
        api.success.assert_not_called()

    def test_default_build_refreshes_existing_fastcgi_archive(self) -> None:
        supervisor = launcher.Supervisor(Path("/home/user/project"), 8443)
        with patch.object(supervisor, "command") as command:
            supervisor.build()
        self.assertEqual(command.call_args_list[0].args,
                         ("build-fastcgi", ["sh", "dependencies/scripts/build-fastcgi-host.sh"]))

    def test_stop_waits_for_supervisor_lock_release(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            runtime = root / ".work/console-local"
            runtime.mkdir(parents=True)
            launcher.write_private(runtime / "supervisor.lock", {})
            connection = Mock()
            connection.recv.return_value = b'{"phase":"ready"}'
            context = Mock()
            context.__enter__ = Mock(return_value=connection)
            context.__exit__ = Mock(return_value=False)
            with patch.object(launcher, "private_directory"), patch.object(Path, "lstat", return_value=socket_info()), \
                    patch.object(launcher, "private_file", return_value=b""), \
                    patch.object(launcher.socket, "socket", return_value=context), \
                    patch.object(launcher.fcntl, "flock", side_effect=[BlockingIOError, BlockingIOError, None]) as flock, \
                    patch.object(launcher.time, "sleep") as sleep:
                result = launcher.control(root, "stop")
            self.assertEqual(result["phase"], "stopped")
            self.assertEqual(flock.call_count, 3)
            sleep.assert_called_once_with(0.05)

    def test_cleanup_signals_owned_groups_even_when_leader_has_exited(self) -> None:
        supervisor = launcher.Supervisor(Path("/home/user/project"), 8443)
        child = Mock()
        child.pid = 12345
        child.poll.return_value = 1
        supervisor.children = [("providers", child)]

        def signal_group(_group: int, sig: int) -> None:
            if sig == 0:
                raise ProcessLookupError

        with patch.object(launcher.os, "killpg", side_effect=signal_group) as kill:
            supervisor.cleanup()
        kill.assert_any_call(12345, signal.SIGTERM)
        child.wait.assert_called_once_with(timeout=2)

    def test_cleanup_kills_remaining_group_after_global_deadline(self) -> None:
        supervisor = launcher.Supervisor(Path("/home/user/project"), 8443)
        child = Mock()
        child.pid = 12345
        child.poll.return_value = 1
        supervisor.children = [("providers", child)]
        with patch.object(launcher.os, "killpg") as kill, patch.object(launcher.time, "monotonic", side_effect=[0, 5]):
            supervisor.cleanup()
        kill.assert_any_call(12345, signal.SIGKILL)

    def test_cleanup_does_not_remove_replaced_socket(self) -> None:
        supervisor = launcher.Supervisor(Path("/home/user/project"), 8443)
        path = Mock(spec=Path)
        path.lstat.return_value = socket_info(99)
        supervisor.owned[path] = (1, 10)
        supervisor.cleanup()
        path.unlink.assert_not_called()

    def test_status_without_owned_lock_is_stopped_and_does_not_signal(self) -> None:
        with tempfile.TemporaryDirectory() as name, patch.object(launcher.os, "killpg") as kill:
            self.assertEqual(launcher.control(Path(name), "stop"), {"phase": "stopped"})
        kill.assert_not_called()


if __name__ == "__main__":
    unittest.main()
