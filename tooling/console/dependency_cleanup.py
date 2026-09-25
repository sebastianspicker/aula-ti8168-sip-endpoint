"""Recoverable work cleanup while dependency metadata remains locked in place."""
import json
import os
from pathlib import Path
import stat


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def require_owned_directory(path, label):
    try:
        info = path.lstat()
    except FileNotFoundError as error:
        raise RuntimeError(f"{label} is absent: {path}") from error
    if (not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid()
            or info.st_mode & 0o022):
        raise RuntimeError(f"{label} must be an owned non-writable real directory: {path}")
    return info


def require_owned_file(path, label):
    try:
        info = path.lstat()
    except FileNotFoundError as error:
        raise RuntimeError(f"{label} is absent: {path}") from error
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_nlink != 1 or info.st_mode & 0o022):
        raise RuntimeError(f"{label} must be an owned single-link non-writable regular file")
    return info


def write_all(descriptor, contents):
    offset = 0
    while offset < len(contents):
        written = os.write(descriptor, contents[offset:])
        if written == 0:
            raise OSError("short write while publishing generated metadata")
        offset += written


def publication_temporary(path):
    return path.parent / f".{path.name}.pending"


def recover_linked_publication(path, label):
    temporary = publication_temporary(path)
    if not temporary.exists() and not temporary.is_symlink():
        require_owned_file(path, label)
        return
    final_info = path.lstat()
    temporary_info = temporary.lstat()
    if (not stat.S_ISREG(final_info.st_mode) or final_info.st_uid != os.getuid()
            or final_info.st_mode & 0o022 or final_info.st_nlink != 2
            or (final_info.st_dev, final_info.st_ino)
            != (temporary_info.st_dev, temporary_info.st_ino)):
        raise RuntimeError(f"{label} has an unsafe interrupted publication")
    temporary.unlink()
    sync_directory(path.parent)
    require_owned_file(path, label)


def publish_new_file(path, contents, label):
    temporary = publication_temporary(path)
    if path.exists() or path.is_symlink():
        recover_linked_publication(path, label)
        if path.read_bytes() != contents:
            raise RuntimeError(f"{label} does not match the pending transaction")
        return
    pending_ready = False
    if temporary.exists() or temporary.is_symlink():
        require_owned_file(temporary, f"pending {label}")
        if temporary.read_bytes() == contents:
            pending_ready = True
        else:
            temporary.unlink()
            sync_directory(path.parent)
    if not pending_ready:
        descriptor = os.open(temporary,
                             os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        try:
            write_all(descriptor, contents)
            os.fsync(descriptor)
        except BaseException:
            os.close(descriptor)
            temporary.unlink(missing_ok=True)
            raise
        os.close(descriptor)
    try:
        os.link(temporary, path, follow_symlinks=False)
        sync_directory(path.parent)
        temporary.unlink()
        sync_directory(path.parent)
    except BaseException:
        if not path.exists() and not path.is_symlink():
            temporary.unlink(missing_ok=True)
        raise
    require_owned_file(path, label)


def valid_entry(item):
    if not isinstance(item, dict) or set(item) != {"name", "device", "inode"}:
        return False
    name = item["name"]
    return (isinstance(name, str) and name not in {"", ".", "..", "locks"}
            and Path(name).name == name and "\t" not in name and "\n" not in name
            and type(item["device"]) is int and type(item["inode"]) is int)


def read_move_plan(record, work, destination):
    recover_linked_publication(record, "Trash transaction journal")
    try:
        plan = json.loads(record.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError("invalid Trash transaction journal") from error
    expected = {"schema", "source", "destination", "destination_device",
                "destination_inode", "entries"}
    if (not isinstance(plan, dict) or set(plan) != expected or plan["schema"] != 2
            or plan["source"] != str(work) or plan["destination"] != str(destination)):
        raise RuntimeError("Trash transaction belongs to a different work tree")
    entries = plan["entries"]
    if not isinstance(entries, list) or not all(valid_entry(item) for item in entries):
        raise RuntimeError("invalid Trash transaction inventory")
    if len({item["name"] for item in entries}) != len(entries):
        raise RuntimeError("duplicate Trash transaction inventory")
    info = require_owned_directory(destination, "Trash transaction destination")
    if (info.st_dev, info.st_ino) != (plan["destination_device"], plan["destination_inode"]):
        raise RuntimeError("Trash transaction destination identity changed")
    return entries


def initial_destination(bundle, destination):
    if destination.exists() or destination.is_symlink():
        require_owned_directory(destination, "Trash transaction destination")
        if any(destination.iterdir()):
            raise RuntimeError(f"Trash destination already exists: {destination}")
        return
    destination.mkdir(mode=0o700)
    sync_directory(bundle)


def move_plan(work, bundle):
    destination = bundle / ".work"
    record = bundle / "WORK_MOVE.json"
    if record.exists() or record.is_symlink():
        return read_move_plan(record, work, destination)
    bundle.mkdir(parents=True, exist_ok=True)
    require_owned_directory(bundle, "Trash bundle")
    initial_destination(bundle, destination)
    entries = []
    for path in sorted(work.iterdir()):
        if path.name != "locks":
            info = path.lstat()
            entries.append({"name": path.name, "device": info.st_dev, "inode": info.st_ino})
    if not all(valid_entry(item) for item in entries):
        raise RuntimeError("work entries cannot be represented in the Trash inventory")
    info = require_owned_directory(destination, "Trash transaction destination")
    plan = {"schema": 2, "source": str(work), "destination": str(destination),
            "destination_device": info.st_dev, "destination_inode": info.st_ino,
            "entries": entries}
    publish_new_file(record, json.dumps(plan, sort_keys=True).encode() + b"\n",
                     "Trash transaction journal")
    return entries


def partition_paths(work, destination, entries):
    planned = {entry["name"] for entry in entries}
    source_names = {path.name for path in work.iterdir() if path.name != "locks"}
    destination_names = {path.name for path in destination.iterdir()}
    if source_names - planned or destination_names - planned:
        raise RuntimeError("Trash transaction namespace contains an unplanned path")
    for entry in entries:
        name = entry["name"]
        if (name in source_names) == (name in destination_names):
            raise RuntimeError(f"Trash transaction path is missing or conflicts: {name}")
        current = work / name if name in source_names else destination / name
        info = current.lstat()
        if (info.st_dev, info.st_ino) != (entry["device"], entry["inode"]):
            raise RuntimeError(f"Trash transaction path identity changed: {name}")
    return source_names


def move_entry(work, destination, entry):
    (work / entry["name"]).rename(destination / entry["name"])
    sync_directory(work)
    sync_directory(destination)


def ledger_contents(entries):
    return ("source\tdestination\n" + "".join(
        f".work/{entry['name']}\t.work/{entry['name']}\n" for entry in entries)).encode()


def publish_ledger(bundle, entries):
    ledger = bundle / "MOVED_PATHS.tsv"
    expected = ledger_contents(entries)
    if ledger.exists() or ledger.is_symlink():
        recover_linked_publication(ledger, "Trash ledger")
        if ledger.read_bytes() != expected:
            raise RuntimeError("Trash ledger does not match the completed transaction")
        return
    publish_new_file(ledger, expected, "Trash ledger")


def trash_work(dependencies, bundle: Path) -> int:
    dependencies.validate_work_paths()
    work = dependencies.project.parents[1]
    bundle = bundle.expanduser().resolve()
    if bundle.is_relative_to(work.resolve()):
        raise RuntimeError("Trash destination must be outside the canonical work tree")
    with dependencies.leases.metadata(), dependencies.leases.acquire(exclusive=True):
        dependencies.leases.prune()
        dependencies.check_link()
        destination = bundle / ".work"
        entries = move_plan(work, bundle)
        source_names = partition_paths(work, destination, entries)
        dependencies.remove_link()
        for entry in entries:
            if entry["name"] in source_names:
                move_entry(work, destination, entry)
                source_names = partition_paths(work, destination, entries)
        partition_paths(work, destination, entries)
        publish_ledger(bundle, entries)
    print(f"generated work moved to Trash: {destination}; coordination locks retained")
    return 0
