"""Regression fixtures for HTML links, CMake colors, navigation, search and language.

Run: python -m unittest discover -s tools/docs -p test_finalize_html.py -v
Uses temporary fixture files; no real HTML build, browser or network is required.
Unittest prints results and returns nonzero on failure. This is a test module,
not a production command; unittest owns its help and selection options.
"""

import json
from pathlib import Path
from html.parser import HTMLParser
import tempfile
import unittest

from finalize_html import GuideParser, color_cmake, complete_navigation, complete_search, document_language, finalize, portable_links, read_variable, type_links


class GuideTests(unittest.TestCase):
    def test_missing_config_search_fallback_is_guarded_and_idempotent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "search").mkdir()
            script = root / "search/search.js"
            original = ("function init_search() {}\n"
                        "document.addEventListener('DOMContentLoaded', () => {\n"
                        "  const configElem = document.getElementById('doxygen-config');\n"
                        "  if (configElem) { init_search(); }\n});\n")
            script.write_text(original)
            complete_search(root)
            rendered = script.read_text()
            self.assertTrue(rendered.startswith(original))
            self.assertIn("!document.getElementById('doxygen-config')", rendered)
            self.assertIn("document.getElementById('MSearchField')", rendered)
            self.assertIn("typeof searchBox !== 'undefined'", rendered)
            complete_search(root)
            self.assertEqual(script.read_text(), rendered)
            script.write_text("function init_search() { /* Different generator. */ }")
            complete_search(root)
            self.assertEqual(script.read_text(), "function init_search() { /* Different generator. */ }")

    def test_document_language_preserves_code_links_and_other_root_attributes(self):
        text = ('<html xmlns="http://www.w3.org/1999/xhtml" lang="en-US" xml:lang="en-US">'
                '<a href="guide.html">Guide</a><code>lang="en-US"</code></html>')
        rendered = document_language(text, "ru")
        self.assertIn('xmlns="http://www.w3.org/1999/xhtml" lang="ru" xml:lang="ru"', rendered)
        self.assertIn('<a href="guide.html">Guide</a><code>lang="en-US"</code>', rendered)
        self.assertEqual(document_language(rendered, "ru"), rendered)

    def test_finalize_sets_language_for_both_russian_page_naming_forms(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            names = ["index.html", "first-application-ru.html", "md_docs_2ru_2guide.html"]
            tag = root / "pages.tag"
            tag.write_text('<tagfile>' + ''.join(
                '<compound kind="page"><filename>' + name + '</filename></compound>'
                for name in names) + '</tagfile>', encoding="utf-8")
            (root / "navtreedata.js").write_text(
                'var NAVTREE = [["Project", "index.html", []]];\nvar NAVTREEINDEX = [];\n')
            for name in names:
                (root / name).write_text('<html lang="en-US"><body>Guide</body></html>')
            finalize(root, tag)
            for name in names:
                language = "en" if name == "index.html" else "ru"
                self.assertIn(f'lang="{language}"', (root / name).read_text())
                self.assertIn(f'xml:lang="{language}"', (root / name).read_text())

    def test_repository_links_work_in_a_portable_html_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "source", root / "html"
            (source / "docs/examples").mkdir(parents=True)
            (source / "include").mkdir()
            output.mkdir()
            (source / "docs/examples/demo.cpp").write_text("int main() {}")
            (source / "include/tree.hpp").write_text("class WidgetTree {};")
            (source / "docs/guide.md").write_text("# Guide {#guide}\n")
            (output / "guide.html").write_text('<a id="section"></a>')
            text = ('<a href="../include/tree.hpp">WidgetTree</a>'
                    '<a href="examples/demo.cpp">Source</a>'
                    '<a href="guide.md#section">Guide</a>')
            result = portable_links(text, output, source, {"WidgetTree": "type.html"})
            self.assertIn('href="type.html"', result)
            self.assertIn('href="source/docs/examples/demo.cpp"', result)
            self.assertIn('href="guide.html#section"', result)
            self.assertEqual((output / "source/docs/examples/demo.cpp").read_text(), "int main() {}")

    def test_flat_assets_preserve_query_fragment_attributes_and_idempotence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "checkout", root / "html"
            source.mkdir()
            assets = output / "architecture/snapshots/m1"
            assets.mkdir(parents=True)
            (assets / "index.html").write_text("viewer", encoding="utf-8")
            (assets / "ownership-ru.svg").write_text("<svg/>", encoding="utf-8")
            checkout_assets = source / "docs/architecture/snapshots/m1"
            checkout_assets.mkdir(parents=True)
            (checkout_assets / "ownership-ru.svg").write_text("<svg/>", encoding="utf-8")
            text = ('<a class="viewer" href="../architecture/snapshots/m1/index.html?lang=ru&amp;theme=dark#map">Open</a>'
                    '<a href="../architecture/snapshots/m1/ownership-ru.svg">SVG</a>'
                    '<img alt="Diagram" src="../architecture/snapshots/m1/ownership-ru.svg">'
                    '<a href="../missing.html">Missing</a>')
            result = portable_links(text, output, source, {}, russian=True)
            self.assertIn('class="viewer" href="architecture/snapshots/m1/index.html?lang=ru&amp;theme=dark#map"', result)
            self.assertIn('href="architecture/snapshots/m1/ownership-ru.svg"', result)
            self.assertIn('alt="Diagram" src="architecture/snapshots/m1/ownership-ru.svg"', result)
            self.assertIn('href="../missing.html"', result)
            self.assertEqual(portable_links(result, output, source, {}, russian=True), result)
            self.assertFalse((output / "source").exists())

    def test_inline_types_preserve_existing_links_and_code(self):
        parser = GuideParser({"WidgetTree": "tree.html"})
        parser.feed('<p><span class="tt">WidgetTree</span></p>'
                    '<a href="existing"><code>WidgetTree</code></a>'
                    '<pre><code>WidgetTree</code></pre>')
        rendered = "".join(parser.output)
        self.assertIn('<a class="el" href="tree.html"><span class="tt">WidgetTree</span></a>', rendered)
        self.assertIn('<a href="existing"><code>WidgetTree</code></a>', rendered)
        self.assertIn('<pre><code>WidgetTree</code></pre>', rendered)
        self.assertEqual(rendered.count('href="tree.html"'), 1)

    def test_ambiguous_short_type_names_are_not_linked(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "types.tag"
            path.write_text('<tagfile><compound kind="class"><name>a::Item</name>'
                            '<filename>a.html</filename></compound><compound kind="class">'
                            '<name>b::Item</name><filename>b.html</filename></compound></tagfile>')
            links = type_links(path)
            self.assertNotIn("Item", links)
            self.assertEqual(links["a::Item"], "a.html")

    def test_cmake_colors_do_not_change_the_example(self):
        text = '<div class="fragment"><div class="line">project(App LANGUAGES CXX)</div>\n<div class="line"># note</div>\n</div>'
        rendered = color_cmake(text)
        self.assertIn('<span class="keyword">project</span>', rendered)
        self.assertIn('<span class="comment"># note</span>', rendered)
        self.assertEqual(color_cmake(rendered), rendered)
        class Text(HTMLParser):
            def __init__(self):
                super().__init__()
                self.output = []

            def handle_data(self, data):
                self.output.append(data)
        before, after = Text(), Text()
        before.feed(text)
        after.feed(rendered)
        self.assertEqual("".join(before.output), "".join(after.output))

    def test_guide_page_and_sections_have_navigation_index_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            tree = [["Project", "index.html", [["CLI", "cli.html#md_docs_2cli", None]]]]
            (root / "navtreedata.js").write_text('var NAVTREE = ' + json.dumps(tree) + ';\nvar NAVTREEINDEX = ["types.html"];\n')
            (root / "navtreeindex0.js").write_text('var NAVTREEINDEX0 = {"types.html":[1,2]};\n')
            complete_navigation(root, {"cli.html": [(1, "options", "Options")]})
            nav, _ = read_variable((root / "navtreedata.js").read_text(), "NAVTREE")
            index, _ = read_variable((root / "navtreeindex0.js").read_text(), "NAVTREEINDEX0")
            self.assertEqual(nav[0][2][0][2], [["Options", "cli.html#options", None]])
            self.assertEqual(index["cli.html"], [0])
            self.assertEqual(index["cli.html#options"], [0, 0])
            self.assertEqual(index["types.html"], [1, 2])
            # Re-running finalization must preserve the same tree and not nest sections recursively.
            first = (root / "navtreedata.js").read_bytes()
            complete_navigation(root, {"cli.html": [(1, "options", "Options")]})
            self.assertEqual((root / "navtreedata.js").read_bytes(), first)


if __name__ == "__main__":
    unittest.main()
