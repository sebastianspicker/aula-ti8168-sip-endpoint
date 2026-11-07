"""A die while building the group overlay must leave nothing that the rerun would mount unfinished."""
import tempfile
import unittest
from pathlib import Path

from _fragments import PRELUDE, function_text, run_sh
from test_install_identity import allocator_text

GROUPS = ["aula-web", "aula-gateway", "aula-sip", "aula-control",
          "aula-web-gateway", "aula-web-tls", "aula-media-video", "aula-media-audio"]


class GroupOverlayTest(unittest.TestCase):
    def test_rerun_after_collision_builds_complete_overlay(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            view, identity, passwd = tmp / "group", tmp / "identity", tmp / "passwd"
            identity.mkdir()
            view.write_text("root:x:0:\n")
            passwd.write_text("root:x:0:0::/root:/bin/sh\naula-sip:x:5:5::/:/bin/false\n")
            (tmp / "journal").write_text("")
            text = function_text("install.sh", "prepare_group_view").replace("/etc/group", str(view))
            body = (PRELUDE
                    + f"ROOT_PASSWD_SOURCE={passwd}\nIDENTITY={identity}\nJOURNAL={tmp}/journal\n"
                    "mounted_from() { return 1; }\nmount_root_for_target() { echo none; }\n"
                    f"mount() {{ echo \"$*\" >> {tmp}/mounts; }}\n"
                    "record() { grep -Fx \"$1\" \"$JOURNAL\" >/dev/null 2>&1 || printf '%s\\n' \"$1\" >> \"$JOURNAL\"; }\n"
                    + allocator_text() + "\n" + text + "\nprepare_group_view\n")
            first = run_sh(tmp / "run.sh", body)
            self.assertIn("service identity already exists", first.stderr)
            self.assertFalse((identity / "group").exists())
            passwd.write_text("root:x:0:0::/root:/bin/sh\n")
            second = run_sh(tmp / "run.sh", body)
            self.assertEqual(second.returncode, 0, second.stderr)
            names = [line.split(":")[0] for line in (identity / "group").read_text().splitlines()]
            self.assertEqual(names, ["root"] + GROUPS)
            self.assertIn(f"file:{identity}/group", (tmp / "journal").read_text())
            self.assertEqual((tmp / "mounts").read_text().count("\n"), 1)


if __name__ == "__main__":
    unittest.main()
