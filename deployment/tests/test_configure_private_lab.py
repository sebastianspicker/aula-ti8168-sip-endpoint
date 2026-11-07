"""Run configure-private-lab.sh fragments against temp directories."""
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


class PrivateLabTest(unittest.TestCase):
    def setUp(self):
        holder = tempfile.TemporaryDirectory(prefix="privlab.")
        self.addCleanup(holder.cleanup)
        self.tmp = Path(holder.name)
        (self.tmp / "tls").mkdir()

    def release_lock_run(self, staged, with_transaction):
        names = [".stage", ".tlscert.stage", ".tlskey.stage", ".cert.recovery", ".key.recovery"]
        for name in names:
            (self.tmp / name).write_text("x")
        if with_transaction:
            (self.tmp / "tx").write_text("x")
        t = self.tmp
        body = (f"LOCK={t}/lock\nPRIVATE_LAB_TX={t}/tx\nCONFIG_STAGE={t}/.stage\n"
                f"TLS_CERT_STAGE={t}/.tlscert.stage\nTLS_KEY_STAGE={t}/.tlskey.stage\n"
                f"TLS_CERT_RECOVERY={t}/.cert.recovery\nTLS_KEY_RECOVERY={t}/.key.recovery\n"
                f"staged_untransacted={staged}\nmkdir $LOCK\n"
                + fragment("release_lock() {", "transaction_value_once() {") + "release_lock\n")
        result = run(t, body)
        self.assertEqual(result.returncode, 0, result.stderr)
        return [n for n in names if (t / n).exists()]

    def test_orphan_stage_files_removed_when_no_transaction(self):
        self.assertEqual(self.release_lock_run(1, False), [])

    def test_stage_files_kept_when_transaction_exists(self):
        self.assertEqual(len(self.release_lock_run(1, True)), 5)

    def test_stage_files_kept_when_not_ours(self):
        self.assertEqual(len(self.release_lock_run(0, False)), 5)

    def test_unchanged_pair_checked_before_any_copy(self):
        text = SCRIPT.read_text()
        replace = text[text.index('if [ "$replace_tls" = 1 ]; then\n    [ -n "$tls_cert" ]'):]
        self.assertLess(replace.index("TLS replacement pair is unchanged"),
                        replace.index('cp "$TLS_CERT" "$TLS_CERT_RECOVERY"'))


if __name__ == "__main__":
    unittest.main()
