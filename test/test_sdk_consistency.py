from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

CHECK = Path(__file__).resolve().parents[1] / "tools/check_sdk.py"


class SdkConsistencyTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.first = self.root / "external/q_sdk"
        self.second = self.root / "external/q_applets/q_sdk"
        self.first.mkdir(parents=True)
        subprocess.run(["git", "init", "-q", str(self.first)], check=True)
        self.commit(self.first, "Initial SDK")
        self.second.parent.mkdir(parents=True)
        subprocess.run(["git", "clone", "-q", str(self.first), str(self.second)], check=True)

    def commit(self, path, message):
        subprocess.run(["git", "-C", str(path), "-c", "core.hooksPath=/dev/null",
                        "-c", "user.name=SDK check", "-c", "user.email=sdk-check@example.invalid",
                        "commit", "--allow-empty", "--no-gpg-sign", "-qm", message], check=True)

    def check(self):
        return subprocess.run([sys.executable, str(CHECK), str(self.root)], capture_output=True, text=True, timeout=30)

    def test_clean_matching_revisions_pass(self):
        self.assertEqual(self.check().returncode, 0)

    def test_mismatched_revision_fails(self):
        self.commit(self.second, "Different SDK")
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SDK revisions differ", result.stderr)

    def test_dirty_matching_revision_fails(self):
        (self.second / "untracked.txt").write_text("Uncommitted change\n")
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SDK checkout is not clean", result.stderr)


if __name__ == "__main__":
    unittest.main()
