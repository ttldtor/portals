"""Run with python -m unittest tools.test_check_naming; uses no C++ toolchain."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


RUNNER = Path(__file__).with_name("check_naming.py")
# Python itself is the fake executable: each selected .cpp is a Python driver.
DRIVER = '''import json, os, pathlib, sys, time
source = pathlib.Path(__file__)
started = time.perf_counter()
time.sleep(0.18 if source.stem.startswith("a") else 0.08)
print("out:" + source.name, flush=True)
print("err:" + source.name, file=sys.stderr, flush=True)
if source.stem == "large":
    sys.stdout.buffer.write(b"x" * 262144 + b"\\n")
    sys.stderr.buffer.write(b"y" * 262144 + b"\\n")
record = dict(start=started, end=time.perf_counter(), args=sys.argv[1:], cwd=os.getcwd())
(pathlib.Path(os.environ["NAMING_EVENTS"]) / (source.stem + ".json")).write_text(json.dumps(record))
sys.exit(7 if "fail" in source.stem else 0)
'''


class NamingRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="naming tests ")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / "project with spaces"
        (self.root / "tools").mkdir(parents=True)
        shutil.copyfile(RUNNER, self.root / "tools/check_naming.py")
        self.build = self.base / "build with spaces"
        self.build.mkdir()
        self.events = self.base / "events"
        self.events.mkdir()
        self.scratch = self.base / "scratch"
        self.scratch.mkdir()
        self.environment = dict(os.environ, NAMING_EVENTS=str(self.events),
                                TMPDIR=str(self.scratch), TMP=str(self.scratch),
                                TEMP=str(self.scratch), PYTHONDONTWRITEBYTECODE="1")

    def database(self, paths):
        entries = []
        for relative in paths:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(DRIVER, encoding="utf-8")
            entries.append(dict(file=relative, directory=str(self.root), command="unused"))
        (self.build / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")

    def run_checker(self, *arguments, tool=sys.executable):
        result = subprocess.run([sys.executable, str(self.root / "tools/check_naming.py"),
                                 str(self.build), "--clang-tidy", tool, *arguments],
                                cwd=self.base, env=self.environment, capture_output=True,
                                text=True, timeout=20)
        self.assertEqual(list(self.scratch.iterdir()), [], "temporary output leaked")
        return result

    def records(self):
        return [json.loads(path.read_text()) for path in self.events.glob("*.json")]

    def test_selection_and_invocation_contract(self):
        selected = [f"{folder}/unit_{folder}.cpp" for folder in
                    ("src", "tests", "tools", "examples", "experiments", "fuzz")]
        excluded = [f"tests/fixtures/{fixture}/invalid.cpp" for fixture in
                    ("static_scene", "static_styles", "ct_pss")]
        self.database([*selected, selected[0], *excluded, "vendor/no.cpp", "../outside.cpp"])
        database = self.build / "compile_commands.json"
        entries = json.loads(database.read_text())
        entries.append(dict(file=str(self.root / selected[0]), directory=str(self.root), command="unused"))
        database.write_text(json.dumps(entries), encoding="utf-8")
        for arguments in ((), ("--jobs", "2")):
            result = self.run_checker(*arguments)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Checked 6 translation units; 0 failed.", result.stdout)
            self.assertEqual(len(self.records()), 6)
        for record in self.records():
            self.assertEqual(record["cwd"], str(self.root))
            self.assertEqual(record["args"], ["-p", str(self.build), "--quiet",
                                             f"--config-file={self.root / '.clang-tidy'}"])

    def test_parallel_matches_serial_and_checks_after_failure(self):
        self.database(["src/z.cpp", "src/a_fail.cpp", "src/m.cpp"])
        serial = self.run_checker("--jobs", "1")
        parallel = self.run_checker("--jobs", "2")
        self.assertEqual(serial.returncode, 1, serial.stderr)
        self.assertEqual(parallel.returncode, 1, parallel.stderr)
        self.assertEqual(serial.stdout, parallel.stdout)
        self.assertEqual(serial.stderr, parallel.stderr)
        lines = parallel.stdout.splitlines()
        for name in ("a_fail.cpp", "m.cpp", "z.cpp"):
            index = lines.index("out:" + name)
            self.assertEqual(lines[index - 1], f"Checking {Path('src') / name}")
        self.assertEqual([line for line in parallel.stdout.splitlines() if line.startswith("out:")],
                         ["out:a_fail.cpp", "out:m.cpp", "out:z.cpp"])
        self.assertEqual(len(self.records()), 3)
        self.assertIn("Checked 3 translation units; 1 failed.", parallel.stdout)

    def test_bounded_concurrency_overlap_and_timings(self):
        self.database([f"src/{name}.cpp" for name in ("a", "b", "c", "d", "e")])
        timings = self.base / "wall timings.json"
        result = self.run_checker("--jobs", "2", "--timings", str(timings))
        self.assertEqual(result.returncode, 0, result.stderr)
        events = sorted([(record["start"], 1) for record in self.records()] +
                        [(record["end"], -1) for record in self.records()])
        active = peak = 0
        for _, delta in events:
            active += delta
            peak = max(peak, active)
        self.assertEqual(peak, 2)
        report = json.loads(timings.read_text())
        self.assertEqual(report["jobs"], 2)
        self.assertGreater(report["total_seconds"], 0)
        self.assertEqual([Path(entry["source"]).name for entry in report["translation_units"]],
                         ["a.cpp", "b.cpp", "c.cpp", "d.cpp", "e.cpp"])
        self.assertTrue(all(entry["seconds"] > 0 and entry["returncode"] == 0
                            for entry in report["translation_units"]))

    def test_invalid_jobs_and_empty_database(self):
        self.database([])
        for value in ("0", "-1", "invalid"):
            with self.subTest(jobs=value):
                result = self.run_checker("--jobs", value)
                self.assertEqual(result.returncode, 2)
                self.assertIn("--jobs", result.stderr)
        result = self.run_checker()
        self.assertEqual(result.returncode, 2)
        self.assertIn("no project C++ sources", result.stderr)

    def test_missing_tool_cleans_temporary_output(self):
        self.database(["src/a.cpp", "src/b.cpp"])
        for jobs in ("1", "2"):
            result = self.run_checker("--jobs", jobs, tool=str(self.base / "missing executable"))
            self.assertEqual(result.returncode, 1)
            self.assertIn("FileNotFoundError", result.stderr)
        self.assertEqual(self.records(), [])

    def test_large_output_is_replayed_and_cleaned(self):
        self.database(["src/large.cpp", "src/z.cpp"])
        result = self.run_checker("--jobs", "2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("x"), 262144)
        self.assertEqual(result.stderr.count("y"), 262144)
        self.assertLess(result.stdout.index("x" * 100), result.stdout.index("out:z.cpp"))

    def test_unwritable_report_fails_after_all_checks(self):
        self.database(["src/a.cpp", "src/b.cpp"])
        result = self.run_checker("--jobs", "2", "--timings", str(self.base / "absent/report.json"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("FileNotFoundError", result.stderr)
        self.assertIn("Checked 2 translation units; 0 failed.", result.stdout)
        self.assertEqual(len(self.records()), 2)

    def test_failure_timings_and_single_worker(self):
        self.database(["src/a_fail.cpp", "src/b.cpp"])
        timings = self.base / "timings.json"
        result = self.run_checker("--timings", str(timings))
        self.assertEqual(result.returncode, 1, result.stderr)
        report = json.loads(timings.read_text())
        self.assertEqual(report["jobs"], 1)
        self.assertEqual([entry["returncode"] for entry in report["translation_units"]], [7, 0])
        records = sorted(self.records(), key=lambda record: record["start"])
        self.assertLessEqual(records[0]["end"], records[1]["start"])


if __name__ == "__main__":
    unittest.main()
