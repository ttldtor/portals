"""Verify Python help is available before required inputs or operational work."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ToolHelpTests(unittest.TestCase):
    def run_tool(self, script, arguments, cwd):
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        return subprocess.run([sys.executable, str(ROOT / script), *arguments],
                              cwd=cwd, env=environment, capture_output=True,
                              text=True, timeout=10)

    def test_help_requires_no_operational_inputs_or_output(self):
        cases = [
            ("tools/check_naming.py", [], ["--clang-tidy", "compile_commands.json"]),
            ("tools/ci/checks.py", ["select"], ["--base", "--head", "GITHUB_OUTPUT"]),
            ("tools/ci/checks.py", ["gate"], ["CI_NEEDS"]),
            ("tools/docs/check_links.py", [], ["html", "anchor"]),
            ("tools/docs/finalize_html.py", [], ["tag_file", "CMake", "in place"]),
            ("tools/release/start.py", [], ["--task-id", "--dispatch-merged"]),
            ("tools/release/metadata.py", ["prepare"], ["--source", "--output-dir", "overwritten"]),
            ("tools/release/publish.py", [], ["--directory", "--commit", "SHA256SUMS"]),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            for script, arguments, expected in cases:
                for help_flag in ("--help", "-h"):
                    with self.subTest(script=script, flag=help_flag):
                        result = self.run_tool(script, [*arguments, help_flag], temporary)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        for text in expected:
                            self.assertIn(text, result.stdout)
                        self.assertFalse(list(Path(temporary).iterdir()), script)

    def test_link_checker_cli_and_real_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            invalid = self.run_tool("tools/docs/check_links.py", [], root)
            self.assertEqual(invalid.returncode, 2)
            absent = self.run_tool("tools/docs/check_links.py", ["absent"], root)
            self.assertEqual(absent.returncode, 1)
            (root / "index.html").write_text('<a href="target.html#ok">next</a>')
            (root / "target.html").write_text('<h1 id="ok">Target</h1>')
            passed = self.run_tool("tools/docs/check_links.py", [str(root)], root)
            self.assertEqual(passed.returncode, 0, passed.stderr)
            self.assertEqual(json.loads(passed.stdout)["broken"], [])
            (root / "target.html").write_text('<h1 id="wrong">Target</h1>')
            failed = self.run_tool("tools/docs/check_links.py", [str(root)], root)
            self.assertEqual(failed.returncode, 1, failed.stderr)
            self.assertEqual(json.loads(failed.stdout)["broken"][0][2], "missing anchor")


if __name__ == "__main__":
    unittest.main()
