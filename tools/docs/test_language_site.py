"""Multilingual HTML regression fixtures. Run with unittest discovery in tools/docs.

Uses temporary sources, generated HTML and tags. No network or persistent output.
"""

import json
from pathlib import Path
import tempfile
import unittest

from language_site import anchor_map, finalize_languages


class LanguageSiteTests(unittest.TestCase):
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
            self.assertIn('href="ru/topic-ru.html"', english)
            self.assertIn('href="../topic.html"', russian)
            self.assertIn('Français (translation unavailable)', english)
            self.assertIn('&quot;settings&quot;: &quot;settings-ru&quot;', english)
            self.assertNotIn('C++ API — English', russian)
            self.assertIn('href="../dir_lib.html">Source <span lang="en">(English)</span>', russian)
            self.assertIn('href="../md__release_notes.html"', russian)
            redirect = (root / 'topic-ru.html').read_text(encoding='utf-8')
            self.assertIn('"ru/topic-ru.html"+location.search+location.hash', redirect)
            self.assertIn('href="ru/topic-ru.html"', redirect)
            self.assertTrue((root / 'home-ru.html').is_file())
            finalize_languages(root, source, tags)
            self.assertEqual((root / 'ru/topic-ru.html').read_text(encoding='utf-8'), russian)


if __name__ == '__main__':
    unittest.main()
