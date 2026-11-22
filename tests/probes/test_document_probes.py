"""Small finite qualification-tool contract checks; no multi-gigabyte files or native windows.
Run: python test_document_probes.py --ui-exe PATH --oracle-exe PATH. Reports assertions
on stdout; exits 0 on success, 1 on failure, 2 for CLI. Temporary fixtures/processes are
owned by this test and cleaned. --help needs no executables and does not run tests.
"""
import argparse
import gzip
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("document_corpus", HERE / "document_corpus.py")
corpus = importlib.util.module_from_spec(spec)
spec.loader.exec_module(corpus)


class ProbeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="portals-document-probes-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def invoke(self, executable, *arguments, timeout=25):
        return subprocess.run([str(executable), *map(str, arguments)], cwd=self.root,
                              capture_output=True, text=True, encoding="utf-8", timeout=timeout)

    def fixture(self, kind="sql", compressed=False, long_line=0, name="fixture", size_mib=1):
        path = self.root / (name + (".gz" if compressed else ".txt"))
        args = ["--output", str(path), "--size-mib", str(size_mib), "--format", kind,
                "--long-line-mib", str(long_line)]
        if compressed:
            args.append("--gzip")
        result = self.invoke(sys.executable, HERE / "document_corpus.py", *args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return path

    def result(self, executable, *arguments):
        result = self.invoke(executable, *arguments)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(result.stdout)

    def test_help_without_inputs_and_invalid_arguments(self):
        for executable in (UI, ORACLE):
            with self.subTest(executable=executable):
                result = self.invoke(executable, "--help")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("--file", result.stdout)
                self.assertIn("Exit", result.stdout)
                self.assertEqual(self.invoke(executable, "--unknown-option").returncode, 2)
        result = self.invoke(sys.executable, HERE / "document_corpus.py", "--help")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(list(self.root.iterdir()), [])

    def test_generator_extent_and_exclusive_create(self):
        for kind in ("sql", "markdown"):
            with self.subTest(kind=kind):
                path = self.fixture(kind, True, 1, kind)
                manifest = json.loads(path.with_name(path.name + ".manifest.json").read_text())
                with gzip.open(path, "rb") as stream:
                    data = stream.read()
                self.assertEqual(len(data), corpus.MIB)
                self.assertEqual(data.count(b"\n"), 1)
                self.assertEqual(manifest["long_line_bytes"], corpus.MIB)
                before = path.read_bytes()
                result = self.invoke(sys.executable, HERE / "document_corpus.py", "--output", path)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(path.read_bytes(), before)

    def test_shared_application_views_and_cleanup(self):
        path = self.fixture(name="město-城市")
        for views in (1, 4):
            with self.subTest(views=views):
                value = self.result(UI, "--file", path, "--language", "mysql", "--views", views,
                                    "--timeout-ms", 15000)
                self.assertTrue(value["complete"])
                self.assertEqual(value["views"], views)
                self.assertGreater(value["accepted_frames"], 0)
                self.assertEqual(value["logical_provider"]["ui_io_violations"], 0)
                self.assertLess(value["budget_peak_sampled_bytes"], value["budget_limit_bytes"])
                self.assertEqual(value["budget_after_cleanup_bytes"], value["budget_root_metadata_bytes"])

    def test_idle_overflow_and_deadline(self):
        value = self.result(UI, "--idle")
        self.assertTrue(value["complete"])
        path = self.fixture()
        value = self.result(UI, "--file", path, "--samples", 1, "--timeout-ms", 15000)
        overflow = [entry for entry in value["phases"].values() if entry["overflow"]]
        self.assertTrue(overflow)
        self.assertTrue(all(not entry["valid"] and entry["p95_ns"] is None for entry in overflow))
        result = self.invoke(UI, "--file", path, "--timeout-ms", 1)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertTrue(json.loads(result.stdout)["timeout"])

    def test_auto_exhaustion_keeps_preview_frame(self):
        path = self.fixture(size_mib=8)
        value = self.result(ORACLE, "--file", path, "--size-mib", 8, "--evictions", 0,
                            "--timeout-ms", 15000)
        transitions = [stage for stage in value["stages"] if stage["auto_preview_without_new_frame"]]
        self.assertTrue(transitions)
        self.assertTrue(all(stage["syntax_accuracy"] == 1 for stage in transitions))
        self.assertTrue(all(stage["syntax_state"] != stage["retained_frame_syntax_state"] for stage in transitions))

    def test_negative_oracle_contracts(self):
        value = self.result(ORACLE, "--check")
        self.assertEqual(value["negative_contracts"], 4)

    def test_long_line_exact_oracle(self):
        for kind in ("sql", "markdown"):
            with self.subTest(kind=kind):
                path = self.fixture(kind, long_line=1, name="long-" + kind)
                value = self.result(ORACLE, "--file", path, "--pattern", kind, "--long-line-mib", 1,
                                    "--evictions", 0, "--exact", "--timeout-ms", 15000)
                exact = [stage for stage in value["stages"] if stage["name"] == "explicit_exact"]
                self.assertTrue(exact)
                self.assertTrue(all(stage["syntax_accuracy"] == 2 for stage in exact))

    def test_worker_text_and_color_oracles(self):
        for kind in ("sql", "markdown"):
            with self.subTest(kind=kind):
                path = self.fixture(kind, name=kind)
                value = self.result(ORACLE, "--file", path, "--pattern", kind, "--evictions", 3,
                                    "--timeout-ms", 15000)
                self.assertTrue(value["root_destroyed"])
                self.assertEqual(value["owner_thread_io_calls"], 0)
                self.assertTrue(all(stage["color_mismatches"] == 0 for stage in value["stages"]))
                self.assertTrue(any(stage["verified_exact_maps"] > 0 for stage in value["stages"]))

    def test_gzip_oracle(self):
        if not HAS_GZIP:
            self.skipTest("gzip adapter disabled")
        path = self.fixture(compressed=True)
        for executable in (UI, ORACLE):
            arguments = ["--file", path, "--kind", "gzip", "--timeout-ms", 15000]
            if executable == ORACLE:
                arguments += ["--evictions", 1]
            value = self.result(executable, *arguments)
            self.assertTrue(value["complete"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ui-exe", type=Path, required=True, help="Built whole-Application probe")
    parser.add_argument("--oracle-exe", type=Path, required=True, help="Built worker color oracle")
    parser.add_argument("--gzip", action="store_true", help="Enable gzip-enabled binary contracts (default: disabled)")
    args = parser.parse_args()
    global UI, ORACLE, HAS_GZIP
    UI, ORACLE, HAS_GZIP = args.ui_exe.resolve(), args.oracle_exe.resolve(), args.gzip
    if not UI.is_file() or not ORACLE.is_file():
        parser.error("both probe executables must exist")
    return 0 if unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ProbeTests)).wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
