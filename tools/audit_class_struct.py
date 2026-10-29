#!/usr/bin/env python3
"""Audit: class/struct key mismatches (MSVC C4099, /W4 /WX fatal).

A type forward-declared `class X;` but defined `struct X { ... }` (or the
reverse, or an elaborated specifier `const class X&` disagreeing with the
definition) compiles silently under GCC/Clang but is a hard error under
MSVC /W4 /WX. This scanner walks every (class|struct) NAME occurrence in
libs/ + apps/ (comments and string literals stripped) and reports any
name seen with both keys.

Qualified definitions are attributed to the LAST name: `struct Foo::Impl`
records `Impl`, not `Foo` - the class-key applies to the inner name. That
kills the pimpl false positives (`class EventLoop` + `struct
EventLoop::Impl` is consistent). What remains is same-spelling-different-
class collisions (unrelated `Impl` members of different classes), so `Impl`
is on the default ignore list; pass --all to see everything.

Pure Python, no build tree needed - runs on any checkout.

Usage:
    python3 tools/audit_class_struct.py [--all] [--root PATH]
"""
import argparse
import re
import sys
from pathlib import Path

NAME_RE = re.compile(r'\b(class|struct)\s+([A-Za-z_]\w{2,})')
CHAIN_RE = re.compile(r'\s*::\s*([A-Za-z_]\w*)')
DEFAULT_IGNORE = ('Impl',)  # pimpl idiom: per-class, unrelated spellings
SKIP_DIRS = {'build', '.git', '_deps', 'node_modules'}


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments, preserving string/char literals."""
    out = []
    i, n = 0, len(text)
    in_line = in_block = in_str = in_char = False
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ''
        if in_line:
            if c == '\n':
                in_line = False
                out.append(c)
            i += 1
            continue
        if in_block:
            if c == '*' and nxt == '/':
                in_block = False
                i += 2
                continue
            i += 1
            continue
        if in_str:
            out.append(c)
            if c == '\\':
                out.append(nxt)
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if in_char:
            out.append(c)
            if c == '\\':
                out.append(nxt)
                i += 2
                continue
            if c == "'":
                in_char = False
            i += 1
            continue
        if c == '/' and nxt == '/':
            in_line = True
            i += 2
            continue
        if c == '/' and nxt == '*':
            in_block = True
            i += 2
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "'":
            in_char = True
            out.append(c)
            i += 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def scan(root: Path, show_ignored: bool, ignore: tuple) -> int:
    occurrences = {}  # name -> {(key, relpath, line, tag)}
    for path in sorted(root.rglob('*.[hc]pp')):
        rel = path.relative_to(root)
        if any(part in SKIP_DIRS for part in rel.parts):
            continue
        try:
            text = strip_comments(
                path.read_text(encoding='utf-8', errors='replace'))
        except OSError:
            continue
        for m in NAME_RE.finditer(text):
            key, name = m.group(1), m.group(2)
            # template parameter lists: `template <class T>` - skip the
            # whole match if the next non-space char opens a param list
            # right after a template header. Cheap approximation: names
            # of a single char are skipped by the regex already.
            tail = text[m.end():m.end() + 1]
            # qualified chain: attribute the key to the LAST identifier
            end = m.end()
            final = name
            cm = CHAIN_RE.match(text, end)
            while cm:
                final = cm.group(1)
                end = cm.start(1) + len(cm.group(1))
                cm = CHAIN_RE.match(text, end)
            # classify: fwd decl / definition / elaborated-in-signature
            after = text[end:end + 1]
            if after == '{' or (after == ':' and text[end:end + 2] != '::'):
                tag = 'def'
            elif after == ';':
                tag = 'fwd'
            else:
                tag = 'arg'  # e.g. `const class X&` in a parameter list
            line = text.count('\n', 0, m.start()) + 1
            occurrences.setdefault(final, set()).add((key, str(rel), line, tag))

    mismatches = 0
    for name, occs in sorted(occurrences.items()):
        keys = {k for k, *_ in occs}
        if len(keys) < 2:
            continue
        if not show_ignored and name in ignore:
            print(f'ignored  {name} (same spelling, different classes; '
                  f'use --all to show)')
            continue
        mismatches += 1
        print(f'MISMATCH  {name}:')
        for key, rel, line, tag in sorted(occs, key=lambda x: (x[1], x[2])):
            print(f'  {key:6s} {rel}:{line}  ({tag})')
    if mismatches == 0:
        print('OK: no class/struct key mismatches found')
    else:
        print(f'\n{mismatches} mismatched type name(s)')
    return 1 if mismatches else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--all', action='store_true',
                    help='do not ignore same-spelling names like Impl')
    ap.add_argument('--root', type=Path,
                    default=Path(__file__).resolve().parents[1],
                    help='repo root (default: the checkout this file lives in)')
    args = ap.parse_args()
    return scan(args.root, args.all, DEFAULT_IGNORE)


if __name__ == '__main__':
    sys.exit(main())
