# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Internal driver: python tools/ci/test_check_plan.py.

Uses explicit policy fixtures and temporary source bundles, including no-.git
CLI execution. It does not run runtime tests, compilers or Git. Returns unittest
status; actual configured-inventory pilot is separate coordinator evidence.
"""

import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
import check_plan

ROOT = check_plan.ROOT
MATRIX = json.loads(check_plan.MATRIX.read_text(encoding="utf-8"))


def inventory():
    tests = sorted({name for item in MATRIX["contracts"].values() for name in item["tests"]} | {"portals.unrelated-runtime", "portals.package-install-multi-config"})
    return {"kind": "ctestInfo", "tests": [{"name": name} for name in tests]}


class CheckPlanTests(unittest.TestCase):
    def result(self, path, status="M", **kwargs):
        return check_plan.plan([{"path": path, "status": status}], inventory(), MATRIX, ROOT, **kwargs)

    def test_current_selector_is_reused_without_hosted_weakening(self):
        for path in ("docs/development.md", "include/portals/ui.hpp", "tools/ci/checks.py", "unknown.hpp"):
            result = self.result(path)
            self.assertEqual(result["hosted_ci"]["existing_selector_mode"], check_plan.select_mode([path]))
            self.assertFalse(result["hosted_ci"]["policy_changed"])
            self.assertIn("strict EN/RU documentation and links", result["persistent_obligations"]["integration"])

    def test_modified_selector_gets_exact_local_fixtures_only_at_task_level(self):
        task = self.result("tools/ci/checks.py")
        self.assertEqual(task["mode"], "focused preflight")
        self.assertEqual(task["selected_tests"], ["portals.check-plan", "portals.ci-selection"])
        self.assertEqual(task["hosted_ci"]["existing_selector_mode"], "full")
        for level in ("integration", "milestone"):
            result = self.result("tools/ci/checks.py", level=level)
            self.assertEqual(result["mode"], "full")
            self.assertEqual(result["selected_tests"], check_plan.inventory_names(inventory()))

    def test_docs_only_uses_existing_policy_and_no_ctest_command(self):
        result = self.result("docs/development.md")
        self.assertEqual(result["mode"], "documentation")
        self.assertEqual(result["selected_tests"], [])
        self.assertIsNone(result["ctest_arguments"])
        self.assertEqual(result["hosted_ci"]["existing_selector_mode"], "docs")

    def test_core_templates_consumers_styles_runtime_have_no_full_suite_omissions(self):
        scenarios = {"include/portals/geometry.hpp": "geometry", "include/portals/ui.hpp": "templates",
                     "tests/package_install.cmake": "consumer", "src/styles/style.cpp": "styles",
                     "src/runtime/application.cpp": "runtime", "include/portals/managed_diagnostics.hpp": "diagnostics"}
        expected = check_plan.inventory_names(inventory())
        for path, contract in scenarios.items():
            with self.subTest(path=path):
                result = self.result(path)
                self.assertEqual(result["mode"], "full")
                self.assertIn(contract, result["direct_contracts"])
                self.assertEqual(result["selected_tests"], expected)
                self.assertIn("portals.package-install", result["selected_tests"])

    def test_cross_contract_closure_includes_runtime_and_consumer(self):
        result = self.result("include/portals/style.hpp")
        self.assertTrue({"styles", "diagnostics", "runtime", "lifetime", "render", "scheduler", "consumer"} <= set(result["focus_contracts"]))
        self.assertTrue({"portals.writer", "portals.managed-diagnostics", "portals.application", "portals.package-install"} <= set(result["focus_tests"]))

    def test_new_deleted_renamed_unknown_empty_and_missing_paths_fall_back_full(self):
        for status in ("A", "D", "R", "C", "?", ""):
            with self.subTest(status=status):
                self.assertEqual(self.result("tools/ci/checks.py", status)["mode"], "full")
        self.assertEqual(self.result("unknown.hpp")["mode"], "full")
        self.assertEqual(self.result("docs/new-file.md")["mode"], "full")
        self.assertEqual(check_plan.plan([], inventory(), MATRIX, ROOT)["mode"], "full")

    def test_missing_inventory_or_registration_has_full_unknown_plan(self):
        changes = [{"path": "tools/ci/checks.py", "status": "M"}]
        result = check_plan.plan(changes, None, MATRIX, ROOT)
        self.assertEqual(result["mode"], "full")
        self.assertIsNone(result["selected_tests"])
        self.assertEqual(result["ctest_arguments"], ["--output-on-failure"])
        result = check_plan.plan(changes, {"kind": "ctestInfo", "tests": [{"name": "portals.check-plan"}]}, MATRIX, ROOT)
        self.assertEqual(result["mode"], "full")
        self.assertIn("portals.ci-selection", result["proof"]["missing_ctest_names"])

    def test_partial_stale_and_complete_dependency_receipts_do_not_authorize_runtime_skips(self):
        base = {"schema": 1, "coverage": {"record_coverage_complete": True, "observed_compile_objects": 1, "configured_compile_objects": 1},
                "provenance": {"mode": "live Ninja read-only"}, "objects": [{"repository_dependencies": ["include/portals/style.hpp"]}]}
        for kind in ("complete", "partial", "stale", "recorded"):
            receipt = copy.deepcopy(base)
            if kind == "partial":
                receipt["coverage"]["record_coverage_complete"] = False
            if kind == "stale":
                receipt["coverage"]["stale_objects"] = [{"output": "old.o"}]
            if kind == "recorded":
                receipt["provenance"]["mode"] = "recorded Ninja captures"
            result = self.result("include/portals/style.hpp", impact=receipt)
            self.assertEqual(result["mode"], "full")
            self.assertEqual(result["selected_tests"], check_plan.inventory_names(inventory()))

    def test_invalid_inputs_fail_instead_of_producing_a_subset(self):
        for changes in ({}, [{"path": "../outside", "status": "M"}], [{"path": "C:/absolute", "status": "M"}], [{"path": "docs\\development.md", "status": "M"}], [{"path": "./docs/development.md", "status": "M"}], [{"path": "x", "status": "M"}] * 2):
            with self.assertRaises(ValueError):
                check_plan.plan(changes, inventory(), MATRIX, ROOT)
        with self.assertRaises(ValueError):
            self.result("include/portals/style.hpp", impact={"schema": 99})
        invalid = copy.deepcopy(MATRIX)
        invalid["contracts"]["selector"]["kind"] = "unknown"
        with self.assertRaisesRegex(ValueError, "supported kind"):
            check_plan.plan([{"path": "tools/ci/checks.py", "status": "M"}], inventory(), invalid, ROOT)

    def test_help_and_source_archive_cli_need_no_git_or_private_inputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary)
            scripts = bundle / "tools/ci"
            scripts.mkdir(parents=True)
            for name in ("check_plan.py", "checks.py", "check_contracts.json"):
                shutil.copyfile(Path(__file__).with_name(name), scripts / name)
            for name in ("test_checks.py", "test_check_plan.py"):
                shutil.copyfile(Path(__file__).with_name(name), scripts / name)
            empty = bundle / "empty"
            empty.mkdir()
            original_entries = {path.relative_to(bundle) for path in bundle.rglob("*")}
            for flag in ("--help", "-h"):
                result = subprocess.run([sys.executable, str(scripts / "check_plan.py"), flag], cwd=empty, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("--changes", result.stdout)
                self.assertIn("No Git", result.stdout)
                self.assertEqual(list(empty.iterdir()), [])
                self.assertEqual({path.relative_to(bundle) for path in bundle.rglob("*")}, original_entries)
            changes = bundle / "changes.json"
            tests = bundle / "tests.json"
            changes.write_text("\ufeff" + json.dumps([{"path": "tools/ci/checks.py", "status": "M"}]), encoding="utf-8")
            tests.write_text("\ufeff" + json.dumps(inventory()), encoding="utf-8")
            args = [sys.executable, str(scripts / "check_plan.py"), "--changes", str(changes), "--inventory", str(tests), "--output", str(bundle / "result.json")]
            result = subprocess.run(args, cwd=empty, capture_output=True, text=True, encoding="utf-8", timeout=10,
                                    env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["mode"], "focused preflight")
            self.assertFalse((bundle / ".git").exists())
            prior = (bundle / "result.json").read_bytes()
            again = subprocess.run(args, cwd=empty, capture_output=True, text=True, timeout=10)
            self.assertEqual(again.returncode, 1)
            self.assertEqual((bundle / "result.json").read_bytes(), prior)

    def test_nested_payload_types_are_rejected_with_cli_diagnostics(self):
        changes = [{"path": "include/portals/style.hpp", "status": "M"}]
        base = {"schema": 1, "coverage": {}, "provenance": {"mode": "recorded Ninja captures"}, "objects": []}
        malformed = []
        for field, value in (("objects", [None]), ("objects", [{"repository_dependencies": [None]}]),
                             ("provenance", []), ("provenance", None), ("coverage", [])):
            item = copy.deepcopy(base)
            item[field] = value
            malformed.append(item)
        missing = copy.deepcopy(base)
        del missing["provenance"]
        malformed.append(missing)
        for payload in malformed:
            with self.assertRaises(ValueError):
                check_plan.plan(changes, inventory(), MATRIX, ROOT, impact=payload)
            with self.assertRaises(ValueError):
                self.result("tools/ci/checks.py", impact=payload)
        for tests in ([None], [{}], [{"name": []}], [{"name": ""}]):
            with self.assertRaises(ValueError):
                check_plan.plan(changes, {"kind": "ctestInfo", "tests": tests}, MATRIX, ROOT)
        invalid = copy.deepcopy(MATRIX)
        invalid["contracts"]["selector"] = None
        with self.assertRaises(ValueError):
            check_plan.plan(changes, inventory(), invalid, ROOT)
        invalid["contracts"]["selector"] = dict(MATRIX["contracts"]["selector"], kind=[])
        with self.assertRaises(ValueError):
            check_plan.plan(changes, inventory(), invalid, ROOT)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "changes.json").write_text(json.dumps(changes))
            (directory / "inventory.json").write_text(json.dumps(inventory()))
            for payload in malformed:
                (directory / "impact.json").write_text(json.dumps(payload))
                result = subprocess.run([sys.executable, str(Path(check_plan.__file__)), "--changes", str(directory / "changes.json"),
                                         "--inventory", str(directory / "inventory.json"), "--impact", str(directory / "impact.json")],
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1)
                self.assertIn("check-plan:", result.stderr)
                self.assertNotIn("Traceback", result.stderr)

    def test_plan_is_deterministic_and_preserves_full_gate_obligations(self):
        first = self.result("tools/ci/checks.py")
        self.assertEqual(first, self.result("tools/ci/checks.py"))
        self.assertTrue(first["proof"]["witness_sha256"])
        for preset in ("windows-msvc-debug", "windows-msvc-release", "linux-gcc-debug", "linux-gcc-release"):
            self.assertIn(preset, first["persistent_obligations"]["integration"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
