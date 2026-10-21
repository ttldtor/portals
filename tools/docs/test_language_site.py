"""Multilingual HTML regression fixtures. Run with unittest discovery in tools/docs.

Uses temporary sources, generated HTML and tags. No network or persistent output.
"""

import json
from pathlib import Path
import tempfile
import unittest

from language_site import anchor_map, development_label, finalize_languages, navigation_links, page_fragment, public_type_index


class LanguageSiteTests(unittest.TestCase):
    def test_development_header_retains_accessible_build_identity(self):
        identity = "0.5.0 · dev (0123456789ab · 2026-10-21 UTC)"
        result = development_label(f'<span id="projectnumber">&#160;{identity}</span>')
        self.assertIn('<span aria-hidden="true">&#160;0.5.0 · dev</span>', result)
        self.assertIn(f'title="{identity}"', result)
        self.assertIn(f'<span class="portals-visually-hidden">{identity}</span>', result)
        self.assertNotIn('aria-label=', result)
        self.assertEqual(development_label(result), result)

    def test_release_and_preview_labels_are_unchanged(self):
        for label in ("0.5.0", "PR 70 preview"):
            content = f'<span id="projectnumber">{label}</span>'
            self.assertEqual(development_label(content), content)

    def test_public_type_index_retains_full_internal_reference(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            content = ('<div class="contents"><table><tr id="row_0_"><td>Public</td></tr>'
                       '<tr id="row_0_1_"><td><span class="icon">N</span><b>detail</b></td></tr>'
                       '<tr id="row_0_1_0_"><td>Internal</td></tr>'
                       '<tr id="row_0_2_"><td>Button</td></tr></table></div>')
            (root / 'annotated.html').write_text(content)
            public_type_index(root)
            public = (root / 'annotated.html').read_text()
            self.assertIn('Button', public)
            self.assertNotIn('Internal', public)
            self.assertIn('annotated-all.html', public)
            self.assertEqual((root / 'annotated-all.html').read_text(), content)
            public_type_index(root)
            self.assertEqual((root / 'annotated.html').read_text(), public)
            self.assertEqual((root / 'annotated-all.html').read_text(), content)

    def test_whole_page_anchor_preserves_sections(self):
        page = {"source": "docs/architecture.md"}
        self.assertEqual(page_fragment("md_docs_2architecture", page), "")
        self.assertEqual(page_fragment("ownership", page), "ownership")
        urls = {"architecture.html": page}
        original = 'var pages = [["Overview", "architecture.html#md_docs_2architecture"], ["Owners", "architecture.html#ownership"]];'
        expected = 'var pages = [["Overview", "architecture.html"], ["Owners", "architecture.html#ownership"]];'
        self.assertEqual(navigation_links(original, "", urls), expected)
        self.assertEqual(navigation_links(expected, "", urls), expected)
        self.assertEqual(anchor_map('<a id="md_docs_2architecture"></a>',
                                    '<a id="md_docs_2ru_2architecture"></a>', 'en', 'ru'), {})

    def test_sections_require_explicit_unambiguous_ids(self):
        mapping = anchor_map('<a id="settings"></a><a id="autotoc_md12"></a>',
                             '<a id="settings-ru"></a><a id="autotoc_md12"></a>', 'en', 'ru')
        self.assertEqual(mapping, {'settings': 'settings-ru'})

    def test_page_switch_cross_links_missing_translation_and_legacy_redirect(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'checkout'
            root = Path(temporary) / 'site'
            locales = []
            tags = {}
            for code, name in [('en', 'English'), ('ru', 'Русский'), ('fr', 'Français')]:
                directory = '' if code == 'en' else code
                prefix = 'docs' if code == 'en' else 'docs/' + code
                (source / prefix).mkdir(parents=True, exist_ok=True)
                (root / directory).mkdir(parents=True, exist_ok=True)
                images = root / directory / 'images/turbo'
                images.mkdir(parents=True)
                svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 760 200"><title>Components &amp; flow</title></svg>'
                (root / directory / 'components.svg').write_text(svg)
                (source / 'docs/images/turbo').mkdir(parents=True, exist_ok=True)
                (source / 'docs/images/turbo/components.svg').write_text(svg)
                locales.append(dict(code=code, name=name, directory=directory,
                                    source=prefix, main=prefix + '/index.md'))
                pages = [('index', 'index.html')]
                if code != 'fr':
                    pages.append(('topic', 'topic' + ('-ru' if code == 'ru' else '') + '.html'))
                compounds = []
                for topic, filename in pages:
                    (source / prefix / (topic + '.md')).write_text(
                        '# Title {#' + ('home-' + code if topic == 'index' else filename[:-5]) + '}\n', encoding='utf-8')
                    body = '<html><head></head><body><div id="titlearea"></div>'
                    credit = 'Создано системой' if code == 'ru' else 'Generated by'
                    body += f'<ul><li class="footer">{credit} <a href="https://www.doxygen.org/">Doxygen</a> 1.18.0</li><li>Keep navigation</li></ul>'
                    body += '<object type="image/svg+xml" data="components.svg" style="pointer-events: none;"></object>'
                    if topic == 'topic':
                        body += '<a id="settings' + ('-ru' if code == 'ru' else '') + '"></a>'
                        body += '<a href="' + ('../topic.md' if code == 'ru' else 'ru/topic.md') + '">Other</a>'
                    (root / directory / filename).write_text(body + '</body></html>', encoding='utf-8')
                    compounds.append('<compound kind="page"><filename>' + filename + '</filename></compound>')
                tags[code] = source / (code + '.tag')
                tags[code].write_text('<tagfile>' + ''.join(compounds) + '</tagfile>', encoding='utf-8')
            (source / 'docs/languages.json').write_text(json.dumps(dict(default='en', languages=locales)), encoding='utf-8')
            (source / 'vendor/lib').mkdir(parents=True)
            (source / 'vendor/lib/README.md').write_text('# Library source record')
            (source / 'ReleaseNotes.md').write_text('# Release notes')
            extras = ('<compound kind="dir"><path>vendor/lib/</path><filename>dir_lib.html</filename></compound>'
                      '<compound kind="page"><name>md__release_notes</name><filename>md__release_notes.html</filename></compound>')
            tags['en'].write_text(tags['en'].read_text().replace('</tagfile>', extras + '</tagfile>'))
            for filename in ('dir_lib.html', 'md__release_notes.html'):
                (root / filename).write_text('<html><head></head><div id="titlearea"></div></html>')
            translated = root / 'ru/topic-ru.html'
            translated.write_text(translated.read_text(encoding='utf-8').replace('</body>',
                                  '<a href="../../vendor/lib/README.md">Source</a>'
                                  '<a href="../../ReleaseNotes.md">Release notes</a></body>'), encoding='utf-8')
            (root / 'annotated.html').write_text('<html><head></head><body><div id="titlearea"></div></body></html>', encoding='utf-8')
            finalize_languages(root, source, tags)
            english = (root / 'topic.html').read_text(encoding='utf-8')
            russian = (root / 'ru/topic-ru.html').read_text(encoding='utf-8')
            for rendered in (english, russian):
                self.assertNotIn('Generated by', rendered)
                self.assertNotIn('Создано системой', rendered)
                self.assertIn('<li>Keep navigation</li>', rendered)
                self.assertEqual(rendered.count('class="turbo-diagram"'), 1)
                self.assertIn('class="turbo2-diagram"', rendered)
                self.assertIn('theme-diagrams/turbo/', rendered)
                self.assertIn('aria-label="Components &amp; flow"', rendered)
                self.assertIn('<object class="standard-diagram"', rendered)
            self.assertIn('href="ru/topic-ru.html"', english)
            self.assertIn('href="../topic.html"', russian)
            self.assertIn('aria-current="page" title="English" aria-label="English">EN</a>', english)
            self.assertIn('aria-label="Русский"', english)
            self.assertIn('data-label="Color scheme"', english)
            self.assertIn('data-label="Цветовая схема"', russian)
            self.assertIn('data-system="Системная"', russian)
            self.assertIn('data-turbo="Turbo Vision"', russian)
            self.assertIn('data-turbo="Turbo Vision"', english)
            self.assertIn('data-turbo2="Turbo2"', russian)
            self.assertIn('data-turbo2="Turbo2"', english)
            self.assertEqual(english.count('<script src="turbo-scrollbars.js" defer></script>'), 1)
            self.assertIn('<button type="button" class="portals-appearance" hidden ', english)
            self.assertNotIn('<select', english)
            self.assertEqual(english.count('<script src="appearance.js"></script>'), 1)
            self.assertLess(english.index('appearance.js'), english.index('</head>'))
            self.assertIn('Français (translation unavailable)', english)
            self.assertIn('&quot;settings&quot;: &quot;settings-ru&quot;', english)
            self.assertNotIn('C++ API — English', russian)
            self.assertIn('href="../dir_lib.html">Source <span lang="en">(English)</span>', russian)
            self.assertIn('href="../md__release_notes.html"', russian)
            redirect = (root / 'topic-ru.html').read_text(encoding='utf-8')
            self.assertIn('"ru/topic-ru.html"+location.search+location.hash', redirect)
            self.assertIn('href="ru/topic-ru.html"', redirect)
            self.assertTrue((root / 'home-ru.html').is_file())
            api = (root / 'annotated.html').read_text(encoding='utf-8')
            self.assertIn('href="ru/index.html"', api)
            self.assertIn('aria-label="Русские руководства"', api)
            finalize_languages(root, source, tags)
            self.assertEqual((root / 'ru/topic-ru.html').read_text(encoding='utf-8'), russian)


if __name__ == '__main__':
    unittest.main()
