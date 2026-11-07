"""configure-private-lab.sh keeps a config backup and checks first-use TLS inputs strictly."""
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "targets/ti8168/configure-private-lab.sh"


def fragment(start, end):
    text = SCRIPT.read_text()
    return text[text.index(start):text.index(end)]


def run(tmp, body):
    script = tmp / "run.sh"
    script.write_text("set -eu\ndie() { printf '%s\\n' \"$*\" >&2; exit 1; }\n" + body)
    return subprocess.run(["sh", str(script)], capture_output=True, text=True, timeout=60, check=False)


class ConfigBackupTest(unittest.TestCase):
    def setUp(self):
        holder = tempfile.TemporaryDirectory(prefix="privlab.")
        self.addCleanup(holder.cleanup)
        self.tmp = Path(holder.name)

    def test_publish_keeps_previous_config_with_mode_0600(self):
        t = self.tmp
        (t / "conf").write_text("old\n")
        (t / "stage").write_text("new\n")
        body = (f"CONFIG={t}/conf\nCONFIG_STAGE={t}/stage\nCONFIG_BEFORE={t}/conf.before\n"
                "sync() { :; }\ncurrent_config=old\n"
                + fragment("publish_private_lab_transaction() {", "validate_private_lab_config_transaction() {")
                + "publish_private_lab_transaction none new\n")
        result = run(t, body)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((t / "conf").read_text(), "new\n")
        before = t / "conf.before"
        self.assertEqual(before.read_text(), "old\n")
        self.assertEqual(stat.S_IMODE(os.stat(before).st_mode), 0o600)

    def test_purge_knows_the_backup_name(self):
        remove = SCRIPT.with_name("remove.sh").read_text()
        self.assertEqual(remove.count("aula-sipd.conf.before"), 3)

    def test_first_use_tls_inputs_use_strict_check(self):
        text = SCRIPT.read_text()
        first_use = text[text.index("gateway_uid=$(value_once gateway_uid)"):text.index('cat > "$CONFIG_STAGE"')]
        self.assertIn('require_tls_input "$tls_cert"', first_use)
        self.assertIn('require_tls_input "$tls_key"', first_use)


if __name__ == "__main__":
    unittest.main()
