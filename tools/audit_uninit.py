#!/usr/bin/env python3
"""Audit: use-of-uninitialized-value (MSVC /RTCu, debug CRT abort -> exit 3).

MSVC debug builds run runtime checks (/RTC1) that abort on reads of
uninitialized stack variables; GCC -Wall at -O0 usually stays silent
because the optimizer never sees the flow. This tool recompiles every
in-scope TU at -O2 with -Wmaybe-uninitialized -Wuninitialized (output to
/dev/null; the build tree is untouched) and reports the findings.

Note: -Wmaybe-uninitialized is optimization-dependent and has false
negatives; it is a cheap screen, not a proof. The ASan/UBSan build
(tools/build_asan.py) is the stronger check.

Needs a GCC/Clang build tree with compile_commands.json - i.e. the
`linux` preset. MSVC (cl.exe) entries are skipped: on that side /RTC1
already covers it.

Usage:
    python3 tools/audit_uninit.py [--build-dir build/linux]
                                  [--libs core,math,spice,apps/spice]
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
    ap.add_argument('--libs', default='core,math,spice,apps/spice',
                    help='comma-separated path fragments selecting TUs')
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
              f'{"y" if skipped == 1 else "ies"} - /RTC1 covers those')
    print(f'scanning {len(gcc)} TUs at -O2 -Wmaybe-uninitialized ...')

    failures = 0
    for e in gcc:
        cmd = shlex.split(e['command'])
        out = []
        skip_next = False
        for a in cmd:
            if skip_next:
                skip_next = False
                continue
            if a in TAKE_VALUE:
                skip_next = True
                if a == '-o':
                    out += ['-o', '/dev/null']
                continue
            if a in DROP_FLAGS or a.startswith(DROP_PREFIXES):
                continue
            if a == '-Werror':
                continue  # unrelated -Wpedantic noise must not stop the scan
            out.append(a)
        out += ['-O2', '-Wmaybe-uninitialized', '-Wuninitialized']
        r = subprocess.run(out, cwd=e['directory'], capture_output=True,
                           text=True)
        findings = [ln for ln in (r.stderr + r.stdout).splitlines()
                    if 'uninitiali' in ln.lower()]
        if r.returncode != 0 or findings:
            failures += 1
            print(f'\n=== {Path(e["file"]).name} (rc={r.returncode}) ===')
            for ln in findings:
                print('  ' + ln)
    if failures == 0:
        print('OK: no uninitialized-use findings')
    else:
        print(f'\n{failures} TU(s) with findings')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
