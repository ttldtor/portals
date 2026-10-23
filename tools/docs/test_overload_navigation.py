"""Run with unittest discovery in tools/docs; temporary Doxygen fixtures only."""

import html
import json
from pathlib import Path
import tempfile
import unittest

from finalize_html import complete_navigation, read_variable, write_variable
from overload_navigation import group_overloads, short_signature, split_arguments


class OverloadTests(unittest.TestCase):
    def test_conflicting_metadata_preserves_siblings_but_identical_repeats_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            original = [['open', 'api.html#a', None], ['open', 'api.html#b', None],
                        ['close', 'api.html#c', None], ['close', 'api.html#d', None]]
            tree = [['Project', 'index.html', original]]
            (root / 'navtreedata.js').write_text('var NAVTREE = ' + json.dumps(tree) + ';', encoding='utf-8')
            def member(name, anchor, args):
                return (f'<member kind="function"><name>{name}</name><anchorfile>api.html</anchorfile>'
                        f'<anchor>{anchor}</anchor><arglist>{args}</arglist></member>')
            members = member('open', 'a', '(int value)') + member('open', 'b', '(bool value)')
            members += member('close', 'c', '(int value)') * 2 + member('close', 'd', '(bool value)')
            conflicting = member('open', 'a', '(int value)')
            # A third original tuple must not restore a key already marked ambiguous.
            tag = root / 'site.tag'
            tag.write_text('<tagfile><compound><name>generic</name>' + members + '</compound>'
                           '<compound><name>specialized</name>' + conflicting + '</compound>'
                           '<compound><name>generic</name>' + conflicting + '</compound></tagfile>')
            group_overloads(root, tag, read_variable, write_variable)
            rendered = read_variable((root / 'navtreedata.js').read_text(), 'NAVTREE')[0][0][2]
            self.assertEqual(rendered[:2], original[:2])
            self.assertEqual(rendered[2][:2], ['close', None])
            self.assertEqual([child[1] for child in rendered[2][2]], ['api.html#c', 'api.html#d'])

    def test_nested_types_defaults_qualifiers_and_safe_declarator_fallback(self):
        self.assertEqual(short_signature('(Ref< Dialog > reference, Body &&body, Options options={})'),
                         '(Ref<Dialog>, Body &&, Options)')
        self.assertEqual(short_signature('(std::map<int, std::vector<Foo>> values={}, const Foo &value) const &'),
                         '(std::map<int, std::vector<Foo>>, const Foo &) const &')
        self.assertEqual(split_arguments('std::array<int, 2>{1, 2}, "a,b", call(1, 2)'),
                         ['std::array<int, 2>{1, 2}', '"a,b"', 'call(1, 2)'])
        self.assertEqual(short_signature('(void (*callback)(int, int), unsigned long, const Foo)'),
                         '(void (*callback)(int, int), unsigned long, const Foo)')
        self.assertEqual(short_signature('(const volatile Foo, struct Foo, class Bar, enum State)'),
                         '(const volatile Foo, struct Foo, class Bar, enum State)')
        self.assertEqual(short_signature('(const volatile Foo value, struct Foo item)'),
                         '(const volatile Foo, struct Foo)')
        self.assertEqual(short_signature('(int value=call("a,b", 2), bool active=true)'), '(int, bool)')
        self.assertEqual(short_signature('(Text text="(", bool active=true)'), '(Text, bool)')
        self.assertEqual(short_signature('(TextValue title, Body &&body, Options options={})', omit_defaults=True),
                         '(TextValue, Body &&)')
        self.assertEqual(short_signature('(bool value=1<2, bool other=false)'),
                         '(bool value=1<2, bool other=false)')
        self.assertEqual(short_signature('unrecognized'), 'unrecognized')

    def test_lazy_payload_routes_exact_hrefs_other_scopes_and_idempotence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def write(name, value):
                (root / (name + '.js')).write_text('var ' + name + ' = ' + json.dumps(value) + ';', encoding='utf-8')
            write('navtreedata', [])
            (root / 'navtreedata.js').write_text('var NAVTREE = ' + json.dumps([
                ['Project', 'index.html', [['API', 'api.html', 'api'], ['Guide', 'guide.html', None]]]
            ]) + '; var NAVTREEINDEX = [];', encoding='utf-8')
            write('api', [['dialog', 'api.html#a', None], ['variable', 'api.html#v', None],
                          ['dialog', 'api.html#b', None], ['Other scope', 'other.html', 'other'],
                          ['operator()', 'api.html#c', None], ['operator()', 'api.html#d', None],
                          ['compact', 'api.html#e', None], ['compact', 'api.html#f', None],
                          ['collision', 'api.html#g', None], ['collision', 'api.html#h', None]])
            write('other', [['dialog', 'other.html#a', None]])
            write('NAVTREEINDEX0', {'api.html#a': [0, 0], 'api.html#b': [0, 2],
                                   'api.html#v': [0, 1], 'other.html#a': [0, 3, 0],
                                   'api.html#d': [0, 5], 'guide.html': [1]})
            (root / 'NAVTREEINDEX0.js').rename(root / 'navtreeindex0.js')
            members = []
            for name, anchor, args in [('dialog', 'a', '(TextValue title, Body &&body)'),
                                       ('dialog', 'b', '(Ref<Dialog> ref, TextValue title, Body &&body)'),
                                       ('operator()', 'c', '(int value)'), ('operator()', 'd', '(double value)'),
                                       ('compact', 'e', '(TextValue title, Options options={})'),
                                       ('compact', 'f', '(Ref<Dialog> ref, Options options={})'),
                                       ('collision', 'g', '(int value, Options options={})'),
                                       ('collision', 'h', '(int value, OtherOptions options={})')]:
                members.append(f'<member kind="function"><type>Result</type><name>{name}</name>'
                               f'<anchorfile>api.html</anchorfile><anchor>{anchor}</anchor>'
                               f'<arglist>{html.escape(args)}</arglist></member>')
            members.append('<member kind="variable"><name>variable</name><anchorfile>api.html</anchorfile>'
                           '<anchor>v</anchor><arglist>()</arglist></member>')
            tag = root / 'site.tag'
            tag.write_text('<tagfile><compound><name>ns</name>' + ''.join(members) + '</compound></tagfile>')
            complete_navigation(root, {})
            group_overloads(root, tag, read_variable, write_variable)
            api = read_variable((root / 'api.js').read_text(), 'api')[0]
            self.assertEqual(api[0][0:2], ['dialog', None])
            self.assertEqual([child[1] for child in api[0][2]], ['api.html#a', 'api.html#b'])
            self.assertIn('(TextValue, Body &amp;&amp;)', api[0][2][0][0])
            self.assertIn('data-overload-signature="Result ns::dialog(', api[0][2][0][0])
            self.assertEqual(api[1], ['variable', 'api.html#v', None])
            compact = next(node for node in api if node[0] == 'compact')
            self.assertIn('>(TextValue)</span>', compact[2][0][0])
            self.assertIn('>(Ref&lt;Dialog&gt;)</span>', compact[2][1][0])
            self.assertIn('Options options={}', html.unescape(compact[2][0][0]))
            collision = next(node for node in api if node[0] == 'collision')
            self.assertIn('>(int, Options)</span>', collision[2][0][0])
            self.assertIn('>(int, OtherOptions)</span>', collision[2][1][0])
            self.assertIn('OtherOptions options={}', html.unescape(collision[2][1][0]))
            index = read_variable((root / 'navtreeindex0.js').read_text(), 'NAVTREEINDEX0')[0]
            self.assertEqual(index['api.html#a'], [0, 0, 0])
            self.assertEqual(index['api.html#b'], [0, 0, 1])
            self.assertEqual(index['api.html#v'], [0, 1])
            self.assertEqual(index['other.html#a'], [0, 2, 0])
            self.assertEqual(index['api.html#d'], [0, 3, 1])
            self.assertEqual(index['guide.html'], [1])
            before = {path.name: path.read_bytes() for path in root.glob('*.js')}
            # Exercise the actual finalize ordering, not just the grouping helper.
            complete_navigation(root, {})
            group_overloads(root, tag, read_variable, write_variable)
            self.assertEqual(before, {path.name: path.read_bytes() for path in root.glob('*.js')})


if __name__ == '__main__':
    unittest.main()
