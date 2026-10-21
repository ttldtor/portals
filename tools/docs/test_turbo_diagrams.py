"""Diagram semantics and locale packaging tests; unittest discovery in tools/docs.

Uses temporary site/source trees. Mermaid rendering is covered by the actual
documentation build; these tests require no browser, Node process or network.
"""

import json
from pathlib import Path
import tempfile
import unittest
import xml.etree.ElementTree as ET

from turbo_diagrams import (DiagramCatalog, PALETTES, SVG, _inheritance_graphs,
                            _inheritance_svg, _snapshot_svg, _wrap_mermaid_labels,
                            prepare_diagrams, rewrite_diagrams)


ROOT = Path(__file__).resolve().parents[2]


class TurboDiagramTests(unittest.TestCase):
    def test_snapshot_variants_retain_every_node_edge_description_and_source(self):
        data = json.loads((ROOT / 'docs/architecture/snapshots/m1-968651a/maps.json').read_text(encoding='utf-8'))
        for diagram in data['maps']:
            for language in ('en', 'ru'):
                for palette in PALETTES.values():
                    with self.subTest(map=diagram['id'], language=language, palette=palette):
                        svg = ET.fromstring(_snapshot_svg(data, diagram, language, palette))
                        links = svg.findall(SVG + 'a')
                        self.assertEqual([link.get('data-node') for link in links],
                                         [node['id'] for node in diagram['nodes']])
                        for link, node in zip(links, diagram['nodes']):
                            source = node['source']
                            revision = source.get('revision', data['revision'])
                            self.assertEqual(link.get('href'), f'https://github.com/ttldtor/portals/blob/{revision}/{source["path"]}#L{source["line"]}')
                            self.assertEqual(link.findtext(SVG + 'title'), node['detail'][language])
                            text = ' '.join(element.text or '' for element in link.findall(SVG + 'text'))
                            self.assertEqual(' '.join(node['title'][language].split()),
                                             text[:len(' '.join(node['title'][language].split()))])
                            self.assertEqual(link.get('aria-label'), node['title'][language] + ': ' + node['label'])
                        paths = [path for path in svg.findall(SVG + 'path') if path.get('data-from')]
                        self.assertEqual([(path.get('data-from'), path.get('data-to'), path.findtext(SVG + 'title'))
                                          for path in paths],
                                         [(edge['from'], edge['to'], edge['label'][language])
                                          for edge in diagram.get('edges', [])])
                        self.assertNotIn('rx=', ET.tostring(svg, encoding='unicode'))

    def test_locale_rewrite_retains_original_alt_and_live_source_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            variant = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 760 240"><title>Diagram</title></svg>'
            variants = {}
            for theme in PALETTES:
                path = root / 'theme-diagrams' / theme / 'architecture' / 'map.svg'
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(variant, encoding='utf-8')
                variants[theme] = path.relative_to(root).as_posix()
            catalog = DiagramCatalog(root, {'architecture/map.svg': variants})
            original = '<img class="inline" src="../architecture/map.svg" alt="Связи &amp; роли"/>'
            rewritten = rewrite_diagrams(original, root / 'ru/architecture-ru.html', catalog)
            self.assertIn('class="standard-diagram inline"', rewritten)
            self.assertIn('src="../architecture/map.svg" alt="Связи &amp; роли"', rewritten)
            self.assertEqual(rewritten.count('aria-label="Связи &amp; роли"'), 2)
            self.assertIn('data="../theme-diagrams/turbo/architecture/map.svg"', rewritten)
            self.assertIn('data="../theme-diagrams/turbo2/architecture/map.svg"', rewritten)
            self.assertIn('style="--turbo-diagram-width: 760px"', rewritten)
            self.assertEqual(rewrite_diagrams(rewritten, root / 'ru/architecture-ru.html', catalog), rewritten)

    def test_inventory_covers_used_diagrams_and_leaves_screenshots_untouched(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, source = Path(temporary) / 'site', Path(temporary) / 'source'
            root.mkdir()
            (source / 'docs/images/turbo').mkdir(parents=True)
            svg = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 760 240"><title>Flow</title><text x="20" y="40">A → B</text></svg>'
            (root / 'components.svg').write_text(svg, encoding='utf-8')
            (source / 'docs/images/turbo/components.svg').write_text(svg, encoding='utf-8')
            original = '<object data="components.svg" type="image/svg+xml"></object><img src="app.png" alt="Application"/>'
            (root / 'page.html').write_text(original, encoding='utf-8')
            catalog = prepare_diagrams(root, source)
            self.assertEqual(len(catalog.inventory), 1)
            self.assertEqual(catalog.inventory[0]['kind'], 'architecture')
            result = rewrite_diagrams(original, root / 'page.html', catalog)
            self.assertIn('<img src="app.png" alt="Application"/>', result)
            self.assertEqual(result.count('class="turbo-diagram"'), 1)
            self.assertEqual((root / 'components.svg').read_text(encoding='utf-8'), svg)

    def test_missing_optional_architecture_sources_do_not_block_minimal_sites(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'page.html').write_text('<p>No diagrams</p>', encoding='utf-8')
            catalog = prepare_diagrams(root, root / 'missing-checkout')
            self.assertEqual(catalog.assets, {})
            self.assertEqual(rewrite_diagrams('<p>No diagrams</p>', 'page.html', catalog), '<p>No diagrams</p>')

    def test_inheritance_retains_multiple_bases_template_aliases_children_and_links(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tag = root / 'types.tag'
            tag.write_text('<tagfile>'
                '<compound kind="class"><name>api::Base</name><filename>classBase.html</filename></compound>'
                '<compound kind="class"><name>api::Other</name><filename>classOther.html</filename></compound>'
                '<compound kind="class"><name>api::Diamond</name><filename>classDiamond.html</filename>'
                '<base>api::Base&lt; T &gt;</base><base>api::Other</base></compound>'
                '<compound kind="class"><name>api::Child</name><filename>classChild.html</filename>'
                '<base>api::Diamond</base></compound></tagfile>')
            page = root / 'classDiamond.html'
            page.write_text('<div class="dynheader">Inheritance diagram for api::Diamond:</div>'
                '<img src="classDiamond.png" alt="" usemap="#types"/>'
                '<map id="types" name="types">'
                '<area href="classBase.html" alt="api::Base&lt; T &gt;" title="Templated base"/>'
                '<area href="classOther.html" alt="api::Other" title="Other base"/>'
                '<area href="classChild.html" alt="api::Child" title="Derived child"/></map>')
            graph, current = _inheritance_graphs(root, {'en': tag})[root / 'classDiamond.png']
            self.assertEqual(len(graph['nodes']), 4)
            self.assertEqual({(edge['from'], edge['to']) for edge in graph['edges']},
                             {('classDiamond.html', 'classBase.html'),
                              ('classDiamond.html', 'classOther.html'),
                              ('classChild.html', 'classDiamond.html')})
            for palette in PALETTES.values():
                svg = ET.fromstring(_inheritance_svg(graph, current,
                                     'theme-diagrams/turbo/classDiamond.svg', palette))
                self.assertEqual({link.get('href') for link in svg.findall(SVG + 'a')},
                                 {'../../classBase.html', '../../classOther.html',
                                  '../../classChild.html', '../../classDiamond.html'})
                self.assertTrue(all(link.get('target') == '_top' for link in svg.findall(SVG + 'a')))
                self.assertEqual(len([path for path in svg.findall(SVG + 'path') if path.get('data-from')]), 3)

    def test_unlinked_external_base_is_recovered_from_exact_source_declaration(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            tag = root / 'types.tag'
            tag.write_text('<tagfile><compound kind="struct"><name>api::Trait&lt; Box&lt; T &gt; &gt;</name>'
                           '<filename>structTrait.html</filename></compound></tagfile>')
            (root / 'structTrait.html').write_text(
                '<a href="trait_source.html">Header</a>'
                '<div class="dynheader">Inheritance diagram for api::Trait&lt; Box&lt; T &gt; &gt;:</div>'
                '<img src="structTrait.png" alt=""/>')
            (root / 'trait_source.html').write_text(
                '<div><span class="lineno">42</span>struct Trait&lt;Box&lt;T&gt;&gt; '
                ': std::integral_constant&lt;bool, true&gt; {};</div>')
            graph, _ = _inheritance_graphs(root, {'en': tag})[root / 'structTrait.png']
            base = next(node for node in graph['nodes'] if node['id'].startswith('external:'))
            self.assertEqual(base['label'], 'std::integral_constant<bool, true>')
            self.assertNotIn('href', base)
            self.assertEqual(graph['edges'], [{'from': 'structTrait.html', 'to': base['id']}])

    def test_short_widget_hierarchy_uses_compact_cards_at_full_font_size(self):
        labels = ['portals::Widget', 'portals::SizedWidget', 'portals::Control', 'portals::Button']
        graph = {'title': labels[0], 'nodes': [
            {'id': str(index), 'label': label, 'description': label}
            for index, label in enumerate(labels)],
            'edges': [{'from': str(index), 'to': str(index - 1)} for index in range(1, len(labels))]}
        for palette in PALETTES.values():
            rendered = _inheritance_svg(graph, 'classWidget.html', 'theme-diagrams/turbo/widget.svg', palette)
            svg = ET.fromstring(rendered)
            self.assertLess(float(svg.get('width')), 400)
            cards = [rect for rect in svg.iter(SVG + 'rect') if rect.get('x')]
            self.assertGreater(len({rect.get('width') for rect in cards}), 1)
            self.assertIn('font-size:16px', rendered)
            self.assertEqual(len([path for path in svg.findall(SVG + 'path') if path.get('data-from')]), 3)

    def test_long_template_label_wraps_without_expanding_or_losing_accessible_text(self):
        label = 'portals::Example<' + 'VeryLongTypeName' * 8 + '>'
        graph = {'title': label, 'nodes': [{'id': 'long', 'label': label,
                  'description': label, 'href': 'classLong.html'}], 'edges': []}
        svg = ET.fromstring(_inheritance_svg(graph, 'classLong.html',
                            'theme-diagrams/turbo/long.svg', PALETTES['turbo']))
        self.assertLessEqual(float(svg.get('width')), 500)
        self.assertEqual(svg.find(SVG + 'a').get('aria-label'), label)

    def test_mermaid_caption_wrapping_preserves_words_identifiers_and_connections(self):
        source = ('flowchart TB\n'
                  '  source[Application file export after complete terminal restoration] --> '
                  'target{"Dirty frame<br/>ready to submit?"}\n')
        wrapped = _wrap_mermaid_labels(source)
        self.assertEqual(wrapped.count('-->'), 1)
        self.assertIn('source["Application file export<br/>after complete terminal<br/>restoration"]', wrapped)
        self.assertIn('target{"Dirty frame<br/>ready to submit?"}', wrapped)


if __name__ == '__main__':
    unittest.main()
