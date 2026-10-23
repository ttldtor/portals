"""Checks for the disposable APT cache validator; no root/network required."""

import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import apt_cache


class AptCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.archive = self.directory / "clang-tidy-20_1%3a20.1.2_amd64.deb"
        self.archive.write_bytes(b"archive payload")
        self.digest = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def metadata(self, **overrides):
        fields = {"Package": "clang-tidy-20", "Version": "1:20.1.2",
                  "Architecture": "amd64", "SHA256": self.digest}
        fields.update(overrides)
        return "\n".join(f"{key}: {value}" for key, value in fields.items()) + "\n"

    def test_valid_archive_uses_exact_package_version_and_architecture(self):
        with patch("apt_cache.subprocess.run", return_value=subprocess.CompletedProcess(
                [], 0, self.metadata(), "")) as command:
            self.assertEqual(apt_cache.prune(self.directory), (1, 0))
        self.assertEqual(command.call_args.args[0],
                         ["apt-cache", "show", "clang-tidy-20:amd64=1:20.1.2"])
        self.assertTrue(self.archive.exists())

    def test_corrupt_archive_is_discarded(self):
        with patch("apt_cache.trusted_hashes", return_value={"0" * 64}):
            self.assertEqual(apt_cache.prune(self.directory), (0, 1))
        self.assertFalse(self.archive.exists())

    def test_stale_mismatched_and_missing_hash_metadata_are_not_trusted(self):
        for overrides in ({"Version": "old"}, {"Architecture": "arm64"},
                          {"Package": "other"}, {"SHA256": ""}, {"SHA256": "invalid"}):
            with self.subTest(overrides=overrides), patch("apt_cache.subprocess.run",
                    return_value=subprocess.CompletedProcess([], 0, self.metadata(**overrides), "")):
                self.assertEqual(apt_cache.trusted_hashes(self.archive), set())

    def test_unknown_package_is_discarded(self):
        with patch("apt_cache.subprocess.run", return_value=subprocess.CompletedProcess([], 100, "", "missing")):
            self.assertEqual(apt_cache.prune(self.directory), (0, 1))

    def test_unrelated_files_are_preserved_and_empty_cache_succeeds(self):
        self.archive.unlink()
        unrelated = self.directory / "keep.txt"
        unrelated.write_text("keep")
        self.assertEqual(apt_cache.prune(self.directory), (0, 0))
        self.assertEqual(unrelated.read_text(), "keep")

    def test_invalid_filename_is_not_passed_to_apt(self):
        with patch("apt_cache.subprocess.run") as command:
            self.assertEqual(apt_cache.trusted_hashes(Path("--bad_1_amd64.deb")), set())
            self.assertEqual(apt_cache.trusted_hashes(Path("invalid.deb")), set())
            command.assert_not_called()

    @unittest.skipIf(sys.platform == "win32", "Symlink creation needs Windows privilege")
    def test_symlink_is_removed_without_touching_target(self):
        target = self.directory / "outside.txt"
        target.write_text("keep")
        self.archive.unlink()
        self.archive.symlink_to(target)
        with patch("apt_cache.trusted_hashes") as metadata:
            self.assertEqual(apt_cache.prune(self.directory), (0, 1))
            metadata.assert_not_called()
        self.assertEqual(target.read_text(), "keep")

    def test_help_needs_no_directory_and_runs_no_apt(self):
        result = subprocess.run([sys.executable, str(Path(apt_cache.__file__)), "--help"],
                                capture_output=True, text=True, cwd=self.directory, check=False)
        self.assertEqual(result.returncode, 0)
        self.assertIn("Deletes invalid", result.stdout)
        self.assertEqual(list(self.directory.iterdir()), [self.archive])


if __name__ == "__main__":
    unittest.main()
