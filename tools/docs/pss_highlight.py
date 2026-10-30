"""Highlight plain PSS v0 fragments for finalize_html.finalize.

Input and output are HTML strings; no I/O or dependencies are required. Only
complete supported PSS documents in unmarked Doxygen line wrappers are colored.
Unknown syntax and existing markup pass through unchanged, without errors.
The grammar follows include/portals/detail/pss_parser.hpp; this is a conservative
presentation recognizer, not a replacement for the runtime parser.
"""

import html
import re


_IDENTIFIER = r"[A-Za-z_][A-Za-z_0-9-]*"
_COMMENT = r"/\*.*?\*/"
_SPACE = rf"(?:[ \t\r\n\f\v]|{_COMMENT})*"
_SELECTOR = rf"(?:\*|{_IDENTIFIER})?(?:[.#:]{_IDENTIFIER})*"
_RULE = re.compile(
    rf"(?P<selector>{_SELECTOR}){_SPACE}\{{(?P<body>(?:{_COMMENT}|[^{{}}/]|/(?!\*))*)\}}",
    re.S,
)
_DECLARATION = re.compile(
    rf"(?P<property>{_IDENTIFIER}){_SPACE}:{_SPACE}"
    rf'''(?P<value>"[^"\r\n]*"|'[^'\r\n]*'|[^;}}\s]+){_SPACE};''', re.S,
)
_TRIVIA = re.compile(_SPACE, re.S)
_STATES = {"normal", "focused", "disabled", "selected", "header", "caret"}
_GLYPH_PARTS = {"track", "thumb", "start", "end", "backward", "forward",
                "thumb-start", "thumb-body", "thumb-end", "thumb-single"}
_GLYPH_PROPERTIES = {f"scrollbar-{axis}-{part}-glyph"
                     for axis in ("horizontal", "vertical") for part in _GLYPH_PARTS}
_GLYPH_PROPERTIES.add("scrollbar-corner-glyph")


def _tokens(source):
    """Return non-overlapping (start, end, CSS class) ranges, or None."""
    tokens = []
    cursor = 0
    rules = 0
    while True:
        cursor = _TRIVIA.match(source, cursor).end()
        if cursor == len(source):
            break
        rule = _RULE.match(source, cursor)
        if not rule or not rule["selector"]:
            return None
        selector = rule["selector"]
        predicates = re.findall(rf"([.#:])({_IDENTIFIER})", selector)
        ids = [name for marker, name in predicates if marker == "#"]
        states = [name for marker, name in predicates if marker == ":"]
        if len(ids) > 1 or len(states) > 1 or any(state not in _STATES for state in states):
            return None
        for token in re.finditer(rf"[.#:]?{_IDENTIFIER}|\*", selector):
            kind = "keywordtype" if token[0][0] not in ".#:" else "preprocessor"
            tokens.append((rule.start("selector") + token.start(),
                           rule.start("selector") + token.end(), kind))
        body_cursor, body_end = rule.start("body"), rule.end("body")
        while True:
            body_cursor = _TRIVIA.match(source, body_cursor).end()
            if body_cursor == body_end:
                break
            declaration = _DECLARATION.match(source, body_cursor, body_end)
            if not declaration:
                return None
            name, value = declaration["property"], declaration["value"]
            if name in {"foreground", "background"}:
                valid = re.fullmatch(r"#[0-9A-Fa-f]{6}", value)
                kind = "stringliteral"
            elif name in _GLYPH_PROPERTIES:
                # Only recognize the quoted syntax here; the receiving WidthPolicy
                # performs grapheme/cell admission when applying the stylesheet.
                valid = (len(value) >= 2 and value[0] in "\"'" and value[-1] == value[0]
                         and len(value[1:-1].encode("utf-8")) <= 64)
                kind = "stringliteral"
            elif name == "min-width":
                digits = value.removeprefix("-")
                significant_digits = digits.lstrip("0") or "0"
                valid = (not states and re.fullmatch(r"[0-9]+", digits)
                         and len(significant_digits) <= 10
                         and int(significant_digits) <= 2147483647
                         and (not value.startswith("-") or significant_digits == "0"))
                kind = "stringliteral"
            else:
                valid = name in {"bold", "italic", "underline"} and value in {"true", "false"}
                kind = "keyword"
            if not valid:
                return None
            tokens.extend([(declaration.start("property"), declaration.end("property"), "keyword"),
                           (declaration.start("value"), declaration.end("value"), kind)])
            body_cursor = declaration.end()
        rules += 1
        cursor = rule.end()
    if not rules:
        return None
    tokens.extend((match.start(), match.end(), "comment")
                  for match in re.finditer(_COMMENT, source, re.S))
    return sorted(tokens)


def color_pss(content):
    """Color recognized plain fragments, preserving decoded text and wrappers."""
    def fragment(match):
        body = match[1]
        lines = list(re.finditer(r'<div class="line">([^<]*)</div>', body))
        # Reject anchors, spans, copy controls, and unknown wrapper structures.
        if not lines or re.sub(r'<div class="line">[^<]*</div>', '', body).strip():
            return match[0]
        values = [html.unescape(line[1]) for line in lines]
        source = "\n".join(values)
        tokens = _tokens(source)
        if tokens is None:
            return match[0]
        offset = 0
        rendered = []
        for value in values:
            end = offset + len(value)
            parts, previous = [], offset
            for start, stop, kind in tokens:
                start, stop = max(start, offset), min(stop, end)
                if start >= stop:
                    continue
                parts.append(html.escape(source[previous:start]))
                parts.append(f'<span class="{kind}">{html.escape(source[start:stop])}</span>')
                previous = stop
            parts.append(html.escape(source[previous:end]))
            rendered.append('<div class="line">' + ''.join(parts) + '</div>')
            offset = end + 1
        replacements = iter(rendered)
        body = re.sub(r'<div class="line">[^<]*</div>', lambda _: next(replacements), body)
        return '<div class="fragment pss-colors">' + body + '</div>'

    return re.sub(r'<div class="fragment">((?:<div class="line">[^\n]*</div>\s*)+)</div>',
                  fragment, content)
