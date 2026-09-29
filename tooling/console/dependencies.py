#!/usr/bin/env python3
"""Coordinate cached console installations and concurrent dependency users."""
import hashlib
import json
import os
from pathlib import Path
import signal
import stat
import subprocess
import sys

from dependency_leases import BusyError, Leases
from dependency_cleanup import sync_directory, trash_work, write_all

MANIFESTS = ("package.json", "pnpm-lock.yaml", "pnpm-workspace.yaml")


class Dependencies:
    def __init__(self, root: Path):
        self.root = root.resolve()
        root = self.root
        self.web = root / "product/console/web"
        self.project = root / ".work/cache/console-project"
        self.modules = self.project / "node_modules"
        self.store = root / ".work/cache/pnpm-store"
        self.link = self.web / "node_modules"
        self.target = "../../../.work/cache/console-project/node_modules"
        self.ready = self.project / ".installation-ready.json"
        self.validate_work_paths()
        self.leases = Leases(root / ".work/locks/console")
        self.validate_work_paths()

    def validate_work_paths(self):
        for relative in (".work", ".work/cache", ".work/cache/console-project",
                         ".work/cache/console-project/node_modules", ".work/cache/pnpm-store",
                         ".work/locks", ".work/locks/console"):
            path = self.root / relative
            try:
                info = path.lstat()
            except FileNotFoundError:
                continue
            if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid()
                    or info.st_mode & 0o022):
                raise RuntimeError(
                    f"console work path must be an owned real directory without group/world write access: {path}")

    @staticmethod
    def replace_generated_file(path, contents):
        temporary = path.parent / f".{path.name}.pending"
        descriptor = os.open(temporary,
                             os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            try:
                write_all(descriptor, contents)
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            os.replace(temporary, path)
            sync_directory(path.parent)
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise

    def fingerprint(self):
        return self.snapshot_fingerprint(self.manifest_snapshot())

    def manifest_snapshot(self):
        return {name: (self.web / name).read_bytes() for name in MANIFESTS}

    @staticmethod
    def snapshot_fingerprint(snapshot):
        digest = hashlib.sha256()
        for name in MANIFESTS:
            digest.update(name.encode())
            digest.update(snapshot[name])
        return digest.hexdigest()

    def readiness(self):
        paths = [self.modules / name for name in (".modules.yaml", ".pnpm/lock.yaml")]
        manifest = json.loads((self.web / "package.json").read_text())
        packages = set(manifest.get("dependencies", {})) | set(manifest.get("devDependencies", {}))
        paths.extend(self.modules / name / "package.json" for name in sorted(packages))
        binaries = [self.modules / ".bin" / name for name in ("tsc", "vite", "vitest")]
        if not all(path.is_file() for path in paths + binaries):
            return None
        return {str(path.relative_to(self.modules)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in paths + binaries}

    def valid(self):
        try:
            record = json.loads(self.ready.read_text())
            return (record["fingerprint"] == self.fingerprint() and
                    record["readiness"] is not None and record["readiness"] == self.readiness())
        except (OSError, ValueError, KeyError):
            return False

    def check_link(self):
        if self.link.exists() or self.link.is_symlink():
            if not self.link.is_symlink() or os.readlink(self.link) != self.target:
                raise RuntimeError(f"refusing unexpected dependency path: {self.link}")

    def remove_link(self):
        self.check_link()
        if self.link.is_symlink():
            self.link.unlink()

    def install(self):
        self.validate_work_paths()
        with self.leases.metadata():
            self.leases.prune()
            self.check_link()
            if self.valid():
                return 0
            if os.environ.get('AULA_CONSOLE_REQUIRE_READY') == '1':
                raise RuntimeError('reviewed console dependencies must be prepared before this build')
            lock = self.leases.acquire(exclusive=True)
            self.remove_link()
        with lock:
            self.project.mkdir(parents=True, exist_ok=True)
            self.store.mkdir(parents=True, exist_ok=True)
            self.validate_work_paths()
            self.ready.unlink(missing_ok=True)
            snapshot = self.manifest_snapshot()
            fingerprint = self.snapshot_fingerprint(snapshot)
            for name in MANIFESTS:
                self.replace_generated_file(self.project / name, snapshot[name])
            result = supervise(["pnpm", "--dir", str(self.project), "install", "--frozen-lockfile",
                                "--store-dir", str(self.store)], self.web, (lock.fileno(),))
            if result:
                return result
            readiness = self.readiness()
            if readiness is None or snapshot != self.manifest_snapshot():
                raise RuntimeError("dependency installation is incomplete or manifests changed during installation")
            self.replace_generated_file(
                self.ready, json.dumps({"fingerprint": fingerprint, "readiness": readiness}).encode())
        return 0

    def clean(self):
        self.validate_work_paths()
        with self.leases.metadata(), self.leases.acquire(exclusive=True):
            self.leases.prune()
            self.remove_link()
        return 0

    def command(self, arguments):
        self.validate_work_paths()
        with self.leases.metadata():
            self.leases.prune()
            lock = self.leases.acquire(exclusive=False)
            try:
                self.check_link()
                if not self.valid():
                    raise RuntimeError("console dependencies are absent or changed; run make ui-install first")
                lease = self.leases.register()
                if not self.link.is_symlink():
                    self.link.symlink_to(self.target)
            except BaseException:
                lock.close()
                raise
        try:
            environment = dict(os.environ, PATH=f"{self.modules}/.bin:{os.environ.get('PATH', '')}")
            return supervise(arguments, self.web, (lock.fileno(), lease.fileno()), environment)
        finally:
            with self.leases.metadata():
                lease.close()
                lock.close()
                self.leases.prune()
                try:
                    exclusive = self.leases.acquire(exclusive=True)
                except BusyError:
                    pass
                else:
                    with exclusive:
                        self.remove_link()


def supervise(arguments, cwd, descriptors, environment=None):
    process = subprocess.Popen(arguments, cwd=cwd, env=environment,
                               pass_fds=descriptors, start_new_session=True)
    previous = {}

    def forward(number, _frame):
        try:
            os.killpg(process.pid, number)
        except ProcessLookupError:
            pass

    for number in (signal.SIGHUP, signal.SIGINT, signal.SIGTERM):
        previous[number] = signal.signal(number, forward)
    try:
        result = process.wait()
        return 128 - result if result < 0 else result
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)


def main(arguments):
    try:
        dependencies = Dependencies(Path(__file__).resolve().parents[2])
        if arguments == ["--install"]:
            return dependencies.install()
        if len(arguments) == 2 and arguments[0] == "--trash-work":
            return trash_work(dependencies, Path(arguments[1]))
        if arguments == ["--clean"]:
            return dependencies.clean()
        if not arguments or arguments[0].startswith("--"):
            raise RuntimeError("usage: with-dependencies.sh --install | --clean | COMMAND [ARG ...]")
        return dependencies.command(arguments)
    except (OSError, RuntimeError) as error:
        print(f"console dependencies: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
