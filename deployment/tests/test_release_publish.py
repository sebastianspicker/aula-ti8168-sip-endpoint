"""Crash leftovers of a release publish: own temporaries are removed, an unjournaled verified release is adopted."""
import tempfile
import unittest
from pathlib import Path

from _fragments import PRELUDE, function_text, run_sh


class ReleasePublishTest(unittest.TestCase):
    def test_own_temporaries_removed_others_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "releases").mkdir()
            body = (PRELUDE + f"BASE={tmp}\nRELEASES={tmp}/releases\nversion=1.0\n"
                    + function_text("install.sh", "remove_own_temporaries") + "\n"
                    "mkdir $RELEASES/.staging-$version-$$ $RELEASES/.staging-$version-1 $BASE/.current-$version-1\n"
                    "ln -s releases/$version $BASE/.current-$version-$$\n"
                    "remove_own_temporaries\n")
            result = run_sh(tmp / "run.sh", body)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(sorted(p.name for p in tmp.iterdir() if p.name != "run.sh"),
                             [".current-1.0-1", "releases"])
            self.assertEqual([p.name for p in (tmp / "releases").iterdir()], [".staging-1.0-1"])

    def publish(self, journal, verified):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / "releases/1.0").mkdir(parents=True)
            if verified:
                (tmp / "releases/1.0/ok").write_text("")
            (tmp / "journal").write_text(journal)
            (tmp / "verify-payload.sh").write_text('[ -e "$2/ok" ]\n')
            body = (PRELUDE + f"SCRIPT_DIR={tmp}\nRELEASES={tmp}/releases\nJOURNAL={tmp}/journal\n"
                    "version=1.0\nmanifest_sha=abc\nrelease=$RELEASES/$version\n"
                    "record() { printf '%s\\n' \"$1\" >> \"$JOURNAL\"; }\nrequire_live_payload_layout() { :; }\n"
                    + function_text("install.sh", "publish_release") + "\npublish_release\n")
            result = run_sh(tmp / "run.sh", body)
            return result, (tmp / "journal").read_text()

    def test_unjournaled_verified_release_is_recorded(self):
        result, journal = self.publish("", verified=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(journal, "release:1.0\nrelease-sha256:1.0:abc\n")

    def test_journaled_or_unverified_release_still_dies(self):
        for journal, verified in (("release:1.0\n", True), ("", False)):
            with self.subTest(journal=journal, verified=verified):
                result, _ = self.publish(journal, verified)
                self.assertIn("release already exists", result.stderr)


if __name__ == "__main__":
    unittest.main()
