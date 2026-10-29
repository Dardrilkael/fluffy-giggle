#!/usr/bin/env python3
"""Build: ASan + UBSan instrumented spice binaries (the strong UB check).

Recompiles every in-scope TU from compile_commands.json with
-fsanitize=address,undefined -fno-sanitize-recover=all and relinks the
spice app (ninja's archive + link steps are replayed with the sanitizer
runtime injected after the compiler token - ninja's ": && ... && :"
chains would otherwise swallow flags appended at the end).

What it catches that the plain build misses: heap/stack use-after-free,
out-of-bounds indexing, assignment through disengaged optionals, bad
casts, misaligned loads, signed overflow. This is the tool that found
the .dc empty-optional UB (commit b265ae7) after MSVC debug aborted with
exit code 3 and Linux ran "fine".

WARNING: mutates the build tree in place - the instrumented objects stay
until the next clean rebuild. Recommended: dedicate a scratch tree
(`cmake --preset linux -B build/linux-audit` style) or run
`cmake --build <dir> --target clean` when done testing.

Needs a GCC/Clang build tree with compile_commands.json (`linux` preset).
Leaks are reported at exit by default; set ASAN_OPTIONS=detect_leaks=0
to focus purely on hard errors.

Usage:
    python3 tools/build_asan.py [--build-dir build/linux]
                                [--libs core,math,spice,apps/spice]
                                [--target spice]
"""
import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SAN = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
       '-fno-omit-frame-pointer']
DROP_FLAGS = ('-fmodules-ts', '-MD')
DROP_PREFIXES = ('-fmodule-mapper=', '-fdeps-format=')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--build-dir', type=Path, default=Path('build/linux'),
                    help='build tree to instrument IN PLACE '
                         '(default: build/linux)')
    ap.add_argument('--libs', default='core,math,spice,apps/spice',
                    help='comma-separated path fragments selecting TUs')
    ap.add_argument('--target', default='spice',
                    help='executable target to relink (default: spice)')
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
    print(f'recompiling {len(gcc)} TUs with ASan+UBSan into {args.build_dir} '
          f'(IN PLACE - clean rebuild to restore) ...')
    for e in gcc:
        cmd = shlex.split(e['command'])
        cmd = [a for a in cmd
               if a not in DROP_FLAGS and not a.startswith(DROP_PREFIXES)]
        cmd[1:1] = SAN
        r = subprocess.run(cmd, cwd=e['directory'], capture_output=True,
                           text=True)
        if r.returncode != 0:
            print(f'FAILED {e["file"]}:', file=sys.stderr)
            print(r.stderr[-3000:], file=sys.stderr)
            return r.returncode

    # replay ninja's archive + link steps; skip the compile lines (done
    # above, instrumented) and inject the sanitizer runtime right after
    # the COMPILER TOKEN inside ninja's ": && c++ ... && :" chains.
    # Only real link lines (they carry " -o "); archive/dyndep steps run
    # untouched - a loose "ends with cc" regex once matched a fragment
    # inside a cmake_ninja_dyndep invocation and broke the step.
    def inject(line: str) -> str:
        if ' -o ' not in line:
            return line
        for tok in line.split():
            if Path(tok).name in ('c++', 'g++', 'clang++'):
                return line.replace(tok, tok + ' '
                                    + ' '.join(shlex.quote(s) for s in SAN),
                                    1)
        return line

    r = subprocess.run(['ninja', '-C', str(build), '-t', 'commands',
                        args.target], capture_output=True, text=True,
                       check=True)
    for line in r.stdout.splitlines():
        if ' -c ' in line or not line.strip():
            continue
        cmd = inject(line)
        print('step:', cmd[:110])
        r2 = subprocess.run(['bash', '-c', cmd], cwd=build,
                            capture_output=True, text=True)
        if r2.returncode != 0:
            print(r2.stderr[-3000:], file=sys.stderr)
            return r2.returncode
    print(f'sanitized binary ready: {build / "bin" / args.target}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
