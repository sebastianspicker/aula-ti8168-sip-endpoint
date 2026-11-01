"""Run install.sh's identity allocator against fake passwd/group files."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

INSTALL = Path(__file__).resolve().parents[1] / "targets/ti8168/install.sh"
GROUPS = ["aula-web", "aula-gateway", "aula-sip", "aula-control",
          "aula-web-gateway", "aula-web-tls", "aula-media-video", "aula-media-audio"]
USERS = [("aula-web", "aula-web"), ("aula-gateway", "aula-gateway"), ("aula-sip", "aula-sip")]


def allocator_text():
    text = INSTALL.read_text()
    start = text.index("next_unused_id()")
    end = text.index("\n", text.index("append_user()")) + 1
    return text[start:end]


class InstallIdentityTest(unittest.TestCase):
    def test_service_ids_are_distinct_and_unused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            passwd, group, temp = tmp / "passwd", tmp / "identity/group", tmp / "passwd.new"
            group.parent.mkdir()
            passwd.write_text("root:x:0:0::/root:/bin/sh\nuser:x:1000:1000::/home:/bin/sh\n"
                              "old:x:200:200::/:/bin/false\n")
            group.write_text("root:x:0:\nuser:x:1000:\nold:x:200:\n")
            temp.write_text(passwd.read_text())
            before = passwd.read_text() + group.read_text()
            calls = [f"append_group {g} ''" for g in GROUPS] + [f"append_user {n} {g}" for n, g in USERS]
            script = tmp / "run.sh"
            script.write_text(
                "set -eu\n"
                "die() { printf '%s\\n' \"$*\" >&2; exit 1; }\n"
                f"ROOT_PASSWD_SOURCE={passwd}\nIDENTITY={group.parent}\nROOT_PASSWD_TEMP={temp}\n"
                + allocator_text() + "\n".join(calls) + "\n")
            result = subprocess.run(["sh", str(script)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            used = set(re.findall(r"^[^:]*:x:(\d+):", before, re.M)) | set(re.findall(r"^[^:]*:x:\d+:(\d+)", before, re.M))
            gids = re.findall(r"^aula[^:]*:x:(\d+):", group.read_text(), re.M)
            uids = re.findall(r"^aula[^:]*:x:(\d+):", temp.read_text(), re.M)
            self.assertEqual(len(set(gids)), 8, gids)
            self.assertEqual(len(set(uids)), 3, uids)
            self.assertFalse(used & set(gids) | used & set(uids))


if __name__ == "__main__":
    unittest.main()
