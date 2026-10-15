"""Regression tests for PR check selection and the required result gate."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from checks import select_mode, validate_results


class SelectionTests(unittest.TestCase):
    def test_documentation_paths(self):
        for path in ("README.md", "ReleaseNotes.md", "docs/index.md",
                     "docs/ru/styles.md", "docs/images/example.png",
                     "docs/diagrams.css", "docs/DoxygenLayout.xml", "docs/mermaid.json"):
            with self.subTest(path=path):
                self.assertEqual(select_mode([path]), "docs")

    def test_full_for_code_infrastructure_and_unknown_paths(self):
        for path in ("include/portals/ui.hpp", "src/input.cpp", "tests/input.cpp",
                     "examples/hello.cpp", "CMakeLists.txt", "cmake/docs.cmake",
                     ".github/workflows/ci.yml", "tools/docs/check_links.py",
                     "docs/example.cpp", "docs/run.py", "docs/images/run.sh",
                     "docs/measurements/data.json", "new-file", "vendor/README.md"):
            with self.subTest(path=path):
                self.assertEqual(select_mode([path]), "full")

    def test_empty_and_mixed_changes_use_full(self):
        self.assertEqual(select_mode([]), "full")
        self.assertEqual(select_mode(["docs/index.md", "src/input.cpp"]), "full")

    def test_git_diff_handles_deletions_and_both_rename_paths(self):
        script = Path(__file__).with_name("checks.py").resolve()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)

            def git(*args):
                return subprocess.check_output(["git", *args], cwd=root, text=True).strip()

            git("init", "-q")
            git("config", "user.name", "CI tests")
            git("config", "user.email", "ci@example.invalid")
            (root / "source.cpp").write_text("example\n")
            (root / "old.md").write_text("old\n")
            git("add", ".")
            git("commit", "-qm", "base")
            base = git("rev-parse", "HEAD")
            (root / "docs").mkdir()
            git("mv", "source.cpp", "docs/example.md")
            git("rm", "old.md")
            git("commit", "-qm", "move and delete")
            output = subprocess.check_output(
                [sys.executable, str(script), "select", "--base", base,
                 "--head", "HEAD"], cwd=root, text=True)
            result = json.loads(output)
            self.assertEqual(result["mode"], "full")
            self.assertEqual(set(result["paths"]),
                             {"source.cpp", "old.md", "docs/example.md"})
            base = git("rev-parse", "HEAD")
            git("rm", "docs/example.md")
            (root / "docs").mkdir(exist_ok=True)
            # Exercise Git paths without shell word splitting or API file limits.
            for index in range(350):
                (root / "docs" / f"guide {index}.md").write_text("guide\n")
            git("add", ".")
            git("commit", "-qm", "documentation only")
            environment = dict(os.environ, GITHUB_OUTPUT=str(root / "outputs"),
                               GITHUB_STEP_SUMMARY=str(root / "summary"))
            output = subprocess.check_output(
                [sys.executable, str(script), "select", "--base", base,
                 "--head", "HEAD"], cwd=root, env=environment, text=True)
            result = json.loads(output)
            self.assertEqual(result["mode"], "docs")
            self.assertEqual(len(result["paths"]), 351)
            self.assertEqual((root / "outputs").read_text(), "mode=docs\n")
            self.assertIn("Selected checks: docs", (root / "summary").read_text())
            result = subprocess.run(
                [sys.executable, str(script), "select", "--base", "missing-ref",
                 "--head", "HEAD"], cwd=root, capture_output=True)
            self.assertNotEqual(result.returncode, 0)


def results(mode="docs", changes="success", docs="success",
            naming="skipped", smoke="skipped"):
    return {"changes": {"result": changes, "outputs": {"mode": mode}},
            "docs": {"result": docs}, "naming": {"result": naming},
            "smoke": {"result": smoke}}


class GateTests(unittest.TestCase):
    def test_successful_modes(self):
        validate_results(results())
        validate_results(results("full", naming="success", smoke="success"))

    def test_selected_job_must_succeed(self):
        for state in ("failure", "cancelled", "skipped", ""):
            for job in ("docs", "naming", "smoke"):
                with self.subTest(state=state, job=job):
                    data = results("full", naming="success", smoke="success")
                    data[job]["result"] = state
                    with self.assertRaises(ValueError):
                        validate_results(data)

    def test_selection_cannot_fail_or_be_missing(self):
        for data in (results(changes="failure"), results(mode=""), {},
                     results(mode="typo"), results(changes="skipped")):
            with self.subTest(data=data), self.assertRaises(ValueError):
                validate_results(data)

    def test_documentation_failure_blocks_docs_mode(self):
        with self.assertRaises(ValueError):
            validate_results(results(docs="failure"))

    def test_gate_command_exits_nonzero_on_failure(self):
        script = Path(__file__).with_name("checks.py")
        environment = dict(os.environ, CI_NEEDS=json.dumps(results(docs="skipped")))
        result = subprocess.run([sys.executable, str(script), "gate"],
                                env=environment, capture_output=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
