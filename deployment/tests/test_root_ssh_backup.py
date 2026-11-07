"""A root-SSH passwd backup left by a run that died before journaling must not block the rerun."""
import tempfile
import unittest
from pathlib import Path

from _fragments import PRELUDE, function_text, run_sh
from test_install_identity import allocator_text

PASSWD = "root:x:0:0::/root:/bin/sh\n"


class RootSshBackupTest(unittest.TestCase):
    def script(self, tmp, view):
        text = function_text("install.sh", "install_service_users_into_root_ssh").replace("/etc/passwd", str(view))
        return (PRELUDE
                + f"ROOT_PASSWD_SOURCE={tmp}/passwd\nROOT_SSH_BACKUP={tmp}/backup\nJOURNAL={tmp}/journal\n"
                  f"IDENTITY={tmp}/identity\nROOT_SSH_PROVISION={tmp}/prov\nROOT_SSH_USER=u\n"
                  f"ROOT_SSH_APPLY={tmp}/apply.sh\n"
                  "chown() { :; }\nrequire_secure_path_chain() { :; }\ndurability_barrier() { :; }\n"
                  "record() { grep -Fx \"$1\" \"$JOURNAL\" >/dev/null 2>&1 || printf '%s\\n' \"$1\" >> \"$JOURNAL\"; }\n"
                  + allocator_text() + "\n" + text + "\ninstall_service_users_into_root_ssh\n")

    def test_rerun_after_die_before_journal(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            view = tmp / "view"
            (tmp / "prov/account-overlays/u").mkdir(parents=True)
            (tmp / "identity").mkdir()
            (tmp / "identity/group").write_text(
                "".join(f"{g}:x:{300 + i}:\n" for i, g in enumerate(["aula-web", "aula-gateway", "aula-sip"])))
            (tmp / "passwd").write_text(PASSWD)
            (tmp / "journal").write_text("")
            (tmp / "apply.sh").write_text(f"#!/bin/sh\ncp {tmp}/passwd {view}\n")
            (tmp / "apply.sh").chmod(0o755)
            run = tmp / "run.sh"
            view.write_text("root:x:0:0::/root:/bin/ash\n")
            first = run_sh(run, self.script(tmp, view))
            self.assertIn("bind view changed", first.stderr)
            view.write_text(PASSWD)
            second = run_sh(run, self.script(tmp, view))
            self.assertEqual(second.returncode, 0, second.stderr)
            self.assertIn(f"root-ssh-passwd-backup:{tmp}/backup", (tmp / "journal").read_text())
            self.assertEqual((tmp / "backup").read_text(), PASSWD)

    def test_foreign_backup_is_still_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "passwd").write_text(PASSWD)
            (tmp / "backup").write_text("someone else\n")
            (tmp / "journal").write_text("")
            result = run_sh(tmp / "run.sh", self.script(tmp, tmp / "view"))
            self.assertIn("backup already exists without journal ownership", result.stderr)


if __name__ == "__main__":
    unittest.main()
