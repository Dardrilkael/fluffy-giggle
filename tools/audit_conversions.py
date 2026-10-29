#!/usr/bin/env python3
"""Audit: implicit conversion warnings (MSVC C4244/C4245/C4267/C4389, /W4).

MSVC /W4 flags lossy and sign-flipping implicit conversions (double->float,
size_t->int, signed/unsigned mixes) and treats them as errors under /WX;
the workspace's GCC flags (-Wall -Wextra -Wpedantic -Werror) do NOT include
-Wconversion or -Wsign-conversion, so Linux builds stay silent. This tool
recompiles every in-scope TU from compile_commands.json with both flags
appended (output to /dev/null; the build tree is untouched) and reports
the findings.

Needs a GCC/Clang build tree with compile_commands.json - i.e. the
`linux` preset. MSVC (cl.exe) entries are skipped: on that side /W4
already covers it.

Usage:
    python3 tools/audit_conversions.py [--build-dir build/linux]
                                       [--libs libs,apps]
"""
import argparse
import json
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DROP_FLAGS = ('-MD', '-MMD', '-fmodules-ts', '-g')
DROP_PREFIXES = ('-fmodule-mapper=', '-fdeps-format=')
TAKE_VALUE = ('-o', '-MF', '-MT')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--build-dir', type=Path, default=Path('build/linux'),
                    help='build tree with compile_commands.json '
                         '(default: build/linux)')
    ap.add_argument('--libs', default='libs,apps',
                    help='comma-separated path fragments selecting TUs '
                         '(default: every first-party TU)')
    args = ap.parse_args()

    cc_path = ROOT / args.build_dir / 'compile_commands.json'
    if not cc_path.exists():
        print(f'error: {cc_path} not found - configure first, e.g. '
              f'`cmake --preset linux`', file=sys.stderr)
        return 2
    scope = tuple(f'/{s.strip()}/' for s in args.libs.split(',') if s.strip())

    entries = json.loads(cc_path.read_text())
    targets = [e for e in entries
               if any(s in e['file'] for s in scope)
               and '_deps' not in e['file']]
    gcc = [e for e in targets if 'cl.exe' not in e['command']]
    skipped = len(targets) - len(gcc)
    if skipped:
        print(f'note: skipped {skipped} MSVC (cl.exe) entr'
              f'{"y" if skipped == 1 else "ies"} - /W4 covers those')
    print(f'scanning {len(gcc)} TUs with -Wconversion -Wsign-conversion ...')

    failures = 0
    for e in gcc:
        cmd = shlex.split(e['command'])
        out = []
        skip_next = False
        for a in cmd:
            if skip_next:
                skip_next = False
                if a in TAKE_VALUE and a == '-o':
                    out += ['-o', '/dev/null']
                continue
            if a in TAKE_VALUE:
                skip_next = True
                if a == '-o':
                    out += ['-o', '/dev/null']
                continue
            if a in DROP_FLAGS or a.startswith(DROP_PREFIXES):
                continue
            out.append(a)
        out += ['-Wconversion', '-Wsign-conversion']
        r = subprocess.run(out, cwd=e['directory'], capture_output=True,
                           text=True)
        findings = [ln for ln in (r.stderr + r.stdout).splitlines()
                    if 'warning:' in ln or '[-Werror=conversion' in ln
                    or '[-Werror=sign-conversion' in ln]
        if r.returncode != 0 or findings:
            failures += 1
            print(f'\n=== {Path(e["file"]).name} (rc={r.returncode}) ===')
            for ln in findings:
                print('  ' + ln)
    if failures == 0:
        print('OK: no conversion findings in scope')
    else:
        print(f'\n{failures} TU(s) with findings')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
