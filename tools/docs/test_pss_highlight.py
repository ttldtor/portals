"""PSS HTML regression fixtures: python -m unittest discover -s tools/docs -p test_pss_highlight.py -v.

Uses repository Markdown and in-memory HTML; no browser, build, or network.
Unittest owns help and returns nonzero on failure.
"""

import html
from html.parser import HTMLParser
from pathlib import Path
import re
import tempfile
import unittest

from pss_highlight import color_pss


def fragment(source):
    return '<div class="fragment">' + '\n'.join(
        '<div class="line">' + html.escape(line) + '</div>'
        for line in source.split('\n')) + '</div>'


class TextParser(HTMLParser):
    def __init__(self):
        super().__init__()
        self.text = []

    def handle_data(self, data):
        self.text.append(data)


def text(value):
    parser = TextParser()
    parser.feed(value)
    return ''.join(parser.text)


class PssHighlightTests(unittest.TestCase):
    def test_finalize_pipeline_colors_pss_and_is_idempotent(self):
        from finalize_html import finalize

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tag = root / 'pages.tag'
            tag.write_text('<tagfile><compound kind="page"><filename>index.html</filename>'
                           '</compound></tagfile>', encoding='utf-8')
            (root / 'navtreedata.js').write_text(
                'var NAVTREE = [["Project", "index.html", []]];\nvar NAVTREEINDEX = [];\n')
            page = root / 'index.html'
            original = fragment('Button { bold: true; }')
            page.write_text('<html><body>' + original + '</body></html>', encoding='utf-8')
            finalize(root, tag)
            result = page.read_text(encoding='utf-8')
            self.assertIn('fragment pss-colors', result)
            self.assertIn(text(original), text(result))
            finalize(root, tag)
            self.assertEqual(page.read_text(encoding='utf-8'), result)

    def assert_preserved(self, original):
        result = color_pss(original)
        self.assertEqual(text(result), text(original))
        self.assertEqual(color_pss(result), result)
        self.assertEqual(result.count('<div class="line">'), original.count('<div class="line">'))
        return result

    def test_actual_english_and_russian_grammar_examples(self):
        root = Path(__file__).resolve().parents[2]
        for name in ('docs/styles.md', 'docs/ru/styles.md'):
            source = (root / name).read_text(encoding='utf-8')
            examples = re.findall(r'```css\n(.*?)\n```', source, re.S)
            self.assertTrue(examples, name)
            for example in examples:
                with self.subTest(page=name, example=example):
                    result = self.assert_preserved(fragment(example))
                    self.assertIn('fragment pss-colors', result)
                    self.assertIn('<span class="comment">', result)
                    self.assertIn('<span class="keywordtype">Button</span>', result)
                    self.assertIn('<span class="preprocessor">.primary</span>', result)
                    self.assertIn('<span class="preprocessor">#save</span>', result)
                    self.assertIn('<span class="preprocessor">:focused</span>', result)
                    self.assertIn('<span class="keyword">foreground</span>', result)
                    self.assertIn('<span class="stringliteral">#dce1e8</span>', result)

    def test_multiline_comments_entities_and_copy_text_are_preserved(self):
        source = '/* <script> & "quotes"\n } { text */\nButton { /* body\n comment */ bold: true; }'
        result = self.assert_preserved(fragment(source))
        self.assertIn('pss-colors', result)
        self.assertEqual(result.count('<span class="comment">'), 4)
        self.assertIn('&lt;script&gt;', result)
        self.assertNotIn('<script>', result)

    def test_supported_selector_orders_and_values(self):
        source = '#save.primary:focused { bold: false; }\n:caret { background: #Aa01Ff; }\n_Thing-name { min-width: 2147483647; }\n* { min-width: -0; }'
        result = self.assert_preserved(fragment(source))
        self.assertIn('pss-colors', result)
        self.assertIn('<span class="keyword">false</span>', result)
        self.assertIn('<span class="stringliteral">2147483647</span>', result)

    def test_quoted_scrollbar_glyphs_preserve_copy_text(self):
        source = ('ScrollArea {\n'
                  '  scrollbar-horizontal-thumb-start-glyph: "[";\n'
                  '  scrollbar-horizontal-thumb-body-glyph: " ";\n'
                  "  scrollbar-vertical-thumb-body-glyph: '│';\n"
                  '  scrollbar-corner-glyph: "";\n}')
        result = self.assert_preserved(fragment(source))
        self.assertIn('fragment pss-colors', result)
        self.assertEqual(result.count('<span class="stringliteral">'), 4)

    def test_unknown_or_over_budget_glyph_syntax_is_not_colored(self):
        for declaration in ('scrollbar-diagonal-track-glyph: "x";',
                            'scrollbar-horizontal-track-glyph: x;',
                            'scrollbar-horizontal-track-glyph: "' + 'x' * 65 + '";'):
            with self.subTest(declaration=declaration):
                original = fragment('ScrollArea { ' + declaration + ' }')
                self.assertEqual(color_pss(original), original)

    def test_non_pss_fragments_and_invalid_pss_are_unchanged(self):
        examples = [
            'int main() { return 0; }',
            'auto styles = R"(Button { bold: true; })";',
            '{"foreground": "#ffffff"}',
            'cmake --build build',
            'Button:hover { bold: true; }',
            'Button > Label { bold: true; }',
            'Button Label { bold: true; }',
            'Button, Label { bold: true; }',
            'Button { color: #ffffff; }',
            'Button { bold: true }',
            'Button { foreground: #fff; }',
            'Button { min-width: 2147483648; }',
            'Button { min-width: -1; }',
            'Button:focused { min-width: 20; }',
            'Button#a#b { bold: true; }',
            'Button:normal:focused { bold: true; }',
            '/* Button { bold: true; } */',
            '/* unterminated\nButton { bold: true; }',
        ]
        for source in examples:
            with self.subTest(source=source):
                original = fragment(source)
                self.assertEqual(color_pss(original), original)

    def test_existing_links_highlighting_and_other_markup_are_protected(self):
        for markup in ['<a href="Button.html">Button</a>',
                       '<span class="keywordtype">Button</span>', '<b>Button</b>']:
            original = '<div class="fragment"><div class="line">' + markup + ' { bold: true; }</div></div>'
            self.assertEqual(color_pss(original), original)

    def test_many_leading_zeroes_do_not_exceed_python_integer_limit(self):
        result = self.assert_preserved(fragment('* { min-width: ' + '0' * 5000 + '20; }'))
        self.assertIn('pss-colors', result)


if __name__ == '__main__':
    unittest.main()
