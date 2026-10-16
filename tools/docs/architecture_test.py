# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
"""Internal regression driver: python tools/docs/architecture_test.py.

Reads checked-in data/exports and pinned source through read-only Git commands.
Historical anchors are checked only when their Git revisions are available.
Source archives still run the portable export and viewer checks.
CLI fixtures use temporary directories. Returns unittest status; no browser or
network is used. Browser legibility and independent fact review are separate.
"""

import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
import architecture

ROOT = architecture.ROOT
DATA = json.loads(architecture.DEFAULT.read_text(encoding="utf-8"))
NS = {"s": "http://www.w3.org/2000/svg"}


class ArchitectureTests(unittest.TestCase):
    def cli(self, *args, cwd=None):
        return subprocess.run([sys.executable, str(ROOT / "tools/docs/architecture.py"), *args],
                              cwd=cwd or ROOT, capture_output=True, text=True,
                              encoding="utf-8", timeout=15,
                              env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))

    def test_help_has_no_workload_inputs_or_side_effects(self):
        with tempfile.TemporaryDirectory() as temporary:
            for flag in ("--help", "-h"):
                result = self.cli(flag, cwd=temporary)
                self.assertEqual(result.returncode, 0, result.stderr)
                for expected in ("--source", "--check", "Exit:", "No network", "Help performs no workload"):
                    self.assertIn(expected, result.stdout)
                self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_checked_in_exports_match_and_regeneration_is_stable(self):
        result = self.cli("--check")
        self.assertEqual(result.returncode, 0, result.stderr)
        generated = architecture.products(DATA)
        self.assertEqual(generated, architecture.products(copy.deepcopy(DATA)))
        for name, content in generated.items():
            self.assertEqual((architecture.DEFAULT.parent / name).read_bytes(), content.encode("utf-8"))

    def test_stale_export_check_does_not_repair_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary)
            source = path / "maps.json"
            source.write_text(json.dumps(DATA), encoding="utf-8")
            generated = self.cli("--source", str(source))
            self.assertEqual(generated.returncode, 0, generated.stderr)
            target = path / "index.html"
            target.write_text("stale", encoding="utf-8")
            checked = self.cli("--source", str(source), "--check")
            self.assertEqual(checked.returncode, 1)
            self.assertIn("stale export", checked.stderr)
            self.assertEqual(target.read_text(), "stale")

    def test_invalid_source_reference_is_rejected(self):
        invalid = copy.deepcopy(DATA)
        invalid["maps"][0]["nodes"][0]["source"]["path"] = "../private.txt"
        with self.assertRaisesRegex(ValueError, "public repository path"):
            architecture.validate(invalid)

    def test_duplicate_map_cannot_overwrite_an_export(self):
        invalid = copy.deepcopy(DATA)
        invalid["maps"].append(copy.deepcopy(invalid["maps"][0]))
        with self.assertRaisesRegex(ValueError, "duplicate or invalid map id"):
            architecture.validate(invalid)

    def test_references_exist_at_pinned_revision(self):
        paths = {(node["source"].get("revision", DATA["revision"]), node["source"]["path"]) for diagram in DATA["maps"] for node in diagram["nodes"]}
        if not (ROOT / ".git").exists() or shutil.which("git") is None:
            self.skipTest("historical anchor verification requires a Git checkout and Git")
        for revision in sorted({revision for revision, _ in paths}):
            available = subprocess.run(["git", "cat-file", "-e", revision + "^{commit}"],
                                       cwd=ROOT, capture_output=True, timeout=10)
            if available.returncode:
                self.skipTest("historical anchor revision unavailable: " + revision)
        sources = {}
        for revision, path in sorted(paths):
            result = subprocess.run(["git", "show", revision + ":" + path], cwd=ROOT,
                                    capture_output=True, encoding="utf-8", timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            sources[revision, path] = result.stdout.splitlines()
        for diagram in DATA["maps"]:
            for node in diagram["nodes"]:
                source = node["source"]
                lines = sources[source.get("revision", DATA["revision"]), source["path"]]
                self.assertLessEqual(source["line"], len(lines))
                self.assertTrue(lines[source["line"] - 1].strip(), node["id"])
                self.assertIn(source["anchor"], lines[source["line"] - 1], node["id"])

    def test_source_archive_runs_portable_checks_without_git_history(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / "source archive"
            tools = archive / "tools/docs"
            tools.mkdir(parents=True)
            for name in ("architecture.py", "architecture_test.py", "architecture_viewer.html"):
                shutil.copy2(ROOT / "tools/docs" / name, tools / name)
            snapshot = architecture.DEFAULT.relative_to(ROOT).parent
            shutil.copytree(ROOT / snapshot, archive / snapshot)
            source_paths = {node["source"]["path"] for diagram in DATA["maps"]
                            for node in diagram["nodes"]}
            for relative in source_paths:
                target = archive / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / relative, target)
            tests = ["ArchitectureTests." + name for name in
                     unittest.defaultTestLoader.getTestCaseNames(ArchitectureTests)
                     if name != "test_source_archive_runs_portable_checks_without_git_history"]
            result = subprocess.run([sys.executable, str(tools / "architecture_test.py"), *tests],
                                    cwd=archive, capture_output=True, encoding="utf-8", timeout=60,
                                    env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("skipped=1", result.stderr)
            self.assertIn("historical anchor verification requires", result.stderr)
            self.assertIn("Ran 8 tests", result.stderr)

    def test_svg_cards_do_not_overlap_and_text_stays_in_cards(self):
        for diagram in DATA["maps"]:
            for lang in ("en", "ru"):
                tree = ET.fromstring(architecture.svg(DATA, diagram, lang))
                _, _, width, height = map(float, tree.attrib["viewBox"].split())
                boxes = []
                for link in tree.findall("s:a", NS):
                    rect = link.find("s:rect", NS)
                    x, y, w, h = (float(rect.attrib[key]) for key in ("x", "y", "width", "height"))
                    self.assertLessEqual(x + w, width)
                    self.assertLessEqual(y + h, height)
                    for px, py, pw, ph in boxes:
                        self.assertTrue(x >= px + pw or px >= x + w or y >= py + ph or py >= y + h)
                    boxes.append((x, y, w, h))
                    for text in link.findall("s:text", NS):
                        tx, ty = float(text.attrib["x"]), float(text.attrib["y"])
                        self.assertTrue(x < tx < x + w and y < ty < y + h)
                        # Conservative em estimate catches wrapping regressions; browser QA checks real glyphs.
                        self.assertLessEqual(tx + len(text.text) * float(text.attrib["font-size"]) * .65, x + w)
                self.assertEqual(len(boxes), len(diagram["nodes"]))

    def test_offline_viewer_has_no_external_runtime_resources_or_private_paths(self):
        viewer = architecture.products(DATA)["index.html"]
        self.assertNotRegex(viewer, r'<(?:script|link|img)[^>]+(?:src|href)=["\']https?://')
        self.assertNotIn("fetch(", viewer)
        self.assertNotIn("XMLHttpRequest", viewer)
        self.assertNotIn("/*SNAPSHOT_DATA*/", viewer)
        self.assertIsNone(re.search(r"(?i)\.work[/\\]|\b[a-z]:[/\\]|account|prompt", viewer), "private data marker found")
        self.assertIn('id="lang"', viewer)
        self.assertIn('id="theme"', viewer)
        self.assertIn('id="export" download', viewer)


if __name__ == "__main__":
    unittest.main(verbosity=2)
