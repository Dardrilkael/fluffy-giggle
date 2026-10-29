#!/usr/bin/env python3
"""Build: libstdc++ bounds-checked spice binaries (_GLIBCXX_ASSERTIONS).

The Linux equivalent of MSVC debug's checked iterators
(_ITERATOR_DEBUG_LEVEL=2): every std::vector/std::string operator[] and
iterator dereference bounds-checks and aborts with file:line on an
out-of-bounds access. Catches on Linux what crashes the Windows debug
build while GCC silently reads out of bounds.

Recompiles every in-scope TU from compile_commands.json with
-D_GLIBCXX_ASSERTIONS and relinks via ninja (the define only changes
preprocessing, so ninja's own link step works unchanged).

WARNING: mutates the build tree in place - the checked objects stay until
the next clean rebuild. Run `cmake --build <dir> --target clean` when
done, or dedicate a scratch tree to it.

Needs a GCC/Clang build tree with compile_commands.json (`linux` preset).

Usage:
    python3 tools/build_checked.py [--build-dir build/linux]
                                   [--libs core,math,spice,apps/spice]
"""
import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--build-dir', type=Path, default=Path('build/linux'),
                    help='build tree to instrument IN PLACE '
                         '(default: build/linux)')
    ap.add_argument('--libs', default='core,math,spice,apps/spice',
                    help='comma-separated path fragments selecting TUs')
    args = ap.parse_args()

    build = (ROOT / args.build_dir).resolve()
    cc_path = build / 'compile_commands.json'
    if not cc_path.exists():
        print(f'error: {cc_path} not found - configure first, e.g. '
              f'`cmake --preset linux`', file=sys.stderr)
        return 2
    scope = tuple(f'/{s.strip()}/' for s in args.libs.split(',') if s.strip())

    entries = json.loads(cc_path.read_text())
    targets = [e for e in entries
               if any(s in e['file'] for s in scope)
               and '_deps' not in e['file']
               and '/tests/' not in e['file']]
    gcc = [e for e in targets if 'cl.exe' not in e['command']]
    if len(gcc) != len(targets):
        print(f'error: {len(targets) - len(gcc)} MSVC (cl.exe) entries in '
              f'scope - this tool needs a GCC/Clang build tree (the '
              f'`linux` preset)', file=sys.stderr)
        return 2
    print(f'recompiling {len(gcc)} TUs with -D_GLIBCXX_ASSERTIONS into '
          f'{args.build_dir} (IN PLACE - clean rebuild to restore) ...')
    for e in gcc:
        cmd = shlex.split(e['command'])
        cmd.insert(1, '-D_GLIBCXX_ASSERTIONS')
        r = subprocess.run(cmd, cwd=e['directory'], capture_output=True,
                           text=True)
        if r.returncode != 0:
            print(f'FAILED {e["file"]}:', file=sys.stderr)
            print(r.stderr[-3000:], file=sys.stderr)
            return r.returncode

    for tgt in ('spice', 'test_spice'):
        r = subprocess.run(['ninja', '-C', str(build), tgt],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stderr[-3000:], file=sys.stderr)
            return r.returncode
    print(f'checked binaries ready: {build / "bin/spice"}, '
          f'{build / "bin/test_spice"}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
