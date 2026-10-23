"""Group Doxygen function siblings after finalize_html completes guide navigation.

The caller supplies JSON variable readers/writers and the matching tag XML.
Only generated navigation payloads and their index paths are rewritten in place;
missing or ambiguous function metadata leaves the original entries intact.
"""

import html
import re
import xml.etree.ElementTree as ET


def _split(value, delimiter):
    """Split C++ argument text at top-level commas; retain nested defaults."""
    parts, start, stack, quote, escaped = [], 0, [], None, False
    pairs = {')': '(', ']': '[', '}': '{', '>': '<'}
    for index, char in enumerate(value):
        if quote:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == quote:
                quote = None
            continue
        if char in "\"'":
            quote = char
        elif char in '([{<':
            stack.append(char)
        elif char in pairs and stack and stack[-1] == pairs[char]:
            stack.pop()
        elif char == delimiter and not stack:
            parts.append(value[start:index].strip())
            start = index + 1
    if stack or quote:
        return None
    parts.append(value[start:].strip())
    return parts


def split_arguments(value):
    """Keep unfamiliar or unbalanced declarators intact as a safe fallback."""
    return _split(value, ',') or [value]


def short_signature(arglist, omit_defaults=False):
    """Prefer parameter types; unfamiliar declarators keep their source text."""
    if not arglist.startswith('('):
        return arglist
    # The final parenthesis may be followed by cv/ref qualifiers or requires.
    depth, end, quote, escaped = 0, None, None, False
    for index, char in enumerate(arglist):
        if quote:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == quote:
                quote = None
        elif char in "\"'":
            quote = char
        elif char == '(':
            depth += 1
        elif char == ')':
            depth -= 1
            if depth == 0:
                end = index
                break
    if end is None:
        return arglist
    parameters = _split(arglist[1:end], ',')
    if parameters is None:
        return arglist
    arguments = []
    for parameter in parameters:
        # A default begins at an equals sign outside a nested declarator.
        default_parts = _split(parameter, '=')
        if default_parts is None:
            return arglist
        if omit_defaults and len(default_parts) > 1:
            continue
        parameter = default_parts[0] if len(default_parts) > 1 else parameter
        if not any(char in parameter for char in '()[]'):
            match = re.match(r'^(.*[\s&*])([A-Za-z_]\w*)$', parameter)
            keywords = {'const', 'volatile', 'unsigned', 'signed', 'long', 'short',
                        'int', 'char', 'bool', 'float', 'double', 'void', 'auto'}
            type_prefixes = {'const', 'volatile', 'class', 'struct', 'enum', 'typename'}
            prefix_only = match and all(word in type_prefixes for word in match[1].split())
            if match and match[2] not in keywords and not prefix_only:
                parameter = match[1].strip()
        parameter = re.sub(r'\s*([<>,])\s*', r'\1', parameter)
        parameter = re.sub(r',(?=\S)', ', ', parameter)
        arguments.append(parameter)
    return '(' + ', '.join(arguments) + ')' + arglist[end + 1:]


def group_overloads(directory, tag_file, read_variable, write_variable):
    """Preserve each href and remap index routes through newly inserted groups."""
    metadata = {}
    ambiguous = set()
    for compound in ET.parse(tag_file).getroot().findall('compound'):
        scope = compound.findtext('name') or ''
        for member in compound.findall("member[@kind='function']"):
            name = member.findtext('name')
            filename, anchor = member.findtext('anchorfile'), member.findtext('anchor')
            args = member.findtext('arglist')
            if name and filename and anchor and args:
                target = filename + '#' + anchor
                info = (scope, name, args, member.findtext('type') or '')
                if target in ambiguous:
                    continue
                if target in metadata and metadata[target] != info:
                    del metadata[target]
                    ambiguous.add(target)
                else:
                    metadata[target] = info

    payloads, routes = {}, {}

    def load(reference):
        if not isinstance(reference, str):
            return reference
        if reference in payloads:
            return payloads[reference][2]
        path = (directory / (reference + '.js')).resolve()
        if not path.is_relative_to(directory.resolve()) or not path.is_file():
            return None
        variable = path.stem.replace('-', '_')
        if variable[0].isdigit():
            variable = '_' + variable
        text = path.read_text(encoding='utf-8')
        value, _ = read_variable(text, variable)
        payloads[reference] = (path, variable, value, text)
        transform(value)
        return value

    def transform(nodes):
        if not isinstance(nodes, list) or id(nodes) in routes:
            return
        original = list(nodes)
        routes[id(nodes)] = (original, {})
        groups = {}
        for index, node in enumerate(original):
            info = metadata.get(node[1])
            if info and node[0] == info[1] and node[2] is None:
                groups.setdefault(info[:2], []).append(index)
        groups = {key: indexes for key, indexes in groups.items() if len(indexes) > 1}
        eligible = {}
        for key, indexes in groups.items():
            args = [metadata[original[index][1]][2] for index in indexes]
            labels = [short_signature(arglist, omit_defaults=True) for arglist in args]
            if len(set(labels)) != len(labels):
                labels = [short_signature(arglist) for arglist in args]
            if len(set(labels)) != len(labels):
                labels = [metadata[original[index][1]][2] for index in indexes]
            if len(set(labels)) != len(labels):
                labels = [metadata[original[index][1]][3] + ' ' + label
                          for index, label in zip(indexes, labels)]
            if len(set(labels)) == len(labels):
                eligible[key] = (indexes, labels)
        nodes[:] = []
        for index, node in enumerate(original):
            info = metadata.get(node[1])
            key = info[:2] if info and node[0] == info[1] and node[2] is None else None
            if key in eligible:
                indexes, labels = eligible[key]
                if index != indexes[0]:
                    continue
                children = []
                for child_index, (old_index, label) in enumerate(zip(indexes, labels)):
                    old = original[old_index]
                    scope, name, args, result = metadata[old[1]]
                    full = (result + ' ' + scope + '::' + name + args).strip()
                    # Doxygen's newNode renders label markup inside its anchor.
                    title = html.escape(full, quote=True)
                    children.append([f'<span data-overload-signature="{title}">{html.escape(label)}</span>', old[1], None])
                    routes[id(nodes)][1][old_index] = [len(nodes), child_index]
                nodes.append([node[0], None, children])
            else:
                routes[id(nodes)][1][index] = [len(nodes)]
                nodes.append(node)
                children = load(node[2])
                if isinstance(children, list):
                    transform(children)

    tree_file = directory / 'navtreedata.js'
    text = tree_file.read_text(encoding='utf-8')
    tree, _ = read_variable(text, 'NAVTREE')
    # The outer project node is deliberately omitted from NAVTREEINDEX routes.
    transform(tree[0][2])
    remap = {}

    def visit(nodes, old_path, new_path, active):
        if not isinstance(nodes, list) or id(nodes) in active:
            return
        original, positions = routes[id(nodes)]
        for index, node in enumerate(original):
            old = (*old_path, index)
            new = (*new_path, *positions[index])
            remap[old] = list(new)
            children = load(node[2])
            if isinstance(children, list):
                visit(children, old, new, active | {id(nodes)})

    visit(tree[0][2], (), (), set())
    tree_file.write_text(write_variable(text, 'NAVTREE', tree), encoding='utf-8')
    for path, variable, value, original in payloads.values():
        path.write_text(write_variable(original, variable, value), encoding='utf-8')
    for path in directory.glob('navtreeindex[0-9]*.js'):
        original = path.read_text(encoding='utf-8')
        variable = path.stem.upper()
        indexes, _ = read_variable(original, variable)
        indexes = {key: remap.get(tuple(route), route) for key, route in indexes.items()}
        path.write_text(write_variable(original, variable, indexes), encoding='utf-8')
