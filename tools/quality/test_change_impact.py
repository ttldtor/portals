# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Internal regression driver: python tools/quality/test_change_impact.py.

Creates disposable source/capture fixtures; executes the analyzer CLI, never a
compiler or a real build. Input/parser/coverage failures are asserted. Actual
project capture integration is a separate measured receipt. Returns unittest status.
"""

import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
import change_impact as impact


class ImpactTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        public = self.root / "include/portals"
        public.mkdir(parents=True)
        (public / "base.hpp").write_text("#pragma once\n", encoding="utf-8")
        (public / "view.hpp").write_text('#include <portals/base.hpp>\n#if 0\n#include "base.hpp"\n#endif\n// #include <portals/missing.hpp>\n', encoding="utf-8")
        self.rows = [{"directory": "/build space", "file": "/source space/src/main.cpp",
                      "output": "CMakeFiles/core.dir/src/main.cpp.o", "command": "c++ -c source"},
                     {"directory": "/build space", "file": "CMakeFiles/core.dir/src/main.cpp.o", "output": "app", "command": "link"},
                     {"directory": "/build space", "file": ".", "output": "phony", "command": ""}]
        self.deps = "CMakeFiles/core.dir/src/main.cpp.o: #deps 2, deps mtime 1 (VALID)\n    /source space/src/main.cpp\n    /source space/include/portals/base.hpp\n\napp: #deps 1, deps mtime 1 (VALID)\n    /lib/libc.so\n"

    def analyze(self, deps=None, rows=None, changed=(), live=False, recorded="/source space"):
        return impact.analyze(self.deps if deps is None else deps,
                              json.dumps(self.rows if rows is None else rows), self.root, recorded, changed, live)

    def cli(self, *args, cwd=None):
        return subprocess.run([sys.executable, str(Path(impact.__file__)), *args], cwd=cwd or self.root,
                              capture_output=True, text=True, encoding="utf-8", timeout=15,
                              env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))

    def test_help_needs_no_workload_inputs_or_side_effects(self):
        with tempfile.TemporaryDirectory() as empty:
            for flag in ("--help", "-h"):
                result = self.cli(flag, cwd=empty)
                self.assertEqual(result.returncode, 0, result.stderr)
                for text in ("--build-dir", "--deps", "--compdb", "--recorded-source-root", "Exit 0", "never a build"):
                    self.assertIn(text, result.stdout)
                self.assertEqual(list(Path(empty).iterdir()), [])

    def test_compile_rows_ignore_phony_and_link_dependency_records(self):
        report = self.analyze(changed=["include/portals/base.hpp"])
        self.assertEqual(report["coverage"]["configured_compile_objects"], 1)
        self.assertEqual(report["coverage"]["observed_compile_objects"], 1)
        self.assertEqual(report["coverage"]["noncompile_dependency_records"], ["app"])
        self.assertTrue(report["coverage"]["record_coverage_complete"])
        self.assertEqual(report["headers"][0]["affected_objects"], 1)
        self.assertEqual(report["changes"][0]["affected_targets"], ["core"])
        self.assertIn("full/unknown", report["changes"][0]["recommendation"])

    def test_windows_drive_case_backslashes_and_spaces(self):
        rows = [{"directory": "C:\\Build Space", "file": "C:\\Source Space\\src\\main.cpp", "output": "CMakeFiles\\CoreLibrary.dir\\src\\main.cpp.obj"}]
        deps = "CMakeFiles/CoreLibrary.dir/src/main.cpp.obj: #deps 1, deps mtime 10 (VALID)\n    c:/source space/include/portals/base.hpp\n"
        report = self.analyze(deps, rows, ["include/portals/base.hpp"], recorded="C:\\Source Space")
        self.assertEqual(report["changes"][0]["affected_objects"], ["cmakefiles/corelibrary.dir/src/main.cpp.obj"])
        self.assertEqual(report["changes"][0]["affected_targets"], ["CoreLibrary"])
        self.assertEqual(report["objects"][0]["source"], "src/main.cpp")

    def test_malformed_count_duplicate_and_unknown_line_fail(self):
        for text in (self.deps.replace("#deps 2", "#deps 3"), self.deps + self.deps, "not a dependency record\n", "    orphan.hpp\n"):
            with self.subTest(text=text[:30]), self.assertRaises(ValueError):
                self.analyze(text)

    def test_missing_stale_unmatched_and_module_coverage(self):
        rows = copy.deepcopy(self.rows)
        rows += [{"directory": "/build space", "file": "/source space/other.cpp", "output": "other.o"},
                 {"directory": "/build space", "file": "/source space/module.cppm", "output": "module.o"}]
        deps = self.deps.replace("(VALID)", "(STALE)") + "obsolete.o: #deps 0, deps mtime 0 (VALID)\n"
        report = self.analyze(deps, rows)
        self.assertFalse(report["coverage"]["record_coverage_complete"])
        self.assertEqual(report["coverage"]["missing_objects"], ["other.o"])
        self.assertEqual(len(report["coverage"]["stale_objects"]), 1)
        self.assertEqual(report["coverage"]["unmatched_dependency_records"], ["obsolete.o"])
        self.assertEqual(report["coverage"]["unsupported_module_sources"], ["/source space/module.cppm"])

    def test_unknown_change_has_null_impact_not_zero(self):
        change = self.analyze(changed=["include/portals/view.hpp"])["changes"][0]
        self.assertEqual(change["status"], "unknown")
        self.assertIsNone(change["affected_objects"])
        self.assertIsNone(change["affected_targets"])
        self.assertIn("full/unknown", change["recommendation"])

    def test_unknown_target_is_null_instead_of_an_empty_target_set(self):
        rows = [{"directory": "/build space", "file": "/source space/main.cpp", "output": "main.o"}]
        deps = "main.o: #deps 1, deps mtime 1 (VALID)\n    /source space/include/portals/base.hpp\n"
        change = self.analyze(deps, rows, ["include/portals/base.hpp"])["changes"][0]
        self.assertEqual(change["affected_objects"], ["main.o"])
        self.assertIsNone(change["affected_targets"])
        self.assertEqual(change["unknown_target_objects"], ["main.o"])

    def test_lexical_graph_is_separate_from_observed_dependencies(self):
        report = self.analyze()
        self.assertEqual(report["summary"]["lexical_public_headers"], 2)
        self.assertEqual(report["summary"]["observed_public_headers"], 1)
        self.assertEqual(report["public_include_graph"]["edges"], [["include/portals/view.hpp", "include/portals/base.hpp"]])
        self.assertIn("inactive conditional", report["public_include_graph"]["kind"])
        self.assertEqual(report["public_include_graph"]["unresolved_public_includes"], [])

    def test_live_mtime_detects_newer_dependencies_and_missing_files(self):
        build = self.root / "build"
        build.mkdir()
        obj = build / "main.o"
        obj.write_text("object")
        source = self.root / "main.cpp"
        source.write_text("source")
        os.utime(obj, ns=(10, 10))
        rows = [{"directory": str(build), "file": str(source), "output": "main.o"}]
        deps = f"main.o: #deps 2, deps mtime 10 (VALID)\n    {source}\n    {self.root / 'absent.hpp'}\n"
        report = self.analyze(deps, rows, live=True, recorded=str(self.root))
        self.assertFalse(report["coverage"]["record_coverage_complete"])
        self.assertTrue(report["coverage"]["unavailable_dependency_paths"])
        self.assertIn("newer", report["coverage"]["stale_objects"][0]["reasons"][0])

    def test_compdb_without_output_or_multiple_directories_is_rejected(self):
        rows = copy.deepcopy(self.rows)
        del rows[0]["output"]
        with self.assertRaisesRegex(ValueError, "require directory"):
            self.analyze(rows=rows)
        rows = copy.deepcopy(self.rows)
        rows.append({"directory": "/another", "file": "/source space/a.cpp", "output": "other.o"})
        with self.assertRaisesRegex(ValueError, "one Ninja build directory"):
            self.analyze(rows=rows)

    def test_cli_receipt_is_reproducible_and_preserves_existing_output(self):
        deps = self.root / "deps.txt"
        compdb = self.root / "compdb.json"
        deps.write_bytes(self.deps.replace("\n", "\r\n").encode())
        compdb.write_text(json.dumps(self.rows), encoding="utf-8")
        args = ["--source-root", str(self.root), "--deps", str(deps), "--compdb", str(compdb), "--recorded-source-root", "/source space"]
        first = self.cli(*args, "--output-dir", str(self.root / "first"))
        self.assertEqual(first.returncode, 0, first.stderr)
        second = self.cli(*args, "--output-dir", str(self.root / "second"))
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(first.stdout, second.stdout)
        self.assertEqual(len(list((self.root / "first").iterdir())), 7)
        self.assertEqual(json.loads(first.stdout)["provenance"]["deps_sha256"], hashlib.sha256(deps.read_bytes()).hexdigest())
        for path in (self.root / "first").iterdir():
            self.assertEqual(path.read_bytes(), (self.root / "second" / path.name).read_bytes())
        again = self.cli(*args, "--output-dir", str(self.root / "first"))
        self.assertEqual(again.returncode, 1)
        self.assertEqual((self.root / "first/ninja-deps.txt").read_bytes(), deps.read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)
