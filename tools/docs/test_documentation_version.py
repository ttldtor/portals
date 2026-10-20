"""Check documentation identity with CMake and an isolated Git repository.

Run: python -m unittest discover -s tools/docs -p test_documentation_version.py.
Fixtures create only temporary files, do not use the network or build C++ code,
and report CMake/Git failures through unittest.
"""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake/DocumentationVersion.cmake"


class DocumentationVersionTests(unittest.TestCase):
    def configure(self, source, build, development):
        return subprocess.run(
            ["cmake", "-S", str(source), "-B", str(build),
             f"-DPORTALS_DOCS_DEVELOPMENT={'ON' if development else 'OFF'}"],
            capture_output=True, text=True, encoding="utf-8", errors="replace")

    def fixture(self, source):
        source.mkdir()
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.24)\n'
            'project(DocIdentity VERSION 3.2.1 LANGUAGES NONE)\n'
            f'include("{MODULE.as_posix()}")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/label.txt" "${DOXYGEN_PROJECT_NUMBER}")\n',
            encoding="utf-8")

    def test_archive_default_needs_no_git_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root / "source")
            result = self.configure(root / "source", root / "build", False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((root / "build/label.txt").read_text(), "3.2.1")

    def test_development_requires_a_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root / "source")
            result = self.configure(root / "source", root / "build", True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Development documentation requires a Git commit", result.stderr)

    def test_explicit_preview_label_is_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.fixture(root / "source")
            result = subprocess.run(
                ["cmake", "-S", str(root / "source"), "-B", str(root / "build"),
                 "-DDOXYGEN_PROJECT_NUMBER=PR preview"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((root / "build/label.txt").read_text(), "PR preview")

    def test_development_identifies_exact_revision_and_can_be_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, build = root / "source", root / "build"
            self.fixture(source)
            for args in (["init"], ["add", "CMakeLists.txt"],
                         ["-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid",
                          "-c", "commit.gpgsign=false", "commit", "-m", "Fixture"]):
                subprocess.run(["git", "-C", str(source), *args], check=True,
                               capture_output=True)
            commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                             text=True).strip()
            result = self.configure(source, build, True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            label = (build / "label.txt").read_text(encoding="utf-8")
            self.assertRegex(label, rf"^3\.2\.1 · dev \({commit[:12]} · \d{{4}}-\d{{2}}-\d{{2}} UTC\)$")
            result = self.configure(source, build, False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((build / "label.txt").read_text(), "3.2.1")


if __name__ == "__main__":
    unittest.main()
