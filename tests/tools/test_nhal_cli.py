"""Tests für ./nhal: Plattform-Flags, Feature-Auswahl, Prüfbericht."""

import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def run(*args):
    return subprocess.run([sys.executable, str(ROOT / "tools" / "nhal.py"), *args],
                          capture_output=True, text=True, cwd=ROOT)


class NhalCliTest(unittest.TestCase):
    def test_list_shows_platform_flags(self):
        r = run("list")
        self.assertEqual(r.returncode, 0)
        for flag in ("--macos", "--windows", "--linux", "--xernel", "--web", "--console"):
            self.assertIn(flag, r.stdout)

    def test_check_single_platform(self):
        r = run("check", "--linux", "--features", "time.monotonic")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("linux", r.stdout)

    def test_check_multiple_platforms(self):
        r = run("check", "--macos", "--web", "--features", "time.monotonic")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("macos", r.stdout)
        self.assertIn("web", r.stdout)

    def test_check_all_reports_missing_on_xernel(self):
        r = run("check", "--all", "--features", "time.monotonic")
        self.assertEqual(r.returncode, 1)
        self.assertIn("xernel", r.stderr)
        self.assertIn("FEHLT", r.stdout)
        self.assertIn("extern", r.stdout)   # console

    def test_xernel_memory_features(self):
        r = run("check", "--xernel", "--features", "mem.pool,mem.frame")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("nativ", r.stdout)

    def test_xernel_reports_reasons(self):
        r = run("check", "--xernel", "--features", "fs.file,thread.scheduler")
        self.assertEqual(r.returncode, 1)
        self.assertIn("VFS", r.stdout)
        self.assertIn("Threads", r.stdout)

    def test_optional_is_not_fatal(self):
        r = run("check", "--xernel", "--optional", "time.monotonic")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("weggelassen", r.stdout)

    def test_unknown_feature(self):
        r = run("check", "--linux", "--features", "gibt.es_nicht")
        self.assertEqual(r.returncode, 1)
        self.assertIn("gibt.es_nicht", r.stderr)

    def test_manifest_targets_are_used(self):
        r = run("check", "--manifest", "examples/minimal/nhal.toml")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("web", r.stdout)
        self.assertIn("windows", r.stdout)

    def test_console_is_never_built_here(self):
        r = run("build", "--console", "--features", "time.monotonic", "--build-dir", str(ROOT / "build-cli-test"))
        self.assertEqual(r.returncode, 1)
        self.assertIn("außerhalb", r.stderr)
        r = run("build", "--console", "--features", "time.monotonic", "--skip-unavailable",
                "--build-dir", str(ROOT / "build-cli-test"))
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
