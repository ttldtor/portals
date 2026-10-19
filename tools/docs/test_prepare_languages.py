"""Test locale layouts and validated configure metadata with temporary fixtures.

Run: python -m unittest discover -s tools/docs -p test_prepare_languages.py -v
No Doxygen, network, or real build is used; unittest exits nonzero on failure.
"""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET

from prepare_languages import filter_document, prepare


class LanguageBuildTests(unittest.TestCase):
    def fixture(self, directory):
        root = Path(directory)
        manifest = root / "languages.json"
        languages = [
            {"code": "ru", "name": "Русский", "directory": "ru", "source": "docs/ru",
             "main": "docs/ru/index.md", "output_language": "Russian"},
            {"code": "en", "name": "English", "directory": "", "source": "docs",
             "main": "docs/index.md", "output_language": "English"},
            {"code": "de", "name": "Deutsch", "directory": "de", "source": "docs/de",
             "main": "docs/de/index.md", "output_language": "German"},
        ]
        config = {"default": "en", "languages": languages}
        manifest.write_text(json.dumps(config), encoding="utf-8")
        layout = root / "layout.xml"
        layout.write_text('<doxygenlayout version="2.0">'
                          '<navindex language="en"><tab title="English"/></navindex>'
                          '<navindex language="ru"><tab title="Русский"/></navindex>'
                          '<navindex language="de"><tab title="Deutsch"/></navindex>'
                          '<class><briefdescription/></class></doxygenlayout>', encoding="utf-8")
        return manifest, layout, root / "output", config

    def test_locales_have_separate_navigation_and_shared_member_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, layout, output, _ = self.fixture(directory)
            self.assertEqual(prepare(manifest, layout, output), ["en", "ru", "de"])
            for code, title in [("en", "English"), ("ru", "Русский"), ("de", "Deutsch")]:
                root = ET.parse(output / f"layout-{code}.xml").getroot()
                self.assertEqual(len(root.findall("navindex")), 1)
                self.assertEqual(root.find("navindex").attrib, {})
                self.assertEqual(root.find("navindex/tab").get("title"), title)
                self.assertIsNotNone(root.find("class/briefdescription"))
            self.assertIn('set(portals_doc_languages "en;ru;de")',
                          (output / "languages.cmake").read_text(encoding="utf-8"))

    def test_invalid_manifest_never_writes_output(self):
        mutations = [
            lambda config: config.update(default="missing"),
            lambda config: config["languages"][0].update(code="en"),
            lambda config: config["languages"][0].update(directory="../outside"),
            lambda config: config["languages"][0].update(source="docs;message(FATAL_ERROR)"),
            lambda config: config["languages"][0].update(main="elsewhere/index.md"),
            lambda config: config["languages"][1].update(directory="english"),
            lambda config: config["languages"][0].update(directory="de"),
            lambda config: config["languages"][0].update(directory="."),
            lambda config: config["languages"][0].update(directory="de/"),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                manifest, layout, output, config = self.fixture(directory)
                mutation(config)
                manifest.write_text(json.dumps(config), encoding="utf-8")
                with self.assertRaises(ValueError):
                    prepare(manifest, layout, output)
                self.assertFalse(output.exists())

    def test_missing_layout_is_reported_before_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, layout, output, _ = self.fixture(directory)
            layout.write_text('<doxygenlayout><navindex language="en"/></doxygenlayout>')
            with self.assertRaisesRegex(ValueError, "navindex"):
                prepare(manifest, layout, output)
            self.assertFalse(output.exists())

    def test_help_has_no_operational_side_effects(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(Path(__file__).with_name("prepare_languages.py")),
                                     "--help"], cwd=directory, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--output", result.stdout)
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_filter_preserves_code_and_links_to_the_current_language(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, _, _, _ = self.fixture(directory)
            root = Path(directory)
            (root / "docs").mkdir()
            page = root / "docs/page.md"
            original = ('[Русский](ru/page.md#section) [Local](other.md)\n'
                        '`[literal](ru/page.md)`\n```md\n[code](ru/page.md)\n```\n'
                        '~~~md\n[code](ru/page.md)\n~~~\n')
            page.write_text(original, encoding="utf-8")
            filtered = filter_document(manifest, root, "en", page)
            self.assertEqual(filtered, original.replace('[Русский](ru/page.md#section)',
                                                       '<a href="ru/page.md#section">Русский</a>'))
            self.assertEqual(page.read_text(encoding="utf-8"), original)
            self.assertEqual(filtered.count('\n'), original.count('\n'))

    def test_filter_uses_source_paths_for_an_additional_language(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, _, _, _ = self.fixture(directory)
            root = Path(directory)
            (root / "docs/de").mkdir(parents=True)
            page = root / "docs/de/page.md"
            page.write_text('[English](../page.md) [Local](other.md) [Web](https://example.org/a.md)',
                            encoding="utf-8")
            self.assertEqual(filter_document(manifest, root, "de", page),
                             '<a href="../page.md">English</a> [Local](other.md) '
                             '[Web](https://example.org/a.md)')


if __name__ == "__main__":
    unittest.main()
